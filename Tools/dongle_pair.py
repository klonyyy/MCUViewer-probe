#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Piotr Wasilewski
# SPDX-License-Identifier: MIT
"""
Pairing of ESP-NOW dongles and probes, through a dongle on a serial port, until MCUViewer does it (needs pyserial):

    python Tools/dongle_pair.py COM7 list                        probes in range, and the dongle's pairings
    python Tools/dongle_pair.py COM7 pair network-probe-a1b2c3   then press the probe's BOOT button briefly
    python Tools/dongle_pair.py COM7 unpair network-probe-a1b2c3 the probe forgets the dongle, then the dongle the probe
                                       [--dongle-only]           only the dongle forgets the probe
    python Tools/dongle_pair.py COM7 connect network-probe-a1b2c3          a session and a HELLO, to see it work
    python Tools/dongle_pair.py COM7 probe-pairings network-probe-a1b2c3   the dongles the probe is paired with
    python Tools/dongle_pair.py COM7 forget-dongle network-probe-a1b2c3 AA:BB:CC:DD:EE:FF

unpair with a probe out of reach only makes the dongle forget it: the probe keeps the dongle until another paired
dongle makes it forget (forget-dongle), or its BOOT button is held for 10 s and then pressed briefly.
"""

import argparse
import struct
import sys
import time

from read_timing import DISCOVER, PROTOCOL, RESP, ProbeError, Probe, SerialLink, frame

HELLO = 0x01
PAIRINGS, UNPAIR = 0x16, 0x17
DONGLE_SELECT, DONGLE_PAIR, DONGLE_UNPAIR, DONGLE_PAIRINGS = 0x40, 0x42, 0x43, 0x44
STATUS = {0: "OK", 1: "MALFORMED", 2: "UNSUPPORTED", 3: "NOT_CONNECTED", 4: "TARGET_ERROR", 5: "NO_TARGET", 6: "BUSY", 7: "NO_PROBE", 8: "NOT_PAIRED", 9: "PAIRING_FAILED"}
PAIRING = {0: "open (paired with no dongle)", 1: "locked (paired with other dongles)", 2: "paired with this dongle"}
FAILURE = {
    1: "the probe did not answer (out of range, ESP-NOW off, or an older firmware)",
    2: "nobody pressed the probe's BOOT button in time",
    3: "another dongle asked the probe at the same time: both refused for 30 s",
    4: "the probe is paired with 4 dongles already: make it forget one first",
    5: "this dongle is paired with 8 probes already: unpair one first",
    6: "the keys did not match",
    7: "the pairing could not be saved",
}


def mac_text(mac):
    return ":".join("%02X" % b for b in mac)


def id_payload(probe_id):
    return bytes([len(probe_id)]) + probe_id.encode()


def status_text(status, data=b""):
    text = STATUS.get(status, str(status))
    if status == 9 and data:
        text += ": " + FAILURE.get(data[0], "reason %d" % data[0])
    return text


def discover(probe, timeout=1.5):
    """Returns {id: (hostname, pairing, session)} of every probe the dongle hears."""
    probe.link.send(frame(DISCOVER, struct.pack("<H", PROTOCOL), 0))
    found = {}
    end = time.monotonic() + timeout
    while True:
        got = probe._frame(end - time.monotonic())
        if got is None:
            return found
        frame_type, _, payload = got
        if frame_type != RESP or len(payload) < 8 or payload[0] != DISCOVER or payload[1] != 0:
            continue
        session, pairing = payload[6], payload[7]
        strings, at = [], 8
        while len(strings) < 4 and at < len(payload):
            strings.append(payload[at + 1:at + 1 + payload[at]].decode(errors="replace"))
            at += 1 + payload[at]
        if len(strings) >= 3:
            found[strings[2]] = (strings[3] if len(strings) > 3 else "", pairing, session)


def request(probe, request_type, payload=b"", timeout=5.0):
    """Sends a request and returns (status, data) of its RESP, waiting up to timeout seconds."""
    probe.seq = (probe.seq + 1) & 0xFFFF
    probe.link.send(frame(request_type, payload, probe.seq))
    end = time.monotonic() + timeout
    while True:
        got = probe._frame(max(0.0, end - time.monotonic()))
        if got is None:
            raise ProbeError("no answer to request 0x%02X within %.0f s" % (request_type, timeout))
        frame_type, seq, body = got
        if frame_type == RESP and seq == probe.seq and body and body[0] == request_type:
            return body[1], body[2:]


def dongle_pairings(probe):
    status, data = request(probe, DONGLE_PAIRINGS)
    if status != 0:
        raise ProbeError("DONGLE_PAIRINGS: " + status_text(status, data))
    entries, at = [], 1
    for _ in range(data[0]):
        mac = data[at:at + 6]
        length = data[at + 6]
        entries.append((data[at + 7:at + 7 + length].decode(errors="replace"), mac))
        at += 7 + length
    return entries


def find(probe, probe_id):
    """Discovers, so the dongle knows where the probe is; returns its (hostname, pairing, session) or None."""
    found = discover(probe)
    return found.get(probe_id)


