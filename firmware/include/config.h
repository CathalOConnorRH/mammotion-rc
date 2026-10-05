#pragma once

// ── WiFi credentials ──────────────────────────────────────────────────────────
// Two parallel sets — one for the HaLow AP (HD01), one for a regular 2.4 GHz
// router used during local testing.  The active set is chosen at build time by
// the USE_STANDARD_WIFI flag set in platformio.ini:
//   - env:hc33                   → HaLow (Heltec ESP_HaLow lib replaces WiFi.h)
//   - env:hc33-standard-wifi     → built-in ESP32 WiFi STA
//
// Both modes use the same logical SSID/PASS pair below — define each
// independently because your HaLow AP almost certainly uses a different
// SSID/password than your home WiFi.

#define WIFI_SSID_HALOW       "your-halow-ap-ssid"
#define WIFI_PASS_HALOW       "your-halow-ap-password"
// HaLow regulatory region — see ESP_HaLow/libraries/wifi-halow/src/mmwlan_regdb.h
// for valid codes.  Mismatch causes association to fail with no clear error.
#define WIFI_REGION_HALOW     "AU"
// HaLow security mode.  Must match what the HD01 AP is configured to broadcast:
//   MMWLAN_OPEN   - no security (HD01 default for testing; password ignored)
//   MMWLAN_SAE    - WPA3-SAE (most common production)
//   MMWLAN_OWE    - Opportunistic Wireless Encryption
// If association silently times out, try MMWLAN_OPEN first to rule out auth.
#define WIFI_SECURITY_HALOW   MMWLAN_SAE

// HaLow IP configuration.  Set HALOW_USE_STATIC_IP to 1 to bypass DHCP.
// Some HD01 firmware revisions don't bridge DHCP packets from the HaLow
// side to the LAN, so DHCP times out even though association succeeds.
// Static IP avoids the issue — pick an address outside the LAN router's
// DHCP pool (or add a static reservation in the router for the HC33's
// HaLow MAC AA:BB:CC:DD:EE:FF).
#define HALOW_USE_STATIC_IP   0
#define HALOW_STATIC_IP       "192.168.1.55"
#define HALOW_STATIC_NETMASK  "255.255.255.0"
#define HALOW_STATIC_GATEWAY  "192.168.1.1"

#define WIFI_SSID_STANDARD    "your-2.4ghz-ssid"
#define WIFI_PASS_STANDARD    "your-2.4ghz-password"

// ── softAP for the mower (HaLow env only) ────────────────────────────────────
// In Mode 2 the HC33's built-in 2.4 GHz radio runs as an AP that the mower
// connects to.  Traffic from the mower is NAPT'd out through the HaLow STA
// netif, so the mower reaches the Mammotion cloud the same way it would on a
// home WiFi router.  Standard-wifi builds skip this entirely — the S3 radio
// is the STA there.
//
// Pick AP_IP / AP_NETMASK so the AP subnet doesn't overlap with the LAN your
// HaLow uplink lands on (HALOW_STATIC_IP above is on 192.168.1.x, so the AP
// is on 192.168.4.x by default — that's also the arduino-esp32 default for
// WiFi.softAP() which makes the DHCP pool predictable).
#define AP_SSID         "MowerAP"
#define AP_PASSWORD     "mower-ap-password"   // min 8 chars or WPA2 rejects it
#define AP_CHANNEL      6                     // 2.4 GHz channel; pick to avoid HaLow band noise
#define AP_MAX_CONN     2                     // mower + spare slot for debugging
#define AP_IP           "192.168.4.1"
#define AP_GATEWAY      "192.168.4.1"
#define AP_NETMASK      "255.255.255.0"
// DNS server advertised to softAP clients via DHCP option 6.  Defaults to the
// HaLow gateway (typical home router does DNS); override to 8.8.8.8 / 1.1.1.1
// if the LAN router doesn't resolve.
#define AP_DNS_SERVER   HALOW_STATIC_GATEWAY

#ifdef USE_STANDARD_WIFI
  #define WIFI_SSID  WIFI_SSID_STANDARD
  #define WIFI_PASS  WIFI_PASS_STANDARD
#else
  #define WIFI_SSID  WIFI_SSID_HALOW
  #define WIFI_PASS  WIFI_PASS_HALOW
#endif

// ── Mower BLE identity ────────────────────────────────────────────────────────
// No MAC is configured: the mower is auto-discovered as the strongest BLE
// advertiser of UUID_SERVICE (below).  The HC33 sits <10 cm from the mower, so
// its signal dominates any other device advertising the same (non-unique) UUID.
// See ble_central.cpp ScanCb and memory: hc33-runtime-config-plan.
//
// Early-stop threshold for that scan.  An advertiser at or above this RSSI is
// treated as definitely the attached mower, so the scan stops immediately
// instead of running the full window — keeps reconnects sub-second.  A mower-
// mounted radio reads roughly -30..-45 dBm; -55 leaves comfortable margin
// while staying well above any device across the room.
#define MOWER_RSSI_STRONG_DBM   (-55)

// ── TCP server ────────────────────────────────────────────────────────────────
// PyMammotion's HC33ProxyTransport connects here.  Single client at a time.
#define TCP_PORT        9876

// Hard cap on a single BLE-op frame (large enough for any BluFi fragment after
// MTU negotiation).  Frames larger than this on the wire are treated as a
// protocol violation and force a client disconnect.
#define MAX_FRAME_LEN   600

