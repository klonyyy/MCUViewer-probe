/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: MIT
 *
 * The ESP-NOW link between a probe and a USB dongle: a packet header around one NETWORK PROBE frame, and the session
 * rules. Header-only C++17, shared by the probe and the dongle firmware. Hosts never see it: the dongle passes the
 * frames on bare.
 */

#ifndef ESPNOW_LINK_HPP
#define ESPNOW_LINK_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "NetworkProbeProto.hpp"

namespace EspNowLink
{
/*
 * Packet: header {u8 version, u8 kind, u16 session, u16 seq, u16 limit, u8 channel, u8 auth[8]}, then for DATA one
 * whole NETWORK PROBE frame, for PAIR a pairing step (EspNowPairing.hpp), for the other kinds nothing. channel is the
 * Wi-Fi channel the sender is on. In a session, auth is its tag, all zeros in a session of an unpaired dongle; with
 * session 0, auth starts with the MAC address the packet is for, ff:ff:ff:ff:ff:ff for every probe, and ends in zeros.
 *
 * Discovery: the dongle broadcasts DISCOVER and IDENTIFY frames with session 0, and a probe answers them with session 0
 * too, in every session state, as broadcasts addressed to the dongle that asked: packets with session 0 are always
 * broadcasts, so they need no ESP-NOW peer and are never encrypted, whatever the pairing of the two. Their seq and
 * limit are 0.
 *
 * Pairing (EspNowPairing.hpp): a probe paired with a dongle keeps it as an encrypted ESP-NOW peer, and the dongle keeps
 * the probe as one during their sessions, so a session of a paired dongle is encrypted by the radio. ESP-NOW also
 * delivers unencrypted packets from an encrypted peer, so every session packet of a paired pair carries a tag in auth
 * that only the two can make, and a packet without the right one is dropped: DATA the tag of its number's wrap
 * (SessionAuth), so an old DATA sent again by someone else does not pass, the other kinds the tag of the session.
 *
 * Channels: a probe stays on the channel of its network, or of its setup access point, so outside a session the dongle
 * sweeps: it broadcasts each DISCOVER and IDENTIFY on every channel in turn, listening for the answers a while before
 * the next one. A probe close by hears, and is heard on, the channels next to its own too, so the dongle takes a
 * probe's channel from the channel field of its answer, not from where it heard it. A probe does not answer while its
 * station scans or connects, which takes its radio across the channels: it would tell a channel it does not stay on. A
 * session owns the radio on both sides: the dongle stays on the probe's channel and broadcasts there only, and the
 * probe moves its radio to the channel in the dongle's packets, stops an attempt of its station under way, and neither
 * scans nor connects by itself while the session lasts, as either takes the radio off the channel.
 *
 * Sessions: the dongle opens one with the first DATA of a new, nonzero session number, and the probe takes it like a
 * new TCP client: it takes over from the client before. While the dongle holds the session, it sends PING when nothing
 * else went out for pingIntervalMs, and it ends the session with CLOSE. The probe ends it after sessionTimeoutMs
 * without a packet of it, or when another client takes over, and then sends CLOSE, once; the dongle ends it too when
 * its packets go unacknowledged for sessionTimeoutMs. A probe answers every packet of a session it does not serve with
 * CLOSE, once, unless the packet may start one: the first DATA (seq 1), or a PING before it (seq 0). So a dongle that
 * missed the end of its session learns of it at its next packet, also from a probe that restarted, whose radio
 * acknowledges packets as soon as it is up again; a paired dongle at its next PING, the packet whose tag the probe can
 * check without knowing the session. A probe paired with dongles takes sessions from them only, and answers the
 * packets of others with CLOSE. A session number is not used twice.
 *
 * Delivery: each side numbers the DATA it sends in a session from 1 on, in seq, and the dongle skips 0 when its numbers
 * wrap; a PING carries the seq of the last DATA the dongle sent, 0 before the first, and CLOSE carries 0. A sender keeps one packet
 * in the air at a time, and sends it again when the radio got no acknowledgement for it, up to maxResends times, after
 * a quiet pause at the lowest rate (EspNowRate.hpp); so packets arrive in order, and a receiver drops a DATA whose seq
 * is the last one it took: the first copy came through, only its acknowledgement was lost. A packet that fails every
 * time is lost; at a usable signal that is rare. A receiver takes a DATA only when its seq is at most seqWindow ahead
 * of the last one taken. A probe's answers outside a session go once, without resends: the dongle that asked may be on
 * another channel by then, and its host asks again.
 *
 * Flow control: every packet from the dongle carries limit, the highest seq the probe may send now. The dongle raises
 * it as it hands frames to the host, so a host that reads slowly holds the probe back instead of losing frames, as a
 * full TCP window does. The probe's packets carry limit 0.
 *
 * Rate: each sender picks the PHY rate of its session packets by itself, see EspNowRate.hpp, starting at the top with
 * every session. Broadcasts go at 1 Mb/s.
 */
constexpr uint8_t version = 5;
constexpr size_t authSize = 8;
constexpr size_t headerSize = 9 + authSize;
constexpr size_t maxPacket = 1470; /* ESP-NOW v2 */
constexpr size_t maxFrame = maxPacket - headerSize;
constexpr size_t maxPayload = maxFrame - NetworkProbe::frameOverhead; /* The max_payload of a probe over ESP-NOW */
constexpr uint16_t noSession = 0;
constexpr uint32_t pingIntervalMs = 500;
constexpr uint32_t sessionTimeoutMs = 2000;
constexpr int maxResends = 10;
constexpr uint16_t seqWindow = 1024; /* How far a DATA's seq may be ahead of the last one taken */
constexpr uint8_t broadcastMac[NetworkProbe::macSize] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

enum class Kind : uint8_t
{
	DATA = 1,
	PING = 2,
	CLOSE = 3,
	PAIR = 4, /* session 0 only */
};

struct Header
{
	Kind kind;
	uint16_t session;
	uint16_t seq;
	uint16_t limit;
	uint8_t channel;
	uint8_t auth[authSize]; /**< The session's tag, or with session 0 the MAC the packet is for. */
};

/**
 * @brief Writes a packet header to out[0 .. headerSize - 1].
 */
inline void encodeHeader(uint8_t* const out, const Header& header)
{
	out[0] = version;
	out[1] = NetworkProbe::toByte(header.kind);
	NetworkProbe::putU16(out + 2, header.session);
	NetworkProbe::putU16(out + 4, header.seq);
	NetworkProbe::putU16(out + 6, header.limit);
	out[8] = header.channel;
	memcpy(out + 9, header.auth, authSize);
}

/**
 * @brief Reads the header of a packet of length bytes.
 * @return false on a packet too short, of another version or of a kind not known.
 */
inline bool decodeHeader(const uint8_t* const in, const size_t length, Header& header)
{
	if (length < headerSize || in[0] != version || in[1] < NetworkProbe::toByte(Kind::DATA) || in[1] > NetworkProbe::toByte(Kind::PAIR))
		return false;
	header.kind = static_cast<Kind>(in[1]);
	header.session = NetworkProbe::getU16(in + 2);
	header.seq = NetworkProbe::getU16(in + 4);
	header.limit = NetworkProbe::getU16(in + 6);
	header.channel = in[8];
	memcpy(header.auth, in + 9, authSize);
	return true;
}

/**
 * @brief A packet with session 0: addresses it to mac, ff:ff:ff:ff:ff:ff for every probe.
 */
inline void addressTo(Header& header, const uint8_t* const mac)
{
	memset(header.auth, 0, authSize);
	memcpy(header.auth, mac, NetworkProbe::macSize);
}

/**
 * @return Whether a packet with session 0 is for mac: addressed to it, or to every probe.
 */
inline bool isFor(const Header& header, const uint8_t* const mac)
{
	return !memcmp(header.auth, mac, NetworkProbe::macSize) || !memcmp(header.auth, broadcastMac, NetworkProbe::macSize);
}

/**
 * @return Whether limit allows seq, across the wrap of the 16-bit numbers.
 */
inline bool allows(const uint16_t limit, const uint16_t seq)
{
	return static_cast<int16_t>(static_cast<uint16_t>(limit - seq)) >= 0;
}
}  // namespace EspNowLink

#endif /* ESPNOW_LINK_HPP */
