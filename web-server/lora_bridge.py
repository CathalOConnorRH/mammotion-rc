"""lora_bridge — stand in for the HC33's TCP server, over a Heltec V3 LoRa link.

    web-server ─TCP 127.0.0.1:9876─► lora_bridge.py ─USB serial─► V3 base ~~LoRa~~ V3 mower ─BLE─► mower

hc33_proxy.py and PyMammotion are unchanged: point the mower's mowers.toml
entry at this bridge (``hc33_host = "127.0.0.1"``, ``link = "lora"``) and it
sees the same ``[2B BE len][payload]`` TCP framing the HC33 speaks.

Lifecycle mirrors firmware/src/tcp_proxy.cpp:
  - TCP client connects  → OPEN over LoRa; TCP frames are held (not read) until
                           the mower node reports STATUS(BLE_UP), exactly like
                           the HC33 blocks in connect_mower().
  - TCP frame            → DATA_TX (one GATT write); next frame isn't read until
                           the base reports it ACKed, so TCP backpressures.
  - mower notification   → TCP frame.
  - TCP client leaves    → CLOSE.
  - anything that can desync BluFi (LoRa message abandoned, mower reports the
    BLE session closed, base rebooted / unplugged) → drop the TCP client so
    PyMammotion reconnects and starts a fresh BLE session.

Serial framing to the base (see firmware/src/main_v3_base.cpp):
    [0xA5][0x5A][type][len 2B BE][payload][crc16-ccitt-false 2B BE over type+len+payload]
Bytes outside frames are the base's log output and are logged as-is.

Usage:
    python lora_bridge.py --serial /dev/tty.usbserial-0001
    python lora_bridge.py --serial COM5 --test 50 --size 600      # link bench test
    python lora_bridge.py --serial COM5 --test 0 --interval 2      # range walk (Ctrl-C to stop)
"""

from __future__ import annotations

import argparse
import asyncio
import binascii
import contextlib
import logging
import os
import statistics
import struct
import time

import serial_asyncio

_LOGGER = logging.getLogger("lora_bridge")

MAGIC = b"\xa5\x5a"
MAX_FRAME_LEN = 600          # firmware/include/config.h MAX_FRAME_LEN

# Message types (firmware/src/lora_link.h + main_v3_base.cpp).
DATA_TX, DATA_RX, OPEN, CLOSE, PING, STATUS, PONG = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
S_HELLO, S_SENT, S_FAILED, S_BUSY, S_STATS, S_INFO = 0x20, 0x21, 0x22, 0x23, 0x24, 0x25
BLE_CLOSED, BLE_UP, BLE_OPEN_FAILED = 0, 1, 2
_BLE_NAMES = {BLE_CLOSED: "closed", BLE_UP: "up", BLE_OPEN_FAILED: "open-failed"}

# The mower node blocks in connect_mower() for up to scan (15 s) + connect
# (10 s); leave headroom for the LoRa round trip on top.
OPEN_TIMEOUT_S = 40.0
# Worst case for one message to be ACKed or abandoned by the base: 3 fragments
# × 6 tries × a few hundred ms.  Past this the base is presumed dead.
SEND_TIMEOUT_S = 20.0


class LinkDown(Exception):
    """The base board went away (unplugged, rebooted, serial error)."""


def _crc16(data: bytes) -> int:
    return binascii.crc_hqx(data, 0xFFFF)   # CRC-16/CCITT-FALSE


def _encode(ftype: int, payload: bytes) -> bytes:
    body = bytes([ftype]) + len(payload).to_bytes(2, "big") + payload
    return MAGIC + body + _crc16(body).to_bytes(2, "big")


