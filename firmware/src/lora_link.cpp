#include "lora_link.h"
#include <esp_log.h>
#include <esp_random.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <mbedtls/gcm.h>
#include "log.h"
#include "config.h"

#if __has_include("lora_key.h")
  #include "lora_key.h"
#endif
#ifndef LORA_PSK
  #error "LORA_PSK not set: copy firmware/include/lora_key.h.example to lora_key.h and generate a key"
#endif

namespace lora {

static const char* TAG = "lora";

namespace {

constexpr uint8_t  PROTO_VER  = 1;
constexpr size_t   HDR_LEN    = 9;    // ver|role, epoch, ctr
constexpr size_t   INNER_HDR  = 4;    // type, msg_id, frag_idx, frag_cnt
constexpr size_t   TAG_LEN    = 8;    // truncated GCM tag — plenty for a 30 s session link
constexpr size_t   MAX_PKT    = HDR_LEN + INNER_HDR + LORA_FRAG_MAX + TAG_LEN;
static_assert(MAX_PKT <= 255, "LoRa packet exceeds SX1262 FIFO");
static_assert((MAX_FRAME_LEN + LORA_FRAG_MAX - 1) / LORA_FRAG_MAX <= 255, "too many fragments");

constexpr uint32_t REKEY_MIN_INTERVAL_MS = 300;

SPIClass      s_spi(FSPI);
SX1262        s_radio = new Module(LORA_PIN_NSS, LORA_PIN_DIO1, LORA_PIN_RST,
                                   LORA_PIN_BUSY, s_spi);
Preferences   s_prefs;
mbedtls_gcm_context s_gcm;
const uint8_t s_key[16] = LORA_PSK;

volatile bool s_irq_flag = false;
void IRAM_ATTR on_dio1() { s_irq_flag = true; }

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
uint32_t get_u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

// nonce = role | 0 0 0 | epoch | ctr — unique per (sender, epoch, ctr).
void make_nonce(uint8_t nonce[12], uint8_t role, uint32_t epoch, uint32_t ctr) {
    nonce[0] = role; nonce[1] = nonce[2] = nonce[3] = 0;
    put_u32(nonce + 4, epoch);
    put_u32(nonce + 8, ctr);
}

bool is_data_type(uint8_t t) { return t >= DATA_TX && t <= PONG; }

}  // namespace

LoraLink::LoraLink(Role role) : role_(role) {}

bool LoraLink::begin() {
    bool key_set = false;
    for (uint8_t b : s_key) key_set |= (b != 0);
    if (!key_set) {
        ESP_LOGE(TAG, "LORA_PSK is all zeros — generate a real key in include/lora_key.h");
        return false;
    }
    mbedtls_gcm_init(&s_gcm);
    if (mbedtls_gcm_setkey(&s_gcm, MBEDTLS_CIPHER_ID_AES, s_key, 128) != 0) {
        ESP_LOGE(TAG, "mbedtls_gcm_setkey failed");
        return false;
    }

    s_prefs.begin("lora", false);
    epoch_      = s_prefs.getUInt("epoch", 0);
    bump_epoch_();                              // fresh nonce space every boot
    peer_epoch_ = s_prefs.getUInt("peer_ep", 0);
    require_new_peer_epoch_ = true;
    next_msg_id_ = (uint8_t)esp_random();

    s_spi.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_NSS);
    int st = s_radio.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                           LORA_SYNC_WORD, LORA_TX_DBM, LORA_PREAMBLE, LORA_TCXO_V,
                           false /* DC-DC regulator, not LDO */);
    if (st != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "SX1262 begin failed: %d", st);
        return false;
    }
    s_radio.setCurrentLimit(140.0);             // needed for full +22 dBm
    s_radio.setCRC(true);
    s_radio.setDio1Action(on_dio1);
    s_irq_flag = false;
    st = s_radio.startReceive();
    if (st != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "SX1262 startReceive failed: %d", st);
        return false;
    }
    ESP_LOGI(TAG, "radio up: %.1f MHz SF%d BW%.0f CR4/%d %d dBm, role=%s epoch=%u peer_epoch=%u",
             LORA_FREQ_MHZ, LORA_SF, LORA_BW_KHZ, LORA_CR, LORA_TX_DBM,
             role_ == ROLE_BASE ? "base" : "mower",
             (unsigned)epoch_, (unsigned)peer_epoch_);
    return true;
}