// Drop the client (and consequently the BLE link) if no frame has arrived in
// this long.  PyMammotion's DeviceHandle emits a ble_sync heartbeat every 20s
// when the transport is alive, so 30s is generous-but-not-laggy: the web UI
// can sit idle as long as the Python side is up, but a silent network failure
// trips the safety stop within seconds.
#define CLIENT_IDLE_TIMEOUT_MS 30000

// ── BLE write backpressure ────────────────────────────────────────────────────
// A failed ble_.write() is almost always ENOMEM (the BLE controller's TX buffers
// are full after a HaLow burst delivered a backlog of frames at once), NOT an RF
// error — the mower's GATT is glued-on and healthy.  We must NEVER drop the
// frame: BluFi numbers every frame with a send sequence, so one dropped frame
// opens a sequence gap and the mower silently ignores all subsequent commands
// until the BLE session is rebuilt (the "reconnect makes the light work"
// symptom).  Instead we retry the same frame in order and stop reading the
// socket so TCP backpressures the server.  These bound each retry burst so a
// genuinely dead link still trips CLIENT_IDLE_TIMEOUT_MS instead of hanging.
#define BLE_WRITE_RETRY_BURST     6      // attempts per loop() tick before yielding
#define BLE_WRITE_RETRY_DELAY_MS  3      // yield between attempts (lets a conn event drain)
#define BLE_WRITE_STALL_LOG_MS    1000   // warn once if a frame stays unwritten this long

// ── BLE central ───────────────────────────────────────────────────────────────
#define DEVICE_NAME     "hc33-mammotion-proxy"   // local NimBLE host name
#define SCAN_TIMEOUT_MS 15000                    // initial scan to cache device
#define BLE_CONNECT_TIMEOUT_MS 10000

// When 1, log every advertising packet the scan sees (not just the mower).
// Useful for confirming the HC33's BLE radio is receiving anything at all.
// Set back to 0 once the mower is reliably discovered.
#define BLE_SCAN_VERBOSE 0

// Mammotion GATT UUIDs (must match pymammotion/bluetooth/const.py)
#define UUID_SERVICE                "0000ffff-0000-1000-8000-00805f9b34fb"
#define UUID_WRITE_CHARACTERISTIC   "0000ff01-0000-1000-8000-00805f9b34fb"
#define UUID_NOTIFY_CHARACTERISTIC  "0000ff02-0000-1000-8000-00805f9b34fb"

// ── LoRa variant (env:v3-mower / env:v3-base, Heltec WiFi LoRa 32 V3) ─────────
// Ignored by the HC33 builds.  See docs/lora-v3-plan.md and lora_link.h.
//
// SX1262 wiring on the Heltec V3 (fixed on the PCB).
#define LORA_PIN_NSS    8
#define LORA_PIN_SCK    9
#define LORA_PIN_MOSI   10
#define LORA_PIN_MISO   11
#define LORA_PIN_RST    12
#define LORA_PIN_BUSY   13
#define LORA_PIN_DIO1   14
#define LORA_TCXO_V     1.8f    // V3 SX1262 runs off a 1.8 V TCXO

// Radio settings — both boards MUST match.  920 MHz sits inside both the AU
// (915–928) and US (902–928) ISM bands.  AU allows up to 1 W EIRP here, so
// +22 dBm (the SX1262 max) plus a ~3 dBi antenna is legal.  If range is short,
// drop to SF8 / BW250 (roughly double the airtime, ~+5 dB link budget).
#define LORA_FREQ_MHZ   920.0f
#define LORA_BW_KHZ     500.0f
#define LORA_SF         7
#define LORA_CR         5       // coding rate 4/5
#define LORA_TX_DBM     22
#define LORA_PREAMBLE   8
#define LORA_SYNC_WORD  0x12    // SX126x "private" sync word

// Link layer.  Frames up to MAX_FRAME_LEN are split into LORA_FRAG_MAX-byte
// chunks; each chunk is stop-and-wait ACKed and retried up to LORA_MAX_RETRIES
// times.  The ACK timeout is computed from the actual time-on-air plus
// LORA_ACK_MARGIN_MS (covers the peer's turnaround) plus random jitter so two
// boards that collided don't retry in lockstep.
#define LORA_FRAG_MAX       200
#define LORA_MAX_RETRIES    5
#define LORA_ACK_MARGIN_MS  40
#define LORA_TX_QUEUE_MAX   32      // outbound messages; overflow = link failure, never a silent drop

// Mower dead-man: drop BLE if no frame from the PC (DATA_TX / OPEN) has
// arrived over LoRa in this long.  Mirrors CLIENT_IDLE_TIMEOUT_MS — PyMammotion
// heartbeats every 20 s while the session is alive.
#define LINK_IDLE_TIMEOUT_MS  CLIENT_IDLE_TIMEOUT_MS

// Base board: USB serial to lora_bridge.py.  Logs share the same UART; the
// bridge resyncs on the frame magic and prints everything else as log text.
#define LORA_SERIAL_BAUD        921600
#define LORA_STATS_INTERVAL_MS  5000    // base → PC link-quality report period

// The AES-128-GCM pre-shared key lives in include/lora_key.h (gitignored) —
// copy include/lora_key.h.example and generate a fresh key.  Both boards must
// be flashed with the same key.
