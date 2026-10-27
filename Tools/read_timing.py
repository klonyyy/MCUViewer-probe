#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Piotr Wasilewski
# SPDX-License-Identifier: MIT
"""
Timing and data check of a NETWORK PROBE over its protocol: network round trip, READ and READ_BULK throughput, a
buffer read the way MCUViewer's recorder reads it, and the HSS sampling rate. It only reads; the target keeps running.

    python Tools/read_timing.py 192.168.1.19 --clock 4000,8000 --flash 0x08000000

--ram is where the timing reads go (default 0x20000000), with --buffer bytes read from there as one recorder buffer
(default 120000, the STM32G474 has 128 KiB of contiguous SRAM). --flash names memory that does not change: there READ and
READ_BULK are compared byte for byte, with 32-, 16- and 8-bit accesses, and --dump/--compare save or check a reference
taken with another firmware. Without an address the probe is discovered. A probe that serves another session is left
alone unless --force, as connecting takes the session over.
"""

import argparse
import socket
import statistics
import struct
import sys
import time
import zlib

PROTOCOL = 1
PORT = 2023
MAGIC = 0x564D

HELLO, CONNECT, DISCONNECT, READ, READ_LIST, DISCOVER = 0x01, 0x02, 0x03, 0x04, 0x06, 0x09
SET_PLAN, START, STOP, READ_BULK = 0x10, 0x11, 0x12, 0x15
RESP, DATA, EVENT, READ_DATA = 0x80, 0x81, 0x82, 0x84

CAP_STREAM, CAP_READ_BULK = 1 << 2, 1 << 5
EVENT_BUFFER_OVERFLOW = 2
STATUS = {0: "OK", 1: "MALFORMED", 2: "UNSUPPORTED", 3: "NOT_CONNECTED", 4: "TARGET_ERROR", 5: "NO_TARGET", 6: "BUSY"}
SESSION = {0: "free", 1: "host", 2: "gdb"}


