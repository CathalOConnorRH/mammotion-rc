// Heltec V3 "mower" role (env:v3-mower): LoRa ↔ BLE central.
//
// The LoRa equivalent of tcp_proxy.cpp.  Instead of a TCP client arriving and
// leaving, the PC-side lora_bridge.py sends OPEN / CLOSE over the link:
//
//   OPEN     → connect_mower(), reply STATUS(BLE_UP | BLE_OPEN_FAILED)
//   DATA_TX  → one GATT write (lossless retry, same as tcp_proxy.cpp)
//   notify   → DATA_RX
//   CLOSE    → disconnect_mower(), reply STATUS(BLE_CLOSED)
//
// Anything that breaks the BluFi sequence (an abandoned LoRa message, a queue
// overflow, BLE dropping) closes the BLE session and reports STATUS(BLE_CLOSED);
// the bridge then drops its TCP client so PyMammotion reconnects and resyncs —
// the same recovery the HC33 gets from a TCP drop.
//
// Dead-man: no frame from the PC for LINK_IDLE_TIMEOUT_MS → drop BLE.

#include <Arduino.h>
#include <esp_log.h>
#include <deque>
#include <queue>
#include <vector>
#include "ble_central.h"
#include "lora_link.h"
#include "log.h"
#include "config.h"

using namespace lora;

static const char* TAG = "v3_mower";

static BleCentral ble;
static LoraLink   link_(ROLE_MOWER);

static bool     ble_open    = false;
static uint32_t last_pc_rx_ms = 0;

// GATT writes waiting for the BLE controller (held, never dropped — see
// BLE_WRITE_* in config.h).
static constexpr size_t WRITE_QUEUE_MAX = 16;
static std::deque<std::vector<uint8_t>> write_queue;
static uint32_t write_pending_since_ms = 0;
static bool     write_stalled = false;

// Notifications pushed from the NimBLE host task, drained in loop().
static SemaphoreHandle_t                notify_mutex = nullptr;
static std::queue<std::vector<uint8_t>> notify_queue;

static void send_status(BleState s) {
    const uint8_t b = s;
    if (!link_.send(BLE_STATUS, &b, 1)) ESP_LOGW(TAG, "STATUS %u not queued", b);
}

static void clear_notify_queue() {
    xSemaphoreTake(notify_mutex, portMAX_DELAY);
    std::queue<std::vector<uint8_t>> empty;
    notify_queue.swap(empty);
    xSemaphoreGive(notify_mutex);
}

static void close_session(const char* why) {
    if (ble_open) ESP_LOGI(TAG, "closing BLE session: %s", why);
    ble.disconnect_mower();
    ble_open = false;
    write_queue.clear();
    write_stalled = false;
    clear_notify_queue();
    link_.clear_queue();           // stale DATA_RX must not leak into the next session
    send_status(BLE_CLOSED);
}

static void open_session() {
    if (ble_open) close_session("re-OPEN");
    ESP_LOGI(TAG, "OPEN — connecting to mower");
    write_queue.clear();
    clear_notify_queue();
    // Own the BLE state before connecting so the failure path also tears down
    // any half-built NimBLE client (see tcp_proxy.cpp on_client_connected_).
    ble_open = true;
    // Blocks for up to scan + connect timeouts.  The bridge sends nothing
    // until our STATUS reply, so the radio can sit unattended meanwhile.
    if (!ble.connect_mower()) {
        ESP_LOGE(TAG, "BLE connect failed");
        ble.disconnect_mower();
        ble_open = false;
        send_status(BLE_OPEN_FAILED);
        return;
    }
    last_pc_rx_ms = millis();
    ESP_LOGI(TAG, "BLE up — piping frames");
    send_status(BLE_UP);
}

