// Heltec V3 "base" role (env:v3-base): USB serial ↔ LoRa.
//
// Plugged into the PC running web-server/lora_bridge.py.  Serial frames
// (both directions):
//
//   [0xA5][0x5A][type 1B][len 2B BE][payload][crc16 2B BE]
//
// crc16 is CRC-16/CCITT-FALSE over type+len+payload.  The magic + CRC let the
// bridge resync around log text, which shares the same UART (ESP_LOGx → Serial).
//
// PC → base:  DATA_TX / OPEN / CLOSE / PING    forwarded over LoRa
//             HELLO                            → INFO
// base → PC:  DATA_RX / STATUS / PONG          forwarded from the mower
//             SENT [type]     a PC message was fully ACKed by the mower
//             FAILED [type]   a PC message was abandoned after all retries
//             BUSY [type]     LoRa TX queue full, message NOT accepted
//             STATS           link counters, every LORA_STATS_INTERVAL_MS
//             INFO            on boot and in reply to HELLO
//
// The bridge sends one message at a time and waits for SENT / FAILED / BUSY,
// so the serial side never outruns the radio.

#include <Arduino.h>
#include <esp_log.h>
#include <vector>
#include "lora_link.h"
#include "log.h"
#include "config.h"

using namespace lora;

static const char* TAG = "v3_base";

enum SerialType : uint8_t {
    S_HELLO  = 0x20,
    S_SENT   = 0x21,
    S_FAILED = 0x22,
    S_BUSY   = 0x23,
    S_STATS  = 0x24,
    S_INFO   = 0x25,
};

static constexpr uint8_t  MAGIC0 = 0xA5, MAGIC1 = 0x5A;
static constexpr uint8_t  SERIAL_PROTO_VER = 1;
static constexpr uint32_t SERIAL_FRAME_GAP_MS = 200;   // abandon a half-read frame after this

static LoraLink link_(ROLE_BASE);