def frame(frame_type, payload, seq):
    body = struct.pack("<HBBHH", MAGIC, 1, frame_type, len(payload), seq) + payload
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def discover(timeout=1.5):
    """Returns (address, tcp port, session) of every probe that answers a DISCOVER broadcast."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(0.2)
    sock.sendto(frame(DISCOVER, struct.pack("<H", PROTOCOL), 1), ("255.255.255.255", PORT))
    found = {}
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            data, peer = sock.recvfrom(2048)
        except socket.timeout:
            continue
        payload = data[8:-4]
        if len(payload) >= 8 and payload[0] == DISCOVER:
            _, tcp_port, session = struct.unpack_from("<HHB", payload, 2)
            found[peer[0]] = (peer[0], tcp_port, session)
    return list(found.values())


class ProbeError(Exception):
    pass


class Probe:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.seq = 0
        self.rx = bytearray()
        self.on_data = None
        self.on_event = None

    def close(self):
        self.sock.close()

    def _frame(self):
        while True:
            if len(self.rx) >= 8:
                magic, version, frame_type, length, seq = struct.unpack_from("<HBBHH", self.rx)
                if magic != MAGIC or version != 1:
                    raise ProbeError("corrupt stream")
                if len(self.rx) >= 12 + length:
                    whole = bytes(self.rx[:12 + length])
                    del self.rx[:12 + length]
                    if struct.unpack_from("<I", whole, 8 + length)[0] != zlib.crc32(whole[:8 + length]) & 0xFFFFFFFF:
                        raise ProbeError("CRC mismatch")
                    return frame_type, seq, whole[8:8 + length]
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise ProbeError("the probe closed the connection")
            self.rx += chunk

    def _dispatch(self, frame_type, payload):
        if frame_type == DATA and self.on_data:
            self.on_data(payload)
        elif frame_type == EVENT and self.on_event:
            self.on_event(payload)

    def request(self, request_type, payload=b"", bulk=None):
        """Sends a request and returns (status, data) of its RESP. READ_DATA of the request goes into bulk."""
        self.seq = (self.seq + 1) & 0xFFFF
        self.sock.sendall(frame(request_type, payload, self.seq))
        while True:
            frame_type, seq, body = self._frame()
            if frame_type == RESP and seq == self.seq:
                if body[0] != request_type:
                    raise ProbeError("answer to the wrong request")
                return body[1], body[2:]
            if frame_type == READ_DATA and seq == self.seq and bulk is not None:
                offset = struct.unpack_from("<I", body)[0]
                if offset != len(bulk):
                    raise ProbeError("READ_DATA out of order: offset %d after %d bytes" % (offset, len(bulk)))
                bulk += body[4:]
            else:
                self._dispatch(frame_type, body)

    def pump(self, seconds):
        """Handles DATA and EVENT frames for a while."""
        end = time.monotonic() + seconds
        self.sock.settimeout(0.05)
        try:
            while time.monotonic() < end:
                try:
                    frame_type, _, body = self._frame()
                except socket.timeout:
                    continue
                self._dispatch(frame_type, body)
        finally:
            self.sock.settimeout(5)

    def read(self, address, length):
        status, data = self.request(READ, struct.pack("<IH", address, length))
        if status != 0 or len(data) != length:
            raise ProbeError("READ of %d bytes at 0x%08X: %s" % (length, address, STATUS.get(status, status)))
        return data

    def read_chunked(self, address, length, chunk):
        """Reads a block in READs of chunk bytes, as MCUViewer does without READ_BULK."""
        data = bytearray()
        while len(data) < length:
            data += self.read(address + len(data), min(chunk, length - len(data)))
        return bytes(data)

    def read_bulk(self, address, length):
        data = bytearray()
        status, _ = self.request(READ_BULK, struct.pack("<II", address, length), bulk=data)
        if status != 0 or len(data) != length:
            raise ProbeError("READ_BULK of %d bytes at 0x%08X: %s, %d bytes came" % (length, address, STATUS.get(status, status), len(data)))
        return bytes(data)


def timed(function, repeat):
    times = []
    for _ in range(repeat):
        start = time.perf_counter()
        function()
        times.append(time.perf_counter() - start)
    return times


def describe(times):
    ms = sorted(t * 1e3 for t in times)
    return "min %7.2f  med %7.2f  p90 %7.2f  max %7.2f ms" % (ms[0], statistics.median(ms), ms[int(len(ms) * 0.9)], ms[-1])


def hss_rate(probe, address, variables, period_us, seconds):
    """Streams single-word blocks every period_us; returns (taken/s, received/s, dropped). A sample index counts every
    sample taken, also those the probe dropped, so the last one received tells the sampling rate."""
    blocks = b"".join(struct.pack("<IHB", address + 4 * i, 1, 4) for i in range(variables))
    status, _ = probe.request(SET_PLAN, struct.pack("<HIB", 1, period_us, variables) + blocks)
    if status != 0:
        raise ProbeError("SET_PLAN: %s" % STATUS.get(status, status))

    state = {"received": 0, "last_index": 0, "dropped": 0}

    def on_data(payload):
        _, first, count, _ = struct.unpack_from("<HIHH", payload)
        state["received"] += count
        state["last_index"] = first + count

    def on_event(payload):
        event, value = struct.unpack_from("<BI", payload)
        if event == EVENT_BUFFER_OVERFLOW:
            state["dropped"] = value

    probe.on_data, probe.on_event = on_data, on_event
    status, _ = probe.request(START)
    if status != 0:
        raise ProbeError("START: %s" % STATUS.get(status, status))
    start = time.monotonic()
    probe.pump(seconds)
    probe.request(STOP)
    elapsed = time.monotonic() - start
    # the probe flushes what its buffer still holds
    probe.pump(1.0)
    probe.on_data = probe.on_event = None
    return state["last_index"] / elapsed, state["received"] / elapsed, state["dropped"]


def check(label, data, reference):
    if data == reference:
        print("  %-34s OK" % label)
        return True
    first = next(i for i in range(min(len(data), len(reference))) if data[i] != reference[i]) if len(data) == len(reference) else -1
    print("  %-34s MISMATCH (lengths %d/%d, first difference at %d)" % (label, len(data), len(reference), first))
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host", nargs="?", help="probe address; discovered when left out")
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--clock", default="8000", help="SWD clocks to test in kHz, comma separated")
    parser.add_argument("--ap", type=int, default=0, help="MEM-AP index")
    parser.add_argument("--ram", type=lambda v: int(v, 0), default=0x20000000, help="address of the timing reads")
    parser.add_argument("--buffer", type=lambda v: int(v, 0), default=120000, help="bytes of the recorder buffer read from --ram on; keep it inside the RAM")
    parser.add_argument("--flash", type=lambda v: int(v, 0), help="memory that does not change, for the data check")
    parser.add_argument("--flash-length", type=lambda v: int(v, 0), default=0x20000)
    parser.add_argument("--dump", help="save the --flash read to this file")
    parser.add_argument("--compare", help="compare the --flash read with this file")
    parser.add_argument("--hss", type=int, default=8, help="variables of the HSS rate test, 0 to skip")
    parser.add_argument("--hss-period", type=int, default=100, help="HSS sampling period in us; 1 samples as fast as the probe can")
    parser.add_argument("--repeat", type=int, default=40)
    parser.add_argument("--force", action="store_true", help="take over a probe that serves another session")
    args = parser.parse_args()

    probes = discover()
    host = args.host or (probes[0][0] if probes else None)
    if not host:
        sys.exit("No probe answered the discovery; give its address.")
    session = {p[0]: p[2] for p in probes}.get(host)
    print("probe %s, session %s" % (host, SESSION.get(session, "unknown") if session is not None else "not discovered"))
    if session not in (None, 0) and not args.force:
        sys.exit("The probe serves another session; --force takes it over.")

    probe = Probe(host, args.port)
    status, hello = probe.request(HELLO, struct.pack("<H", PROTOCOL))
    if status != 0:
        sys.exit("HELLO: %s" % STATUS.get(status, status))
    max_payload, capabilities, max_read = struct.unpack_from("<HIH", hello, 2)
    clock_min, clock_max = struct.unpack_from("<II", hello, 16)
    bulk = bool(capabilities & CAP_READ_BULK)
    print("max_payload %d, max_read %d, SWD %d..%d Hz, READ_BULK %s" % (max_payload, max_read, clock_min, clock_max, "yes" if bulk else "no"))

    failures = 0
    try:
        for khz in (int(k) for k in args.clock.split(",")):
            status, connect = probe.request(CONNECT, struct.pack("<IBB", khz * 1000, args.ap, 0))
            if status != 0:
                sys.exit("CONNECT: %s" % STATUS.get(status, status))
            print("\n=== %d kHz requested, SWD clock %d Hz ===" % (khz, struct.unpack_from("<I", connect, 12)[0]))

            times = timed(lambda: probe.request(READ_LIST, struct.pack("<BIB", 1, args.ram, 4)), args.repeat * 5)
            print("READ_LIST, 1 word         %s" % describe(times))
            for size in (4, 256, 1024, 4096):
                if size > max_read:
                    continue
                times = timed(lambda: probe.read(args.ram, size), args.repeat)
                print("READ %5d B              %s  %7.1f KiB/s" % (size, describe(times), size / statistics.median(times) / 1024))
            if bulk:
                for size in (4096, 16384):
                    times = timed(lambda: probe.read_bulk(args.ram, size), args.repeat)
                    print("READ_BULK %5d B         %s  %7.1f KiB/s" % (size, describe(times), size / statistics.median(times) / 1024))

            # the recorder buffer, as MCUViewer reads it: in READs of max_read bytes, or in one READ_BULK
            times = timed(lambda: probe.read_chunked(args.ram, args.buffer, max_read), 3)
            print("%6d B buffer, READs     %s  %7.1f KiB/s" % (args.buffer, describe(times), args.buffer / statistics.median(times) / 1024))
            if bulk:
                times = timed(lambda: probe.read_bulk(args.ram, args.buffer), 3)
                print("%6d B buffer, READ_BULK %s  %7.1f KiB/s" % (args.buffer, describe(times), args.buffer / statistics.median(times) / 1024))

            if args.hss and capabilities & CAP_STREAM:
                taken, received, dropped = hss_rate(probe, args.ram, args.hss, args.hss_period, 2.0)
                print("HSS, %d words every %d us   sampled %8.0f/s, received %8.0f/s, dropped by the probe %d" % (args.hss, args.hss_period, taken, received, dropped))

        if args.flash is not None:
            print("\n=== data check at 0x%08X, %d bytes ===" % (args.flash, args.flash_length))
            reference = probe.read_chunked(args.flash, args.flash_length, max_read)
            failures += not check("READ twice", probe.read_chunked(args.flash, args.flash_length, max_read), reference)
            if bulk:
                failures += not check("READ_BULK, 32-bit", probe.read_bulk(args.flash, args.flash_length), reference)
                failures += not check("READ_BULK, 16-bit", probe.read_bulk(args.flash + 2, args.flash_length - 4), reference[2:-2])
                failures += not check("READ_BULK, 8-bit", probe.read_bulk(args.flash + 1, args.flash_length - 2), reference[1:-1])
                failures += not check("READ_BULK, page boundaries", probe.read_bulk(args.flash + 0x3FC, 0x808), reference[0x3FC:0xC04])
            failures += not check("READ, 8-bit at odd addresses", probe.read_chunked(args.flash + 1, 0x1001, max_read), reference[1:0x1002])
            if args.dump:
                open(args.dump, "wb").write(reference)
                print("  saved to %s" % args.dump)
            if args.compare:
                failures += not check("against " + args.compare, reference, open(args.compare, "rb").read()[:len(reference)])
    finally:
        try:
            probe.request(DISCONNECT)
        except (OSError, ProbeError):
            pass
        probe.close()

    if failures:
        sys.exit("%d data checks failed" % failures)


if __name__ == "__main__":
    main()
