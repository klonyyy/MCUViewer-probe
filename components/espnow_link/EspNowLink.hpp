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
 * Packet: header {u8 version, u8 kind, u16 session}, then for DATA one whole NETWORK PROBE frame, for the other kinds
 * nothing. Not encrypted yet.
 *
 * Discovery: the dongle broadcasts DISCOVER and IDENTIFY frames with session 0, and a probe answers them unicast, with
 * session 0 too, in every session state.
 *
 * Sessions: the dongle opens one with the first DATA of a new, nonzero session number, and the probe takes it like a
 * new TCP client: it takes over from the client before. While the dongle holds the session, it sends PING when nothing
 * else went out for pingIntervalMs, and it ends the session with CLOSE. The probe ends it after sessionTimeoutMs
 * without a packet of it, or when another client takes over, and then sends CLOSE. Packets of a session that ended are
 * dropped, so a new session needs a new number.
 */
constexpr uint8_t version = 1;
constexpr size_t headerSize = 4;
constexpr size_t maxPacket = 1470; /* ESP-NOW v2 */
constexpr size_t maxFrame = maxPacket - headerSize;
constexpr size_t maxPayload = maxFrame - NetworkProbe::frameOverhead; /* The max_payload of a probe over ESP-NOW */
constexpr uint16_t noSession = 0;
constexpr uint32_t pingIntervalMs = 500;
constexpr uint32_t sessionTimeoutMs = 2000;

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
};

/**
 * @brief Writes a packet header to out[0 .. headerSize - 1].
 */
inline void encodeHeader(uint8_t* const out, const Kind kind, const uint16_t session)
{
	out[0] = version;
	out[1] = NetworkProbe::toByte(kind);
	NetworkProbe::putU16(out + 2, session);
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
	return true;
}
}  // namespace EspNowLink

#endif /* ESPNOW_LINK_HPP */