void LoraLink::loop() {
    handle_rx_();

    if (awaiting_ack_) {
        if ((int32_t)(millis() - ack_deadline_ms_) < 0) return;
        if (cur_tries_ > LORA_MAX_RETRIES) {
            ESP_LOGW(TAG, "msg %u type 0x%02x frag %u: no ACK after %u tries — giving up",
                     txq_.front().msg_id, txq_.front().type, cur_frag_, cur_tries_);
            finish_current_(false);
            return;
        }
        stats_.retries++;
        send_current_fragment_();
        return;
    }
    if (!txq_.empty()) send_current_fragment_();
}

bool LoraLink::send(MsgType type, const uint8_t* data, size_t len) {
    if (len > MAX_FRAME_LEN) {
        ESP_LOGW(TAG, "send: %u bytes exceeds MAX_FRAME_LEN", (unsigned)len);
        return false;
    }
    if (txq_.size() >= LORA_TX_QUEUE_MAX) {
        ESP_LOGW(TAG, "send: TX queue full (%u)", (unsigned)txq_.size());
        return false;
    }
    txq_.push_back(OutMsg{type, next_msg_id_++,
                          std::vector<uint8_t>(data, data + len)});
    return true;
}

void LoraLink::clear_queue() {
    txq_.clear();
    awaiting_ack_ = false;
    cur_frag_     = 0;
    cur_tries_    = 0;
}

// ── TX ────────────────────────────────────────────────────────────────────────

void LoraLink::send_current_fragment_() {
    const OutMsg& m = txq_.front();
    const size_t frag_cnt = m.data.empty() ? 1
        : (m.data.size() + LORA_FRAG_MAX - 1) / LORA_FRAG_MAX;
    const size_t off = (size_t)cur_frag_ * LORA_FRAG_MAX;
    const size_t n   = (m.data.size() > off)
        ? std::min((size_t)LORA_FRAG_MAX, m.data.size() - off) : 0;

    transmit_(m.type, m.msg_id, cur_frag_, (uint8_t)frag_cnt, m.data.data() + off, n);
    cur_tries_++;
    awaiting_ack_    = true;
    ack_deadline_ms_ = millis() + ack_timeout_ms_(n);
}

void LoraLink::send_ctl_(CtlType type, uint8_t msg_id, uint8_t frag_idx) {
    transmit_(type, msg_id, frag_idx, 0, nullptr, 0);
}

bool LoraLink::transmit_(uint8_t type, uint8_t msg_id, uint8_t frag_idx,
                         uint8_t frag_cnt, const uint8_t* chunk, size_t chunk_len) {
    if (ctr_ == UINT32_MAX) bump_epoch_();
    const uint32_t ctr = ++ctr_;

    uint8_t pkt[MAX_PKT];
    pkt[0] = (PROTO_VER << 4) | role_;
    put_u32(pkt + 1, epoch_);
    put_u32(pkt + 5, ctr);

    uint8_t plain[INNER_HDR + LORA_FRAG_MAX];
    plain[0] = type; plain[1] = msg_id; plain[2] = frag_idx; plain[3] = frag_cnt;
    if (chunk_len) memcpy(plain + INNER_HDR, chunk, chunk_len);
    const size_t plen = INNER_HDR + chunk_len;

    uint8_t nonce[12];
    make_nonce(nonce, role_, epoch_, ctr);
    if (mbedtls_gcm_crypt_and_tag(&s_gcm, MBEDTLS_GCM_ENCRYPT, plen, nonce, sizeof nonce,
                                  pkt, HDR_LEN, plain, pkt + HDR_LEN,
                                  TAG_LEN, pkt + HDR_LEN + plen) != 0) {
        ESP_LOGE(TAG, "GCM encrypt failed");
        return false;
    }

    const int st = s_radio.transmit(pkt, HDR_LEN + plen + TAG_LEN);
    stats_.tx_pkts++;
    // transmit() also raises DIO1 (TxDone) — that's not an RX event.
    s_irq_flag = false;
    s_radio.startReceive();
    if (st != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "transmit failed: %d", st);
        return false;
    }
    return true;
}