class BaseLink:
    """USB-serial connection to the V3 base board."""

    def __init__(self, port: str, baud: int) -> None:
        self._port = port
        self._baud = baud
        self._writer: asyncio.StreamWriter | None = None
        self._send_lock = asyncio.Lock()
        self._pending: asyncio.Future[tuple[int, int]] | None = None   # (SENT/FAILED/BUSY, msg type)
        self._gen = 0                                       # bumps on every (re)connect / base reboot
        self.status_q: asyncio.Queue[int] = asyncio.Queue()
        self.pong_q: asyncio.Queue[bytes] = asyncio.Queue()
        self.on_data_rx = None            # callable(bytes) | None — set by the active session
        self.on_reset = None              # callable() | None — base went away / rebooted
        self.connected = asyncio.Event()

    @property
    def generation(self) -> int:
        return self._gen

    # ── public ────────────────────────────────────────────────────────────────

    async def run(self) -> None:
        """Keep the serial port open forever, reconnecting on errors."""
        while True:
            try:
                reader, writer = await serial_asyncio.open_serial_connection(
                    url=self._port, baudrate=self._baud)
            except Exception as exc:  # noqa: BLE001
                _LOGGER.warning("serial %s: %s — retrying in 2 s", self._port, exc)
                await asyncio.sleep(2)
                continue
            _LOGGER.info("serial %s open @ %d", self._port, self._baud)
            self._writer = writer
            self._reset("serial opened")
            self.connected.set()
            writer.write(_encode(S_HELLO, b""))
            try:
                await self._read_loop(reader)
            except Exception as exc:  # noqa: BLE001
                _LOGGER.warning("serial %s lost: %s", self._port, exc)
            finally:
                self.connected.clear()
                self._writer = None
                with contextlib.suppress(Exception):
                    writer.close()
                self._reset("serial lost")
            await asyncio.sleep(1)

    async def send(self, ftype: int, payload: bytes = b"", timeout: float = SEND_TIMEOUT_S) -> bool:
        """Send one message over LoRa; True once the mower node ACKed all of it.

        One message in flight at a time, so the base's queue never overflows
        and the caller naturally backpressures its own source.
        """
        async with self._send_lock:
            if self._writer is None:
                raise LinkDown("serial not open")
            gen = self._gen
            fut: asyncio.Future[tuple[int, int]] = asyncio.get_running_loop().create_future()
            self._pending = fut
            self._writer.write(_encode(ftype, payload))
            try:
                result, acked = await asyncio.wait_for(asyncio.shield(fut), timeout)
            except asyncio.TimeoutError as exc:
                raise LinkDown(f"base didn't answer type 0x{ftype:02x} in {timeout:.0f} s") from exc
            except asyncio.CancelledError:
                # The base will still answer this message; wait for that answer
                # so it can't be mistaken for the reply to the NEXT send.
                with contextlib.suppress(Exception):
                    await asyncio.wait_for(fut, timeout)
                raise
            finally:
                self._pending = None
            if gen != self._gen:
                raise LinkDown("base reset mid-send")
            if acked != ftype:
                raise LinkDown(f"base answered type 0x{acked:02x}, expected 0x{ftype:02x}")
            if result == S_BUSY:
                _LOGGER.warning("base TX queue full — type 0x%02x rejected", ftype)
            elif result == S_FAILED:
                _LOGGER.warning("LoRa delivery failed for type 0x%02x (no ACK after retries)", ftype)
            return result == S_SENT

    # ── internal ──────────────────────────────────────────────────────────────

    def _reset(self, why: str) -> None:
        self._gen += 1
        if self._pending is not None and not self._pending.done():
            self._pending.set_exception(LinkDown(why))
        for q in (self.status_q, self.pong_q):
            while not q.empty():
                q.get_nowait()
        if self.on_reset is not None:
            self.on_reset()

    async def _read_loop(self, reader: asyncio.StreamReader) -> None:
        buf = bytearray()
        text = bytearray()
        while True:
            chunk = await reader.read(4096)
            if not chunk:
                raise LinkDown("EOF")
            buf += chunk
            while True:
                i = buf.find(MAGIC)
                if i < 0:
                    # Keep a trailing 0xA5 in case it's the first magic byte.
                    keep = 1 if buf.endswith(MAGIC[:1]) else 0
                    text += buf[: len(buf) - keep]
                    del buf[: len(buf) - keep]
                    break
                text += buf[:i]
                del buf[:i]
                if len(buf) < 5:
                    break
                length = int.from_bytes(buf[3:5], "big")
                if length > MAX_FRAME_LEN + 64:
                    text += buf[:1]
                    del buf[:1]
                    continue
                if len(buf) < 5 + length + 2:
                    break
                body = bytes(buf[2 : 5 + length])
                crc = int.from_bytes(buf[5 + length : 7 + length], "big")
                if crc != _crc16(body):
                    text += buf[:1]           # not a real frame — treat as log text
                    del buf[:1]
                    continue
                del buf[: 7 + length]
                self._dispatch(body[0], body[3:])
            self._flush_text(text)

    @staticmethod
    def _flush_text(text: bytearray) -> None:
        while (nl := text.find(b"\n")) >= 0:
            line = text[:nl].decode("utf-8", "replace").rstrip("\r")
            del text[: nl + 1]
            if line.strip():
                _LOGGER.info("base| %s", line)
        if len(text) > 1024:                  # runaway garbage (boot ROM at 115200 baud)
            del text[:]

    def _dispatch(self, ftype: int, payload: bytes) -> None:
        if ftype in (S_SENT, S_FAILED, S_BUSY):
            if self._pending is not None and not self._pending.done():
                self._pending.set_result((ftype, payload[0] if payload else -1))
        elif ftype == DATA_RX:
            if self.on_data_rx is not None:
                self.on_data_rx(payload)
        elif ftype == STATUS:
            state = payload[0] if payload else BLE_CLOSED
            _LOGGER.info("mower node: BLE %s", _BLE_NAMES.get(state, state))
            self.status_q.put_nowait(state)
        elif ftype == PONG:
            self.pong_q.put_nowait(payload)
        elif ftype == S_STATS and len(payload) >= 28:
            rssi, snr, tx, retries, fails, rx, bad, replay = struct.unpack(">hhIIIIII", payload[:28])
            _LOGGER.info("link: rssi %.1f dBm snr %.1f dB | tx %d retries %d failed %d | rx %d bad %d replay %d",
                         rssi / 10, snr / 10, tx, retries, fails, rx, bad, replay)
        elif ftype == S_INFO:
            _LOGGER.info("base: %s", payload[1:].decode("ascii", "replace"))
            # Sent on boot and in answer to our HELLO.  Either way the base's
            # link state is fresh, so any session in progress is gone.
            self._reset("base (re)booted")
        else:
            _LOGGER.debug("ignoring frame type 0x%02x", ftype)