static uint16_t crc16(const uint8_t* d, size_t n, uint16_t crc = 0xFFFF) {
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

// One Serial.write per frame so a frame is never split by log output.
static void serial_send(uint8_t type, const uint8_t* data, size_t len) {
    std::vector<uint8_t> f;
    f.reserve(len + 7);
    f.push_back(MAGIC0);
    f.push_back(MAGIC1);
    f.push_back(type);
    f.push_back((uint8_t)(len >> 8));
    f.push_back((uint8_t)len);
    f.insert(f.end(), data, data + len);
    const uint16_t crc = crc16(f.data() + 2, len + 3);
    f.push_back((uint8_t)(crc >> 8));
    f.push_back((uint8_t)crc);
    Serial.write(f.data(), f.size());
}

static void serial_send_byte(uint8_t type, uint8_t b) { serial_send(type, &b, 1); }

static void send_info() {
    char text[96];
    snprintf(text, sizeof text, "mammotion-rc v3-base %.1fMHz SF%d BW%.0f %ddBm",
             LORA_FREQ_MHZ, LORA_SF, LORA_BW_KHZ, LORA_TX_DBM);
    std::vector<uint8_t> p = { SERIAL_PROTO_VER };
    p.insert(p.end(), text, text + strlen(text));
    serial_send(S_INFO, p.data(), p.size());
}

static void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

static void send_stats() {
    const Stats& s = link_.stats();
    const int16_t rssi = (int16_t)(s.last_rssi * 10);
    const int16_t snr  = (int16_t)(s.last_snr * 10);
    std::vector<uint8_t> p = {
        (uint8_t)(rssi >> 8), (uint8_t)rssi, (uint8_t)(snr >> 8), (uint8_t)snr,
    };
    put_u32(p, s.tx_pkts);
    put_u32(p, s.retries);
    put_u32(p, s.failures);
    put_u32(p, s.rx_pkts);
    put_u32(p, s.rx_bad);
    put_u32(p, s.rx_replay);
    serial_send(S_STATS, p.data(), p.size());
}

// ── Serial RX state machine ──────────────────────────────────────────────────
static enum { SR_MAGIC0, SR_MAGIC1, SR_HDR, SR_BODY } sr_state = SR_MAGIC0;
static uint8_t  sr_hdr[3];
static size_t   sr_have = 0;
static size_t   sr_len  = 0;
static std::vector<uint8_t> sr_buf;
static uint32_t sr_last_byte_ms = 0;

static void handle_serial_frame(uint8_t type, const uint8_t* data, size_t len) {
    switch (type) {
    case DATA_TX:
    case OPEN:
    case CLOSE:
    case PING:
        if (!link_.send((MsgType)type, data, len)) serial_send_byte(S_BUSY, type);
        break;
    case S_HELLO:
        send_info();
        break;
    default:
        ESP_LOGW(TAG, "unknown serial frame type 0x%02x", type);
        break;
    }
}

static void poll_serial() {
    if (sr_state != SR_MAGIC0 &&
        (uint32_t)(millis() - sr_last_byte_ms) > SERIAL_FRAME_GAP_MS) {
        sr_state = SR_MAGIC0;              // stalled mid-frame — resync
    }
    while (Serial.available() > 0) {
        const uint8_t b = (uint8_t)Serial.read();
        sr_last_byte_ms = millis();
        switch (sr_state) {
        case SR_MAGIC0:
            if (b == MAGIC0) sr_state = SR_MAGIC1;
            break;
        case SR_MAGIC1:
            sr_state = (b == MAGIC1) ? SR_HDR : (b == MAGIC0 ? SR_MAGIC1 : SR_MAGIC0);
            sr_have = 0;
            break;
        case SR_HDR:
            sr_hdr[sr_have++] = b;
            if (sr_have == 3) {
                sr_len = ((size_t)sr_hdr[1] << 8) | sr_hdr[2];
                if (sr_len > MAX_FRAME_LEN) {
                    ESP_LOGW(TAG, "serial frame too large (%u) — resync", (unsigned)sr_len);
                    sr_state = SR_MAGIC0;
                    break;
                }
                sr_buf.clear();
                sr_have  = 0;
                sr_state = SR_BODY;
            }
            break;
        case SR_BODY:
            sr_buf.push_back(b);
            if (sr_buf.size() < sr_len + 2) break;
            {
                const uint16_t want = ((uint16_t)sr_buf[sr_len] << 8) | sr_buf[sr_len + 1];
                const uint16_t got  = crc16(sr_buf.data(), sr_len, crc16(sr_hdr, 3));
                if (want == got) {
                    handle_serial_frame(sr_hdr[0], sr_buf.data(), sr_len);
                } else {
                    ESP_LOGW(TAG, "serial CRC mismatch — dropped frame type 0x%02x", sr_hdr[0]);
                }
            }
            sr_state = SR_MAGIC0;
            break;
        }
    }
}

void setup() {
    Serial.setRxBufferSize(4096);
    Serial.begin(LORA_SERIAL_BAUD);
    delay(200);
    ESP_LOGI(TAG, "mammotion-rc LoRa base node");

    link_.set_rx_callback([](MsgType type, const uint8_t* data, size_t len) {
        serial_send(type, data, len);      // DATA_RX / STATUS / PONG straight to the PC
    });
    link_.set_sent_callback([](MsgType type) { serial_send_byte(S_SENT, type); });
    link_.set_fail_callback([](MsgType type) { serial_send_byte(S_FAILED, type); });

    if (!link_.begin()) {
        ESP_LOGE(TAG, "LoRa init failed — halting");
        while (true) delay(1000);
    }
    send_info();
}

void loop() {
    poll_serial();
    link_.loop();

    static uint32_t last_stats_ms = 0;
    static uint32_t last_stats_pkts = 0;
    const uint32_t pkts = link_.stats().rx_pkts + link_.stats().tx_pkts;
    if ((uint32_t)(millis() - last_stats_ms) > LORA_STATS_INTERVAL_MS && pkts != last_stats_pkts) {
        last_stats_ms   = millis();
        last_stats_pkts = pkts;
        send_stats();
    }
    delay(1);
}