uint32_t LoraLink::ack_timeout_ms_(size_t /*chunk_len*/) {
    // transmit_() blocks until our packet is fully on air, so the deadline only
    // needs to cover the peer's turnaround plus the ACK's own airtime.  The
    // jitter de-synchronises two boards that keep colliding.
    const uint32_t ack_air = (uint32_t)(s_radio.getTimeOnAir(HDR_LEN + INNER_HDR + TAG_LEN) / 1000) + 1;
    return ack_air + LORA_ACK_MARGIN_MS + (esp_random() % (2 * ack_air + 20));
}

void LoraLink::finish_current_(bool delivered) {
    const MsgType t = txq_.front().type;
    txq_.pop_front();
    awaiting_ack_ = false;
    cur_frag_     = 0;
    cur_tries_    = 0;
    if (delivered) {
        if (sent_cb_) sent_cb_(t);
    } else {
        stats_.failures++;
        if (fail_cb_) fail_cb_(t);
    }
}

void LoraLink::bump_epoch_() {
    epoch_++;
    ctr_ = 0;
    s_prefs.putUInt("epoch", epoch_);
}

// ── RX ────────────────────────────────────────────────────────────────────────

void LoraLink::handle_rx_() {
    if (!s_irq_flag) return;
    s_irq_flag = false;

    uint8_t buf[256];
    size_t len = s_radio.getPacketLength();
    if (len > sizeof buf) len = sizeof buf;
    const int st = s_radio.readData(buf, len);
    const float rssi = s_radio.getRSSI();
    const float snr  = s_radio.getSNR();
    s_radio.startReceive();
    if (st != RADIOLIB_ERR_NONE) {
        stats_.rx_bad++;
        return;
    }
    handle_packet_(buf, len, rssi, snr);
}

