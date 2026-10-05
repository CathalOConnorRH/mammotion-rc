# LoRa link for mammotion-rc on 2× Heltec WiFi LoRa 32 V3 (915 MHz)

## Context
Goal: drive the mower over a long-range LoRa link, with a board at the mower doing Bluetooth so the mower sees a "nearby phone". Today the repo works like this:

```
web-server (PyMammotion) ──TCP:9876, [2B len][payload]──► HC33 (ESP32-S3) ──BLE GATT ff01/ff02──► mower
```
- `web-server/hc33_proxy.py`: the BluFi codec runs on the PC. Each TCP frame is one GATT write (TX) or one notification (RX).
- `firmware/src/tcp_proxy.cpp` + `ble_central.cpp`: a simple byte pipe. A TCP connect opens BLE, a TCP close (or 30 s idle) drops it.

Hardware choice: **two Heltec V3 boards** (ESP32-S3 + SX1262, 863–928 MHz). The mower V3 handles both Bluetooth and LoRa on one board. The base V3 plugs into the PC over USB serial. In AU, the 915–928 MHz band allows up to 1 W EIRP, and the V3 tops out at +22 dBm, so it can run at full power. That gives far more range than 433 MHz at 25 mW.

## Decisions already made (don't revisit)
- **2× Heltec V3 at 915 MHz** chosen over 2× Feather M0 RFM96 (433 MHz) + UM FeatherS3. The Feather M0 has no BLE, so the mower end needed two boards joined by a serial link. 433 MHz in AU is also capped at 25 mW EIRP, against up to 1 W at 915–928 MHz. The V3 does BLE and LoRa on one board.
- **PlatformIO/Arduino, not ESPHome.** ESPHome's `packet_transport` isn't a byte-stream transport, so fragmentation, ARQ, encryption and the dead-man would still be custom C++. ESPHome also uses Bluedroid, which would mean abandoning the NimBLE `ble_central.cpp` (UUID+RSSI scan, RTK-base rejection, write-retry handling).
- **No changes to `hc33_proxy.py` or PyMammotion.** The PC bridge emulates the HC33 TCP server.
- The camera is out of scope over LoRa, because video goes over the mower's own Wi-Fi to the cloud.

## Key existing files to read first
- `web-server/hc33_proxy.py`: TCP framing `[2B BE len][payload]`. TX frame = one GATT write, RX frame = one notification.
- `firmware/src/tcp_proxy.cpp`: connect→open BLE, disconnect/idle→close BLE, BLE write retry/stall logic. This is the template for the mower role.
- `firmware/src/ble_central.cpp`, `firmware/include/config.h` (UUIDs, `MAX_FRAME_LEN` 600, `CLIENT_IDLE_TIMEOUT_MS` 30000).
- `firmware/platformio.ini`: `env:hc33-base` / `env:hc33-standard-wifi` show the stock-platform + NimBLE setup to copy.
- `web-server/static/app.js` (`REPEAT_MS`, status/heading `setInterval`s) and `web-server/app.py` (`/ws/joystick`, mower config loading).

## Architecture
```
PC: web-server ─TCP 127.0.0.1:9876─► lora_bridge.py ─USB serial (CP2102)─► V3 "base"
                                                                         ~~ 915 MHz LoRa (SX1262) ~~
Mower:                                                                   V3 "mower" (NimBLE central) ─BLE─► mower
```
- Leave `hc33_proxy.py` and the PyMammotion integration untouched. `lora_bridge.py` pretends to be the HC33's TCP server, so `mowers.toml` just points at `127.0.0.1:9876`.
- One firmware with two roles, selected by build env. The mower role reuses `ble_central.cpp` unchanged.

## Constraints the design must handle
- **Bandwidth:** SF7/BW500 gives about 22 kbps, SF7/BW125 about 5.5 kbps. The link is half-duplex with packets of up to 255 bytes. Today's traffic is joystick heartbeats every 150 ms (`static/app.js:306`), heading every 1 s and status every 3 s. All three need slowing down for LoRa mowers.
- **No packet loss:** BluFi fragments carry sequence numbers, so a dropped fragment breaks the message and can desync the session. RadioLib has no reliability layer, so we add our own acks and retries.
- **Security:** the radio link must be encrypted and authenticated, or anyone with a 915 MHz radio could drive the mower.
- **Camera:** video goes over the mower's Wi-Fi to the cloud, so it won't work over LoRa. This is unchanged.

