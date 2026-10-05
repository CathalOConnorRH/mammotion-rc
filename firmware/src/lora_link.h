// Reliable, encrypted message link between the two Heltec V3 boards over an
// SX1262 (RadioLib).  Used by both roles: main_v3_base.cpp (USB serial ↔ LoRa)
// and main_v3_mower.cpp (LoRa ↔ BLE).
//
// Over-the-air packet (≤ 9 + 4 + LORA_FRAG_MAX + 8 bytes):
//
//   clear header (also GCM AAD)        ciphertext                       tag
//   [ver|role 1B][epoch 4B][ctr 4B]    [type][msg_id][frag_idx][frag_cnt][chunk]   [8B]
//
// - AES-128-GCM with the pre-shared key from lora_key.h.  The 12-byte nonce is
//   role | epoch | ctr, so the two directions never share a nonce.
// - epoch is a per-board boot counter persisted in NVS (bumped every boot, and
//   on REKEY), ctr counts packets within the epoch.  Together they never repeat,
//   so a reboot can't reuse a nonce.
// - Replay protection: the receiver accepts (epoch, ctr) only if strictly newer
//   than the last authenticated packet from the peer.  The highest peer epoch
//   is persisted, and after our own reboot we require the peer to move to a
//   NEW epoch (we answer its first packet with REKEY) — so packets recorded
//   before our reboot can't be replayed either.
// - Every non-ACK packet is stop-and-wait ACKed (ACKs are authenticated too —
//   a forged ACK would otherwise silently drop a BluFi fragment).  A message
//   whose fragment exhausts LORA_MAX_RETRIES is reported via the failure
//   callback; the role code treats that as "session broken" and resyncs by
//   rebuilding the BLE session, exactly like the HC33 does on a TCP drop.
//
// RX is interrupt-flagged from DIO1; all processing happens in loop().

#pragma once

#include <Arduino.h>
#include <deque>
#include <functional>
#include <vector>

namespace lora {

// Application message types (carried end to end, base ↔ mower).
enum MsgType : uint8_t {
    DATA_TX = 0x01,   // PC → mower: one GATT write
    DATA_RX = 0x02,   // mower → PC: one GATT notification
    OPEN    = 0x03,   // PC → mower: open the BLE session
    CLOSE   = 0x04,   // PC → mower: close the BLE session
    PING    = 0x05,   // PC → mower: echo request (payload echoed in PONG)
    BLE_STATUS = 0x06,  // mower → PC: [BleState]  (not "STATUS": clashes with an IDF ROM typedef)
    PONG    = 0x07,   // mower → PC: [rssi i16 BE, dBm×10][snr i16 BE, dB×10][ping payload]
};

// Link-internal packet types (never surfaced to the role code).
enum CtlType : uint8_t {
    ACK   = 0x10,
    REKEY = 0x11,
};

enum BleState : uint8_t {
    BLE_CLOSED      = 0,
    BLE_UP          = 1,
    BLE_OPEN_FAILED = 2,
};

enum Role : uint8_t {
    ROLE_BASE  = 0,
    ROLE_MOWER = 1,
};

struct Stats {
    uint32_t tx_pkts      = 0;   // packets put on air (incl. retries and ACKs)
    uint32_t retries      = 0;   // fragment retransmissions
    uint32_t failures     = 0;   // messages abandoned after LORA_MAX_RETRIES
    uint32_t rx_pkts      = 0;   // authenticated packets received
    uint32_t rx_bad       = 0;   // CRC errors, auth failures, malformed
    uint32_t rx_replay    = 0;   // authenticated but stale (epoch, ctr)
    float    last_rssi    = 0;   // of the last authenticated packet
    float    last_snr     = 0;
};

class LoraLink {
public:
    using RxCallback   = std::function<void(MsgType type, const uint8_t* data, size_t len)>;
    using FailCallback = std::function<void(MsgType type)>;

    explicit LoraLink(Role role);

    // Bring up the radio and load epochs from NVS.  Returns false if the SX1262
    // didn't respond — the caller should log and halt.
    bool begin();

    // Drive RX, retransmits and the TX queue.  Call every Arduino loop().
    void loop();

    // Queue one message (≤ MAX_FRAME_LEN bytes).  Returns false if the queue is
    // full or the message is too large — never drops silently.
    bool send(MsgType type, const uint8_t* data, size_t len);

    // Fully reassembled inbound messages.  Fires from loop().
    void set_rx_callback(RxCallback cb)     { rx_cb_ = std::move(cb); }

    // A queued message was abandoned (fragment never ACKed).  Fires from loop().
    void set_fail_callback(FailCallback cb) { fail_cb_ = std::move(cb); }

    // A queued message was fully delivered (every fragment ACKed).
    void set_sent_callback(FailCallback cb) { sent_cb_ = std::move(cb); }

    // Drop everything queued (used when the session is torn down).
    void clear_queue();

    size_t queued() const { return txq_.size(); }
    bool   idle()   const { return txq_.empty(); }

    const Stats& stats() const { return stats_; }

private:
    struct OutMsg {
        MsgType              type;
        uint8_t              msg_id;
        std::vector<uint8_t> data;
    };

    Role     role_;
    uint32_t epoch_      = 0;    // ours, persisted
    uint32_t ctr_        = 0;    // ours, within epoch_

    uint32_t peer_epoch_ = 0;    // highest authenticated peer epoch (persisted)
    uint32_t peer_ctr_   = 0;
    bool     peer_seen_  = false;          // any packet from peer_epoch_ yet this boot
    bool     require_new_peer_epoch_ = true;  // set at boot: see header comment
    uint32_t last_rekey_sent_ms_ = 0;

    uint8_t  next_msg_id_ = 0;

    // Outbound stop-and-wait state.
    std::deque<OutMsg> txq_;
    uint8_t   cur_frag_     = 0;
    uint8_t   cur_tries_    = 0;
    bool      awaiting_ack_ = false;
    uint32_t  ack_deadline_ms_ = 0;

    // Inbound reassembly + duplicate suppression.
    bool                 have_last_ = false;
    uint8_t              last_msg_id_ = 0, last_frag_ = 0;
    bool                 reasm_active_ = false;
    uint8_t              reasm_msg_id_ = 0, reasm_next_ = 0, reasm_cnt_ = 0;
    MsgType              reasm_type_ = DATA_TX;
    std::vector<uint8_t> reasm_buf_;

    RxCallback   rx_cb_;
    FailCallback fail_cb_;
    FailCallback sent_cb_;
    Stats        stats_;

    void handle_rx_();
    void handle_packet_(const uint8_t* pkt, size_t len, float rssi, float snr);
    void handle_ack_(uint8_t msg_id, uint8_t frag_idx);
    void handle_data_(MsgType type, uint8_t msg_id, uint8_t frag_idx,
                      uint8_t frag_cnt, const uint8_t* chunk, size_t chunk_len);
    void send_current_fragment_();
    void send_ctl_(CtlType type, uint8_t msg_id, uint8_t frag_idx);
    bool transmit_(uint8_t type, uint8_t msg_id, uint8_t frag_idx, uint8_t frag_cnt,
                   const uint8_t* chunk, size_t chunk_len);
    void finish_current_(bool delivered);
    void bump_epoch_();
    uint32_t ack_timeout_ms_(size_t chunk_len);
};

}  // namespace lora