void LoraLink::handle_packet_(const uint8_t* pkt, size_t len, float rssi, float snr) {
    if (len < HDR_LEN + INNER_HDR + TAG_LEN) { stats_.rx_bad++; return; }
    const uint8_t ver  = pkt[0] >> 4;
    const uint8_t role = pkt[0] & 0x0F;
    if (ver != PROTO_VER || role == role_) { stats_.rx_bad++; return; }
    const uint32_t epoch = get_u32(pkt + 1);
    const uint32_t ctr   = get_u32(pkt + 5);

    const size_t plen = len - HDR_LEN - TAG_LEN;
    uint8_t plain[INNER_HDR + LORA_FRAG_MAX];
    if (plen > sizeof plain) { stats_.rx_bad++; return; }
    uint8_t nonce[12];
    make_nonce(nonce, role, epoch, ctr);
    if (mbedtls_gcm_auth_decrypt(&s_gcm, plen, nonce, sizeof nonce, pkt, HDR_LEN,
                                 pkt + HDR_LEN + plen, TAG_LEN,
                                 pkt + HDR_LEN, plain) != 0) {
        stats_.rx_bad++;            // wrong key, corrupted, or forged
        return;
    }

    // Replay check — only after authentication, so forged headers can't move
    // our state.
    if (epoch < peer_epoch_) { stats_.rx_replay++; return; }
    if (epoch == peer_epoch_) {
        if (require_new_peer_epoch_) {
            // We rebooted since the peer chose this epoch, so we can't tell a
            // replay from a live packet.  Ask the peer to move on; its
            // retransmit in the new epoch will be accepted.
            stats_.rx_replay++;
            if ((uint32_t)(millis() - last_rekey_sent_ms_) > REKEY_MIN_INTERVAL_MS) {
                last_rekey_sent_ms_ = millis();
                ESP_LOGI(TAG, "peer still on pre-reboot epoch %u — sending REKEY", (unsigned)epoch);
                send_ctl_(REKEY, 0, 0);
            }
            return;
        }
        if (ctr <= peer_ctr_) { stats_.rx_replay++; return; }
    } else {
        // Peer rebooted or rekeyed.  Its msg_ids restart, so forget dedup and
        // any half-reassembled message.
        ESP_LOGI(TAG, "peer epoch %u → %u", (unsigned)peer_epoch_, (unsigned)epoch);
        peer_epoch_ = epoch;
        s_prefs.putUInt("peer_ep", peer_epoch_);
        require_new_peer_epoch_ = false;
        have_last_    = false;
        reasm_active_ = false;
    }
    peer_ctr_ = ctr;

    stats_.rx_pkts++;
    stats_.last_rssi = rssi;
    stats_.last_snr  = snr;

    const uint8_t type     = plain[0];
    const uint8_t msg_id   = plain[1];
    const uint8_t frag_idx = plain[2];
    const uint8_t frag_cnt = plain[3];
    if (type == ACK) {
        handle_ack_(msg_id, frag_idx);
    } else if (type == REKEY) {
        ESP_LOGI(TAG, "peer requested REKEY");
        bump_epoch_();
        if (awaiting_ack_) ack_deadline_ms_ = millis();   // retransmit in the new epoch now
    } else if (is_data_type(type)) {
        handle_data_((MsgType)type, msg_id, frag_idx, frag_cnt,
                     plain + INNER_HDR, plen - INNER_HDR);
    } else {
        stats_.rx_bad++;
    }
}

void LoraLink::handle_ack_(uint8_t msg_id, uint8_t frag_idx) {
    if (!awaiting_ack_ || txq_.empty()) return;
    const OutMsg& m = txq_.front();
    if (m.msg_id != msg_id || cur_frag_ != frag_idx) return;   // stale ACK
    const size_t frag_cnt = m.data.empty() ? 1
        : (m.data.size() + LORA_FRAG_MAX - 1) / LORA_FRAG_MAX;
    awaiting_ack_ = false;
    cur_tries_    = 0;
    if (++cur_frag_ >= frag_cnt) finish_current_(true);
}

void LoraLink::handle_data_(MsgType type, uint8_t msg_id, uint8_t frag_idx,
                            uint8_t frag_cnt, const uint8_t* chunk, size_t chunk_len) {
    if (frag_cnt == 0 || frag_idx >= frag_cnt) { stats_.rx_bad++; return; }

    // ACK first (before any slow delivery work) so the sender's timer doesn't
    // fire.  Duplicates are re-ACKed — it means our previous ACK was lost.
    send_ctl_(ACK, msg_id, frag_idx);
    if (have_last_ && msg_id == last_msg_id_ && frag_idx == last_frag_) return;
    have_last_   = true;
    last_msg_id_ = msg_id;
    last_frag_   = frag_idx;

    if (frag_idx == 0) {
        reasm_active_ = true;
        reasm_type_   = type;
        reasm_msg_id_ = msg_id;
        reasm_cnt_    = frag_cnt;
        reasm_next_   = 0;
        reasm_buf_.clear();
    } else if (!reasm_active_ || msg_id != reasm_msg_id_ || frag_idx != reasm_next_) {
        // The sender abandoned a message part-way (or we missed its start).
        reasm_active_ = false;
        return;
    }
    reasm_buf_.insert(reasm_buf_.end(), chunk, chunk + chunk_len);
    if (reasm_buf_.size() > MAX_FRAME_LEN) { reasm_active_ = false; stats_.rx_bad++; return; }
    if (++reasm_next_ < reasm_cnt_) return;

    reasm_active_ = false;
    if (rx_cb_) rx_cb_(reasm_type_, reasm_buf_.data(), reasm_buf_.size());
}

}  // namespace lora