## Changes

### 1. Firmware: shared LoRa link module (`firmware/src/lora_link.cpp/.h`)
- RadioLib `SX1262` with the V3 pins NSS=8, SCK=9, MOSI=10, MISO=11, RST=12, BUSY=13, DIO1=14. Default settings: 915–928 MHz, +22 dBm, SF7/BW500/CR4/5, sync word private. All are configurable in `config.h`.
- Packet header: `type | msg_id | frag_idx | frag_cnt`. Frames up to `MAX_FRAME_LEN` 600 are split into ≤200-byte chunks and reassembled on the other end.
- Reliability: stop-and-wait ARQ per fragment, with an ACK, a retry timeout of about 2× airtime and up to 5 retries. Duplicates are dropped by `msg_id`.
- Encryption: AES-128-GCM via the mbedTLS built into ESP-IDF, with a pre-shared key in `config.h`. The nonce is a counter, and replays are rejected.
- Message types: `DATA_TX` (PC→mower GATT write), `DATA_RX` (mower notify→PC), `OPEN`, `CLOSE`, `PING`.
- RX runs on the DIO1 interrupt and sets a flag. All processing happens in `loop()`, never in the ISR.

### 2. Firmware: roles and build envs
- `src/main_v3_mower.cpp`: `BleCentral` + `LoraLink`. `OPEN` → `connect_mower()`, `CLOSE` → `disconnect_mower()`. Writes and notifies are piped with the same retry and stall handling as `tcp_proxy.cpp`. Dead-man: if nothing is received for `LINK_IDLE_TIMEOUT_MS` (mirroring `CLIENT_IDLE_TIMEOUT_MS`), drop BLE.
- `src/main_v3_base.cpp`: USB `Serial` ↔ `LoraLink`. On the serial side it uses `[1B type][2B BE len][payload]`.
- Optional: show RSSI/SNR, link state and BLE state on the V3's OLED (SSD1306, SDA 17, SCL 18, RST 21, power enabled by driving Vext GPIO 36 low).
- `platformio.ini`: add `env:v3-mower` and `env:v3-base` with `platform = espressif32@6.10.0`, `board = heltec_wifi_lora_32_V3`, and lib deps `jgromes/RadioLib` (plus NimBLE for the mower). Use `build_src_filter` so each env compiles only its own main + `lora_link.cpp` (+ `ble_central.cpp` for the mower). Exclude `main.cpp` and the HaLow/softAP/TCP/discovery sources.
- Reuse `ble_central.cpp`, `log.h` and the `config.h` UUIDs as is.

### 3. Web server
- Add `web-server/lora_bridge.py` (pyserial-asyncio; add it to `requirements.txt`). It listens on 127.0.0.1:9876. On TCP accept it sends `OPEN`, then pipes frames both ways. On TCP close it sends `CLOSE`. It also logs link RSSI/SNR if the base reports them.
- Add a per-mower `link = "lora"` option in `mowers.toml`. When set, the UI slows the joystick repeat to about 400 ms, the heading poll to about 3 s and the status poll to about 10 s. Wi-Fi/HaLow mowers are unchanged.
- Onboarding auto-discovery (UDP broadcast) can't find a LoRa mower. Document the manual `mowers.toml` entry (`host = "127.0.0.1"`, `link = "lora"`).

### 4. Docs
Add a README section on the LoRa variant: the two V3 boards, flashing (`pio run -e v3-mower -t upload` / `-e v3-base`), antennas (never transmit without one), AU power rules, the bridge script, and that the camera won't work over LoRa.

## Verification
1. Bench test the link: flash both V3s and run a loopback test script through `lora_bridge.py` with 600-byte frames. Confirm reassembly, ack/retry under induced loss (move the boards apart or lower the TX power), and measure the round-trip time and throughput.
2. Bench test with the mower: put the mower V3 next to the mower and the base on the PC. Connect from the web UI, then check that status updates arrive and the joystick, Pause/Resume/Go Home and light all work.
3. Check the dead-man: hold the joystick, then unplug the base V3. The mower must stop within the idle timeout. Releasing the stick should stop it immediately.
4. Range walk: log RSSI/SNR across the property at SF7/BW500. If needed, fall back to SF8/BW250 and re-check latency (target under 500 ms joystick response).