static void on_link_rx(MsgType type, const uint8_t* data, size_t len) {
    switch (type) {
    case OPEN:
        last_pc_rx_ms = millis();
        open_session();
        break;
    case CLOSE:
        ESP_LOGI(TAG, "CLOSE from PC");
        close_session("PC closed");
        break;
    case DATA_TX:
        if (!ble_open) {
            ESP_LOGW(TAG, "DATA_TX with no BLE session — telling PC");
            send_status(BLE_CLOSED);
            break;
        }
        last_pc_rx_ms = millis();
        if (write_queue.size() >= WRITE_QUEUE_MAX) {
            close_session("GATT write queue overflow");
            break;
        }
        if (write_queue.empty()) write_pending_since_ms = millis();
        write_queue.emplace_back(data, data + len);
        break;
    case PING: {
        // Echo with our view of the link so the bridge sees both directions.
        const Stats& st = link_.stats();
        const int16_t rssi = (int16_t)(st.last_rssi * 10);
        const int16_t snr  = (int16_t)(st.last_snr * 10);
        std::vector<uint8_t> pong = {
            (uint8_t)(rssi >> 8), (uint8_t)rssi, (uint8_t)(snr >> 8), (uint8_t)snr,
        };
        const size_t echo = std::min(len, (size_t)MAX_FRAME_LEN - pong.size());
        pong.insert(pong.end(), data, data + echo);
        link_.send(PONG, pong.data(), pong.size());
        break;
    }
    default:
        ESP_LOGW(TAG, "unexpected message type 0x%02x", type);
        break;
    }
}

static void on_link_fail(MsgType type) {
    // A DATA_RX (or anything queued during the session) never made it — the
    // PC's BluFi sequence now has a gap, so rebuild the session.
    if (ble_open) close_session("LoRa delivery failed");
    (void)type;
}

// Write the front of write_queue to GATT; same lossless retry policy as
// TcpProxy::try_write_pending_().
static void pump_writes() {
    if (write_queue.empty()) return;
    if (!ble.is_connected()) {
        close_session("BLE link down while writing");
        return;
    }
    const std::vector<uint8_t>& frame = write_queue.front();
    for (int attempt = 0; attempt < BLE_WRITE_RETRY_BURST; attempt++) {
        if (ble.write(frame.data(), frame.size())) {
            if (write_stalled) {
                ESP_LOGI(TAG, "BLE write recovered after %ums backpressure",
                         (unsigned)(millis() - write_pending_since_ms));
                write_stalled = false;
            }
            write_queue.pop_front();
            write_pending_since_ms = millis();
            return;
        }
        delay(BLE_WRITE_RETRY_DELAY_MS);
    }
    const uint32_t stalled_for = millis() - write_pending_since_ms;
    if (!write_stalled && stalled_for > BLE_WRITE_STALL_LOG_MS) {
        ESP_LOGW(TAG, "BLE write backpressured %ums — holding frame, not dropping",
                 (unsigned)stalled_for);
        write_stalled = true;
    }
}

static void drain_notifies() {
    while (link_.queued() < LORA_TX_QUEUE_MAX) {
        std::vector<uint8_t> frame;
        xSemaphoreTake(notify_mutex, portMAX_DELAY);
        if (notify_queue.empty()) { xSemaphoreGive(notify_mutex); return; }
        frame = std::move(notify_queue.front());
        notify_queue.pop();
        const size_t backlog = notify_queue.size();
        xSemaphoreGive(notify_mutex);
        if (!ble_open) continue;
        if (!link_.send(DATA_RX, frame.data(), frame.size())) {
            close_session("DATA_RX rejected by link");
            return;
        }
        // The radio can't keep up and the backlog is unbounded otherwise.
        if (backlog > 4 * LORA_TX_QUEUE_MAX) {
            close_session("notify backlog overflow");
            return;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    ESP_LOGI(TAG, "mammotion-rc LoRa mower node");

    notify_mutex = xSemaphoreCreateMutex();
    ble.set_notify_callback([](const uint8_t* data, size_t len) {
        xSemaphoreTake(notify_mutex, portMAX_DELAY);
        notify_queue.emplace(data, data + len);
        xSemaphoreGive(notify_mutex);
    });
    if (!ble.begin()) ESP_LOGE(TAG, "BLE init failed");

    link_.set_rx_callback(on_link_rx);
    link_.set_fail_callback(on_link_fail);
    if (!link_.begin()) {
        ESP_LOGE(TAG, "LoRa init failed — halting");
        while (true) delay(1000);
    }
}

void loop() {
    link_.loop();

    if (ble_open) {
        if (!ble.is_connected() && write_queue.empty()) {
            close_session("BLE dropped");
        } else if ((uint32_t)(millis() - last_pc_rx_ms) > LINK_IDLE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "no frame from PC in %u ms — dropping BLE (dead-man)",
                     (unsigned)LINK_IDLE_TIMEOUT_MS);
            close_session("dead-man timeout");
        } else {
            pump_writes();
            drain_notifies();
        }
    }
    delay(1);
}