class Bridge:
    """TCP server that speaks the HC33 wire protocol and forwards over LoRa."""

    def __init__(self, link: BaseLink) -> None:
        self._link = link
        self._session: asyncio.Task[None] | None = None

    async def serve(self, host: str, port: int) -> None:
        server = await asyncio.start_server(self._on_client, host, port)
        _LOGGER.info("listening on %s:%d (HC33 wire protocol)", host, port)
        async with server:
            await server.serve_forever()

    async def _on_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        # One client at a time (one mower).  A new connection replaces the old —
        # PyMammotion only reconnects after it has given up on the previous one.
        if self._session is not None and not self._session.done():
            _LOGGER.info("new client %s replaces the active session", peer)
            self._session.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await self._session
        self._session = asyncio.current_task()
        try:
            await self._run_session(reader, writer, peer)
        except LinkDown as exc:
            _LOGGER.warning("session %s: base link down (%s)", peer, exc)
        finally:
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()
            _LOGGER.info("session %s ended", peer)

    async def _run_session(self, reader, writer, peer) -> None:
        link = self._link
        if not link.connected.is_set():
            _LOGGER.warning("client %s: base board not connected — refusing", peer)
            return
        _LOGGER.info("client %s connected — OPEN over LoRa", peer)
        while not link.status_q.empty():
            link.status_q.get_nowait()
        gen = link.generation
        ended = asyncio.Event()

        def on_data_rx(payload: bytes) -> None:
            if not writer.is_closing():
                writer.write(len(payload).to_bytes(2, "big") + payload)

        link.on_reset = ended.set
        uplink = watcher = None
        try:
            if not await link.send(OPEN):
                return
            # Hold the client's frames (don't read TCP) until BLE is up, like
            # the HC33 does while it sits in connect_mower().
            opening = asyncio.create_task(self._wait_status({BLE_UP, BLE_OPEN_FAILED}))
            reset = asyncio.create_task(ended.wait())
            done, _ = await asyncio.wait({opening, reset}, timeout=OPEN_TIMEOUT_S,
                                         return_when=asyncio.FIRST_COMPLETED)
            reset.cancel()
            opening.cancel()
            if opening not in done:
                if not ended.is_set():
                    _LOGGER.warning("no BLE status from mower node within %.0f s", OPEN_TIMEOUT_S)
                return
            if opening.result() != BLE_UP:
                _LOGGER.warning("mower node couldn't connect to the mower over BLE")
                return
            _LOGGER.info("BLE up — piping frames")

            link.on_data_rx = on_data_rx
            uplink = asyncio.create_task(self._tcp_to_lora(reader, ended))
            watcher = asyncio.create_task(self._watch_status(ended))
            await ended.wait()
        finally:
            link.on_data_rx = None
            link.on_reset = None
            tasks = [t for t in (uplink, watcher) if t is not None]
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            # CLOSE is idempotent on the mower node, so send it on every exit
            # path (incl. an OPEN that timed out) as long as the base is the
            # same one we opened through.
            if link.generation == gen and link.connected.is_set():
                await self._close_remote()

    async def _tcp_to_lora(self, reader: asyncio.StreamReader, ended: asyncio.Event) -> None:
        try:
            while True:
                length = int.from_bytes(await reader.readexactly(2), "big")
                if length == 0:
                    continue
                if length > MAX_FRAME_LEN:
                    _LOGGER.warning("frame too large (%d > %d) — disconnect", length, MAX_FRAME_LEN)
                    return
                payload = await reader.readexactly(length)
                if not await self._link.send(DATA_TX, payload):
                    return                  # BluFi sequence now has a gap — force a resync
        except (asyncio.IncompleteReadError, ConnectionError):
            _LOGGER.info("TCP client closed")
        except LinkDown as exc:
            _LOGGER.warning("base link down: %s", exc)
        finally:
            ended.set()

    async def _watch_status(self, ended: asyncio.Event) -> None:
        try:
            while True:
                state = await self._link.status_q.get()
                if state != BLE_UP:
                    _LOGGER.warning("mower node closed the BLE session — dropping client to resync")
                    return
        finally:
            ended.set()

    async def _wait_status(self, wanted: set[int]) -> int:
        while True:
            state = await self._link.status_q.get()
            if state in wanted:
                return state

    async def _close_remote(self) -> None:
        with contextlib.suppress(LinkDown):
            await self._link.send(CLOSE)