def select(probe, probe_id):
    status, data = request(probe, DONGLE_SELECT, id_payload(probe_id))
    if status != 0:
        raise ProbeError("DONGLE_SELECT %s: %s" % (probe_id, status_text(status, data)))
    status, data = request(probe, HELLO, struct.pack("<H", PROTOCOL))
    if status != 0:
        raise ProbeError("HELLO: " + status_text(status, data))
    return struct.unpack_from("<H", data, 2)[0]


def command_list(probe, _):
    found = discover(probe)
    if not found:
        print("no probe answered")
    for probe_id, (hostname, pairing, session) in sorted(found.items()):
        print("%-24s %-28s %s" % (probe_id, hostname, PAIRING.get(pairing, "pairing byte %d" % pairing)))
    entries = dongle_pairings(probe)
    print("\nthis dongle is paired with %d probe(s)%s" % (len(entries), ": it talks to them only" if entries else ": it talks to open probes"))
    for probe_id, mac in entries:
        print("  %-24s %s%s" % (probe_id, mac_text(mac), "" if probe_id in found else "   (not in range)"))


def command_pair(probe, args):
    if find(probe, args.probe) is None:
        sys.exit("%s did not answer DISCOVER" % args.probe)
    print("asking %s for pairing: press its BOOT button briefly once its LED blinks yellow (30 s)" % args.probe)
    start = time.monotonic()
    status, data = request(probe, DONGLE_PAIR, id_payload(args.probe), timeout=45)
    if status != 0:
        sys.exit("pairing failed after %.1f s: %s" % (time.monotonic() - start, status_text(status, data)))
    print("paired in %.1f s" % (time.monotonic() - start))


def command_unpair(probe, args):
    found = None if args.dongle_only else find(probe, args.probe)
    told = False
    if args.dongle_only:
        print("only the dongle forgets %s; the probe keeps this dongle" % args.probe)
    elif found and found[1] == 2:
        select(probe, args.probe)
        status, data = request(probe, UNPAIR)
        if status != 0:
            sys.exit("UNPAIR on the probe: " + status_text(status, data))
        told = data[:1] == b"\x01"
        print("the probe forgot this dongle" if told else "the probe did not know this dongle")
    elif found:
        print("the probe does not know this dongle any more: only the dongle forgets it")
    else:
        print("%s is not in range: only the dongle forgets it; the probe keeps this dongle" % args.probe)
    status, data = request(probe, DONGLE_UNPAIR, id_payload(args.probe))
    if status != 0:
        sys.exit("DONGLE_UNPAIR: " + status_text(status, data))
    print("the dongle forgot %s" % args.probe)


def command_connect(probe, args):
    found = find(probe, args.probe)
    if found is None:
        sys.exit("%s did not answer DISCOVER" % args.probe)
    print("%s: %s" % (args.probe, PAIRING.get(found[1], found[1])))
    start = time.perf_counter()
    max_payload = select(probe, args.probe)
    print("session open and HELLO answered in %.1f ms, max_payload %d" % ((time.perf_counter() - start) * 1e3, max_payload))


def command_probe_pairings(probe, args):
    if find(probe, args.probe) is None:
        sys.exit("%s did not answer DISCOVER" % args.probe)
    select(probe, args.probe)
    status, data = request(probe, PAIRINGS)
    if status != 0:
        sys.exit("PAIRINGS: " + status_text(status, data))
    print("%s is paired with %d dongle(s):" % (args.probe, data[0]))
    for i in range(data[0]):
        entry = data[1 + 7 * i:8 + 7 * i]
        print("  %s%s" % (mac_text(entry[:6]), "   (this dongle)" if entry[6] & 1 else ""))


def command_forget_dongle(probe, args):
    mac = bytes(int(part, 16) for part in args.mac.replace("-", ":").split(":"))
    if len(mac) != 6:
        sys.exit("a MAC address has 6 bytes")
    if find(probe, args.probe) is None:
        sys.exit("%s did not answer DISCOVER" % args.probe)
    select(probe, args.probe)
    status, data = request(probe, UNPAIR, mac)
    if status != 0:
        sys.exit("UNPAIR: " + status_text(status, data))
    print("the probe forgot %s" % mac_text(mac) if data[:1] == b"\x01" else "the probe did not know %s" % mac_text(mac))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="the dongle's serial port")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list")
    for name in ("pair", "unpair", "connect", "probe-pairings"):
        command = commands.add_parser(name)
        command.add_argument("probe", help="the probe's id, network-probe-xxxxxx")
        if name == "unpair":
            command.add_argument("--dongle-only", action="store_true", help="do not tell the probe: it keeps the dongle")
    forget = commands.add_parser("forget-dongle")
    forget.add_argument("probe")
    forget.add_argument("mac")
    args = parser.parse_args()

    probe = Probe(SerialLink(args.port))
    handlers = {"list": command_list, "pair": command_pair, "unpair": command_unpair, "connect": command_connect, "probe-pairings": command_probe_pairings, "forget-dongle": command_forget_dongle}
    try:
        handlers[args.command](probe, args)
    except ProbeError as error:
        sys.exit(str(error))
    finally:
        probe.close()


if __name__ == "__main__":
    main()
