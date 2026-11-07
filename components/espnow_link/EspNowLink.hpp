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

#include "NetworkProbeProto.hpp"

namespace EspNowLink
{
/*
 * Packet: header {u8 version, u8 kind, u16 session, u16 seq, u16 limit}, then for DATA one whole NETWORK PROBE frame,
 * for the other kinds nothing. Not encrypted yet.
 *
 * Discovery: the dongle broadcasts DISCOVER and IDENTIFY frames with session 0, and a probe answers them unicast, with
 * session 0 too, in every session state. Their seq and limit are 0.
 *
 * Sessions: the dongle opens one with the first DATA of a new, nonzero session number, and the probe takes it like a
 * new TCP client: it takes over from the client before. While the dongle holds the session, it sends PING when nothing
 * else went out for pingIntervalMs, and it ends the session with CLOSE. The probe ends it after sessionTimeoutMs
 * without a packet of it, or when another client takes over, and then sends CLOSE; the dongle ends it too when its
 * packets go unacknowledged for sessionTimeoutMs. Packets of a session that ended are dropped, so a new session needs a
 * new number.
 *
 * Delivery: each side numbers the DATA it sends in a session from 1 on, in seq; PING and CLOSE carry 0. A sender keeps
 * one packet in the air at a time, and sends it again when the radio got no acknowledgement for it, up to maxResends
 * times; so packets arrive in order, and a receiver drops a DATA whose seq is the last one it took: the first copy came
 * through, only its acknowledgement was lost. A packet that fails every time is lost; at a usable signal that is rare.
 *
 * Flow control: every packet from the dongle carries limit, the highest seq the probe may send now. The dongle raises
 * it as it hands frames to the host, so a host that reads slowly holds the probe back instead of losing frames, as a
 * full TCP window does. The probe's packets carry limit 0.
 */
constexpr uint8_t version = 2;
constexpr size_t headerSize = 8;
constexpr size_t maxPacket = 1470; /* ESP-NOW v2 */
constexpr size_t maxFrame = maxPacket - headerSize;
constexpr size_t maxPayload = maxFrame - NetworkProbe::frameOverhead; /* The max_payload of a probe over ESP-NOW */
constexpr uint16_t noSession = 0;
constexpr uint32_t pingIntervalMs = 500;
constexpr uint32_t sessionTimeoutMs = 2000;
constexpr int maxResends = 10;

enum class Kind : uint8_t
{
	DATA = 1,
	PING = 2,
	CLOSE = 3,
};

struct Header
{
	Kind kind;
	uint16_t session;
	uint16_t seq;
	uint16_t limit;
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
}

/**
 * @brief Reads the header of a packet of length bytes.
 * @return false on a packet too short, of another version or of a kind not known.
 */
inline bool decodeHeader(const uint8_t* const in, const size_t length, Header& header)
{
	if (length < headerSize || in[0] != version || in[1] < NetworkProbe::toByte(Kind::DATA) || in[1] > NetworkProbe::toByte(Kind::CLOSE))
		return false;
	header.kind = static_cast<Kind>(in[1]);
	header.session = NetworkProbe::getU16(in + 2);
	header.seq = NetworkProbe::getU16(in + 4);
	header.limit = NetworkProbe::getU16(in + 6);
	return true;
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