async def run_test(link: BaseLink, count: int, size: int, interval: float) -> None:
    """PING/PONG bench test: checks echo integrity, RTT and throughput."""
    await link.connected.wait()
    await asyncio.sleep(0.5)                  # let the base's boot INFO settle
    rtts: list[float] = []
    ok = failed = 0
    t_start = time.monotonic()
    i = 0
    try:
        while count == 0 or i < count:
            i += 1
            payload = os.urandom(size)
            while not link.pong_q.empty():
                link.pong_q.get_nowait()
            t0 = time.monotonic()
            try:
                if not await link.send(PING, payload):
                    failed += 1
                    print(f"#{i}: PING not delivered")
                    continue
                pong = await asyncio.wait_for(link.pong_q.get(), 15)
            except (asyncio.TimeoutError, LinkDown) as exc:
                failed += 1
                print(f"#{i}: no PONG ({exc or 'timeout'})")
                continue
            rtt = (time.monotonic() - t0) * 1000
            rssi, snr = struct.unpack(">hh", pong[:4])
            echoed = pong[4:]
            if echoed != payload[: MAX_FRAME_LEN - 4]:   # mower trims the echo to fit its 4-byte header
                failed += 1
                print(f"#{i}: echo MISMATCH ({len(echoed)} bytes)")
                continue
            ok += 1
            rtts.append(rtt)
            print(f"#{i}: {size} B  rtt {rtt:7.1f} ms   mower hears base at {rssi / 10:.1f} dBm, snr {snr / 10:.1f} dB")
            if interval:
                await asyncio.sleep(interval)
    finally:
        elapsed = time.monotonic() - t_start
        print(f"\n{ok} ok, {failed} failed of {i}")
        if rtts:
            print(f"rtt ms: min {min(rtts):.0f}  median {statistics.median(rtts):.0f}  max {max(rtts):.0f}")
            if not interval:
                print(f"throughput: {2 * size * ok / elapsed:.0f} B/s (both directions, payload only)")


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", required=True, help="base V3 serial port, e.g. COM5 or /dev/tty.usbserial-0001")
    ap.add_argument("--baud", type=int, default=921600, help="must match LORA_SERIAL_BAUD (default 921600)")
    ap.add_argument("--listen", default="127.0.0.1:9876", help="host:port for the HC33-compatible TCP server")
    ap.add_argument("--test", type=int, metavar="N", help="run N PING/PONG round trips instead of serving (0 = forever)")
    ap.add_argument("--size", type=int, default=600, help="--test payload bytes (max 600)")
    ap.add_argument("--interval", type=float, default=0.0, help="--test pause between pings, seconds")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S")

    link = BaseLink(args.serial, args.baud)
    serial_task = asyncio.create_task(link.run())
    try:
        if args.test is not None:
            await run_test(link, args.test, min(args.size, MAX_FRAME_LEN), args.interval)
        else:
            host, _, port = args.listen.rpartition(":")
            await Bridge(link).serve(host or "127.0.0.1", int(port))
    finally:
        serial_task.cancel()


if __name__ == "__main__":
    with contextlib.suppress(KeyboardInterrupt):
        asyncio.run(main())
