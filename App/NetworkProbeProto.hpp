/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: MIT
 *
 * NETWORK PROBE protocol, version 1: message types, status codes, payload sizes and the frame codec.
 * The specification lives in MCUViewer. Header-only C++17, shared by the probe firmware and the hosts,
 * which keep verbatim copies of it, so it must not contain code derived from Black Magic Debug.
 */

#ifndef NETWORK_PROBE_PROTO_HPP
#define NETWORK_PROBE_PROTO_HPP

#include <cstddef>
#include <cstdint>

namespace NetworkProbe
{
constexpr uint16_t protocolVersion = 1;
constexpr uint16_t tcpPort = 2023;
constexpr uint16_t discoveryPort = 2023; /* UDP; the DISCOVER answer names the TCP port */

/* Framing: header {u16 magic, u8 version, u8 type, u16 length, u16 seq}, payload, u32 CRC-32 */
constexpr uint16_t frameMagic = 0x564D; /* the bytes 'M', 'V' */
constexpr uint8_t frameVersion = 1;
constexpr size_t headerSize = 8;
constexpr size_t crcSize = 4;
constexpr size_t frameOverhead = headerSize + crcSize;
constexpr size_t payloadBeforeHello = 256; /* largest request payload a host sends before it knows max_payload */

enum class Type : uint8_t
{
	HELLO = 0x01,
	CONNECT = 0x02,
	DISCONNECT = 0x03,
	READ = 0x04,
	WRITE = 0x05,
	READ_LIST = 0x06,
	RESET = 0x07,
	GET_STATUS = 0x08,
	DISCOVER = 0x09, /* over UDP only */
	SET_PLAN = 0x10,
	START = 0x11,
	STOP = 0x12,
	SWO_START = 0x13,
	SWO_STOP = 0x14,
	READ_BULK = 0x15, /* answered with READ_DATA frames, then the RESP */
	RESP = 0x80,
	DATA = 0x81,
	EVENT = 0x82,
	SWO_DATA = 0x83,
	READ_DATA = 0x84, /* part of a READ_BULK answer, with the seq of the request */
};

enum class Status : uint8_t
{
	OK = 0,
	MALFORMED = 1,
	UNSUPPORTED = 2,
	NOT_CONNECTED = 3,
	TARGET_ERROR = 4, /* data: u8 fault */
	NO_TARGET = 5,
	BUSY = 6,
};

/* The fault byte of Status::TARGET_ERROR */
enum class Fault : uint8_t
{
	PARITY = 1,
	WAIT = 2,
	FAULT = 4,
	NO_RESPONSE = 7,
	STICKY = 8,
	OTHER = 255,
};

/* HELLO capability bits */
constexpr uint32_t capSwd = 1UL << 0;
constexpr uint32_t capReset = 1UL << 1;
constexpr uint32_t capStream = 1UL << 2;
constexpr uint32_t capCyccnt = 1UL << 3;
constexpr uint32_t capSwo = 1UL << 4;
constexpr uint32_t capReadBulk = 1UL << 5;

/* GET_STATUS link state */
enum class Link : uint8_t
{
	DISCONNECTED = 0,
	CONNECTED = 1,
	STREAMING = 2,
};

enum class Event : uint8_t
{
	TARGET_LOST = 1,     /* value 0: streaming stopped */
	BUFFER_OVERFLOW = 2, /* value: samples dropped since START */
	SAMPLE_ERRORS = 3,   /* value: samples lost to target errors since START */
	SWO_OVERFLOW = 4,    /* value: SWO bytes dropped since SWO_START */
	SWO_ERRORS = 5,      /* value: SWO receive errors since SWO_START */
};

/* SWO_START encoding: how the target's TPIU sends SWO */
enum class SwoEncoding : uint8_t
{
	NRZ = 0, /* UART: 8 data bits, no parity, 1 stop bit */
	MANCHESTER = 1,
};

/* DISCOVER session state */
enum class Session : uint8_t
{
	FREE = 0,
	HOST = 1, /* a host is connected */
	GDB = 2,  /* another client uses the debug link */
};

/* Payload sizes */
constexpr size_t respHeaderSize = 2;            /* u8 request, u8 status */
constexpr size_t helloRequestSize = 2;          /* u16 protocol */
constexpr size_t helloResponseFixedSize = 24;   /* up to and including clock_max; name, firmware and id follow */
constexpr size_t helloSwoSize = 8;              /* u32 swo_min, u32 swo_max, after id, with capSwo */
constexpr size_t connectRequestSize = 6;        /* u32 clock, u8 ap_sel, u8 flags */
constexpr size_t connectResponseSize = 16;      /* u32 dpidr, u32 targetid, u32 ap_idr, u32 clock */
constexpr size_t readRequestSize = 6;           /* u32 address, u16 length */
constexpr size_t writeRequestHeaderSize = 6;    /* u32 address, u16 length, then the data */
constexpr size_t readListEntrySize = 5;         /* u32 address, u8 size; the request starts with u8 count */
constexpr size_t statusResponseSize = 16;       /* u8 link, u8 reserved[3], u32 clock, u32 target_errors, u32 uptime_ms */
constexpr size_t discoverRequestSize = 2;       /* u16 protocol */
constexpr size_t discoverResponseFixedSize = 6; /* u16 protocol, u16 tcp_port, u8 session, u8 reserved; name, firmware and id follow */
constexpr size_t planHeaderSize = 7;            /* u16 plan_id, u32 period_us, u8 nblocks */
constexpr size_t planBlockSize = 7;             /* u32 address, u16 count, u8 access_size */
constexpr size_t dataHeaderSize = 10;           /* u16 plan_id, u32 first_sample, u16 nsamples, u16 bytes_per_sample */
constexpr size_t eventSize = 5;                 /* u8 event, u32 value */
constexpr size_t sampleTimeSize = 4;            /* u32 t at the start of every sample record */
constexpr size_t swoStartRequestSize = 6;       /* u32 baud, u8 encoding, u8 flags */
constexpr size_t swoStartResponseSize = 4;      /* u32 baud */
constexpr size_t swoDataHeaderSize = 4;         /* u32 first_byte, then the bytes */
constexpr size_t readBulkRequestSize = 8;       /* u32 address, u32 length */
constexpr size_t readDataHeaderSize = 4;        /* u32 offset of the first byte in the READ_BULK, then the bytes */
constexpr uint32_t readBulkMaxLength = 1UL << 24;
constexpr uint32_t blockBoundary = 0x400;       /* a plan block must not cross a multiple of 1 KiB */

/*
 * READ_BULK reads length bytes from address with the widest access of 4, 2 or 1 bytes that divides both, like READ.
 * The probe sends the bytes in order, in READ_DATA frames of at most max_payload, then the RESP: OK without data once
 * every byte went out, or TARGET_ERROR when an access failed, after the READ_DATA of the bytes read before it.
 */

/* Header fields of a received frame. type holds whatever byte the sender put there, known or not. */
struct FrameHeader
{
	Type type;
	uint16_t length; /* payload bytes */
	uint16_t seq;
};

/**
 * @brief The byte an enum travels as on the wire.
 */
template <typename E>
constexpr uint8_t toByte(const E value)
{
	return static_cast<uint8_t>(value);
}

inline void putU16(uint8_t* const out, const uint16_t value)
{
	out[0] = static_cast<uint8_t>(value);
	out[1] = static_cast<uint8_t>(value >> 8U);
}

inline void putU32(uint8_t* const out, const uint32_t value)
{
	out[0] = static_cast<uint8_t>(value);
	out[1] = static_cast<uint8_t>(value >> 8U);
	out[2] = static_cast<uint8_t>(value >> 16U);
	out[3] = static_cast<uint8_t>(value >> 24U);
}

inline uint16_t getU16(const uint8_t* const in)
{
	return static_cast<uint16_t>(in[0] | (static_cast<uint16_t>(in[1]) << 8U));
}

inline uint32_t getU32(const uint8_t* const in)
{
	return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8U) | (static_cast<uint32_t>(in[2]) << 16U) | (static_cast<uint32_t>(in[3]) << 24U);
}

/**
 * @brief CRC-32/ISO-HDLC, the CRC of zlib. Nibble table: small, and fast enough for hosts. ESP32 firmware can use
 *        esp_rom_crc32_le() instead, which computes the same value.
 * @param crc 0 to start, or the previous result to continue over more data.
 */
inline uint32_t crc32(uint32_t crc, const uint8_t* const data, const size_t length)
{
	static constexpr uint32_t table[16] = {0x00000000UL, 0x1db71064UL, 0x3b6e20c8UL, 0x26d930acUL, 0x76dc4190UL, 0x6b6b51f4UL, 0x4db26158UL, 0x5005713cUL,
										   0xedb88320UL, 0xf00f9344UL, 0xd6d6a3e8UL, 0xcb61b38cUL, 0x9b64c2b0UL, 0x86d3d2d4UL, 0xa00ae278UL, 0xbdbdf21cUL};
	crc = ~crc;
	for (size_t i = 0; i < length; ++i)
	{
		crc ^= data[i];
		crc = (crc >> 4U) ^ table[crc & 0x0fU];
		crc = (crc >> 4U) ^ table[crc & 0x0fU];
	}
	return ~crc;
}

/**
 * @brief Writes a frame header to out[0 .. headerSize - 1].
 */
inline void encodeHeader(uint8_t* const out, const Type type, const uint16_t length, const uint16_t seq)
{
	putU16(out, frameMagic);
	out[2] = frameVersion;
	out[3] = toByte(type);
	putU16(out + 4, length);
	putU16(out + 6, seq);
}

/**
 * @brief Reads a frame header.
 * @return false on a bad magic or an unknown frame version.
 */
inline bool decodeHeader(const uint8_t* const in, FrameHeader& header)
{
	if (getU16(in) != frameMagic || in[2] != frameVersion)
		return false;
	header.type = static_cast<Type>(in[3]);
	header.length = getU16(in + 4);
	header.seq = getU16(in + 6);
	return true;
}

/**
 * @brief Appends the CRC to a frame whose header and payload are already in place.
 * @return The size of the whole frame.
 */
inline size_t sealFrame(uint8_t* const frame, const uint16_t length)
{
	const size_t covered = headerSize + length;
	putU32(frame + covered, crc32(0, frame, covered));
	return covered + crcSize;
}

/**
 * @brief Checks the CRC of a complete frame with a payload of length bytes.
 */
inline bool checkFrame(const uint8_t* const frame, const uint16_t length)
{
	const size_t covered = headerSize + length;
	return getU32(frame + covered) == crc32(0, frame, covered);
}
}  // namespace NetworkProbe

#endif /* NETWORK_PROBE_PROTO_HPP */
