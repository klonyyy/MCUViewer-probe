/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: MIT
 *
 * Pairing of a dongle and a probe over ESP-NOW, and the tags of their sessions. Header-only C++17 on PSA Crypto, shared
 * by the probe and the dongle firmware.
 */

#ifndef ESPNOW_PAIRING_HPP
#define ESPNOW_PAIRING_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "EspNowLink.hpp"
#include "psa/crypto.h"

namespace EspNowLink
{
/*
 * Pairing: PAIR packets with session 0, broadcast, addressed in auth like every packet with session 0; after the header
 * u8 step, then the step's fields:
 *
 *   dongle -> probe   REQUEST  u8 dongle_public[32]   again every pairRequestIntervalMs until ACCEPT
 *   probe -> dongle   WAIT                             to each REQUEST while the probe waits for its BOOT button
 *   probe -> dongle   ACCEPT   u8 probe_public[32], u8 confirm[16]   once the button was pressed, and to each REQUEST after
 *   dongle -> probe   DONE     u8 confirm[16]          again every pairDoneIntervalMs until PAIRED
 *   probe -> dongle   PAIRED                           the probe saved the pairing, also to each DONE after
 *   probe -> dongle   REFUSE   u8 PairFailure
 *
 * The probe waits pairWindowMs for the press from the first REQUEST, and takes one dongle at a time: a REQUEST from a
 * second dongle in that time refuses both (CONFLICT), until the window ends. Both sides make an X25519 key pair for the
 * pairing, and the pair key is SHA-256 over a label, the shared secret, both public keys and both MAC addresses
 * (KeyPair::agree()); confirm proves to the other side that the key came out the same (deriveConfirm()). The probe
 * saves the pairing at DONE, the dongle at PAIRED. Every key after the pair key comes from it with SHA-256: the LMK of
 * the encrypted ESP-NOW peer (deriveLmk()), and the session tags (deriveTag()).
 *
 * A dongle and a probe meet nobody else in this: someone else in range during the press can get in between, the press
 * is the only proof. The probe's yellow blinking shows when that window is open.
 */
constexpr size_t keySize = 32;
constexpr size_t publicKeySize = 32;
constexpr size_t lmkSize = 16;
constexpr size_t confirmSize = 16;
constexpr uint32_t pairWindowMs = 30000;
constexpr uint32_t pairRequestIntervalMs = 1000;
constexpr uint32_t pairDoneIntervalMs = 250;
constexpr uint32_t pairDoneTimeoutMs = 5000;				  /**< From ACCEPT, on both sides. */
constexpr uint32_t pairTimeoutMs = pairWindowMs + 5000; /**< The dongle's, for the whole pairing. */

enum class PairStep : uint8_t
{
	REQUEST = 1,
	WAIT = 2,
	ACCEPT = 3,
	DONE = 4,
	PAIRED = 5,
	REFUSE = 6,
};

constexpr size_t pairRequestSize = 1 + publicKeySize;
constexpr size_t pairAcceptSize = 1 + publicKeySize + confirmSize;
constexpr size_t pairDoneSize = 1 + confirmSize;
constexpr size_t pairRefuseSize = 2;
constexpr size_t pairSignalSize = 1; /**< WAIT and PAIRED. */
constexpr size_t maxPairSize = pairAcceptSize;

/** Who sends: the tags of the two directions differ. */
enum class Direction : uint8_t
{
	toProbe = 1,
	toDongle = 2,
};

struct Part
{
	const void* data;
	size_t length;
};

/**
 * @brief SHA-256 over parts, one after the other, into out[0 .. 31].
 */
inline bool sha256(const std::initializer_list<Part> parts, uint8_t* const out)
{
	psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
	bool ok = psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
	for (const Part& part : parts)
		ok = ok && psa_hash_update(&operation, static_cast<const uint8_t*>(part.data), part.length) == PSA_SUCCESS;
	size_t length = 0;
	ok = ok && psa_hash_finish(&operation, out, 32, &length) == PSA_SUCCESS && length == 32;
	if (!ok)
		psa_hash_abort(&operation);
	return ok;
}

/**
 * @brief Compares secrets in a time that does not tell where they differ.
 */
inline bool sameBytes(const uint8_t* const a, const uint8_t* const b, const size_t length)
{
	uint8_t difference = 0;
	for (size_t i = 0; i < length; ++i)
		difference |= static_cast<uint8_t>(a[i] ^ b[i]);
	return difference == 0;
}

/**
 * @brief The LMK of the encrypted ESP-NOW peer of a pair.
 */
inline bool deriveLmk(const uint8_t* const key, uint8_t* const lmk)
{
	uint8_t digest[32];
	if (!sha256({{key, keySize}, {"lmk", 3}}, digest))
		return false;
	memcpy(lmk, digest, lmkSize);
	return true;
}

/**
 * @brief The confirm field of ACCEPT (from the probe) or DONE (from the dongle).
 */
inline bool deriveConfirm(const uint8_t* const key, const Direction direction, uint8_t* const confirm)
{
	const uint8_t role = NetworkProbe::toByte(direction);
	uint8_t digest[32];
	if (!sha256({{key, keySize}, {"confirm", 7}, {&role, 1}}, digest))
		return false;
	memcpy(confirm, digest, confirmSize);
	return true;
}

constexpr uint32_t controlEpoch = UINT32_MAX; /**< The epoch of the tag of PING and CLOSE. */

/**
 * @brief A session tag: of the DATA in one wrap (epoch) of the numbers in one direction, or with controlEpoch of the
 * other kinds.
 */
inline bool deriveTag(const uint8_t* const key, const Direction direction, const uint16_t session, const uint32_t epoch, uint8_t* const tag)
{
	uint8_t fields[7];
	fields[0] = NetworkProbe::toByte(direction);
	NetworkProbe::putU16(fields + 1, session);
	NetworkProbe::putU32(fields + 3, epoch);
	uint8_t digest[32];
	if (!sha256({{key, keySize}, {"tag", 3}, {fields, sizeof(fields)}}, digest))
		return false;
	memcpy(tag, digest, authSize);
	return true;
}

/**
 * @brief An X25519 key pair for one pairing.
 */
class KeyPair
{
   public:
	KeyPair() = default;
	KeyPair(const KeyPair&) = delete;
	KeyPair& operator=(const KeyPair&) = delete;
	~KeyPair() { clear(); }

	bool generate()
	{
		clear();
		psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
		psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
		psa_set_key_bits(&attributes, 255);
		psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
		psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
		if (psa_generate_key(&attributes, &id) != PSA_SUCCESS)
			return false;
		ready = true;
		size_t length = 0;
		if (psa_export_public_key(id, publicKey, sizeof(publicKey), &length) != PSA_SUCCESS || length != publicKeySize)
		{
			clear();
			return false;
		}
		return true;
	}

	void clear()
	{
		if (ready)
			psa_destroy_key(id);
		ready = false;
	}

	bool isReady() const { return ready; }
	const uint8_t* getPublic() const { return publicKey; }

	/**
	 * @brief The pair key, from the other side's public key.
	 * @param ownIsDongle Whether this key pair is the dongle's.
	 */
	bool agree(const uint8_t* const peerPublic, const bool ownIsDongle, const uint8_t* const dongleMac, const uint8_t* const probeMac, uint8_t* const key) const
	{
		uint8_t shared[32];
		size_t length = 0;
		if (!ready || psa_raw_key_agreement(PSA_ALG_ECDH, id, peerPublic, publicKeySize, shared, sizeof(shared), &length) != PSA_SUCCESS || length != sizeof(shared))
			return false;
		static constexpr char label[] = "MCUViewer NETWORK PROBE ESP-NOW pairing 1";
		const uint8_t* const donglePublic = ownIsDongle ? publicKey : peerPublic;
		const uint8_t* const probePublic = ownIsDongle ? peerPublic : publicKey;
		const bool ok = sha256({{label, sizeof(label) - 1}, {shared, sizeof(shared)}, {donglePublic, publicKeySize}, {probePublic, publicKeySize}, {dongleMac, NetworkProbe::macSize}, {probeMac, NetworkProbe::macSize}}, key);
		memset(shared, 0, sizeof(shared));
		return ok;
	}

   private:
	mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
	bool ready = false;
	uint8_t publicKey[publicKeySize] = {};
};

/**
 * @brief One side of a session: the tags of the packets it sends, and the checks of those it gets: the seq of a DATA
 * must follow the last one taken within seqWindow and, in a paired session, its tag must be right.
 *
 * The DATA numbers of a direction count on past 16 bits: the sender passes its 32-bit count to stamp(), the receiver
 * works it out from seq. The upper half is the epoch the DATA tag goes with, so a DATA of an earlier wrap sent again
 * carries the wrong tag.
 */
class SessionAuth
{
   public:
	enum class Verdict : uint8_t
	{
		take,	/**< New, and as it should be. */
		repeat, /**< The last one taken, sent again: its first copy came through. */
		drop,
	};

	/**
	 * @brief A session of an unpaired dongle: packets carry no tag.
	 */
	void startOpen()
	{
		paired = false;
		rxNumber = 0;
	}

	/**
	 * @return False when a tag could not be computed.
	 */
	bool startPaired(const uint8_t* const pairKey, const uint16_t session, const Direction sending)
	{
		memcpy(key, pairKey, keySize);
		sessionNumber = session;
		txDirection = sending;
		rxDirection = sending == Direction::toProbe ? Direction::toDongle : Direction::toProbe;
		rxNumber = 0;
		txEpochValid = false;
		rxEpochValid = false;
		paired = deriveTag(key, txDirection, session, controlEpoch, txControl) && deriveTag(key, rxDirection, session, controlEpoch, rxControl);
		return paired;
	}

	bool isPaired() const { return paired; }

	/**
	 * @brief Fills in the auth of a packet of the session: zeros without pairing, else the tag; a DATA's of number, the
	 * sender's 32-bit count whose lower half is its seq.
	 */
	void stamp(Header& header, const uint32_t number = 0)
	{
		if (!paired)
		{
			memset(header.auth, 0, authSize);
			return;
		}
		if (header.kind != Kind::DATA)
		{
			memcpy(header.auth, txControl, authSize);
			return;
		}
		const uint32_t epoch = number >> 16U;
		if (!txEpochValid || epoch != txEpoch)
		{
			txEpochValid = deriveTag(key, txDirection, sessionNumber, epoch, txTag);
			txEpoch = epoch;
		}
		memcpy(header.auth, txTag, authSize);
	}

	/**
	 * @brief Checks a DATA of the session, and takes it if it is new.
	 */
	Verdict checkData(const Header& header)
	{
		const uint16_t ahead = static_cast<uint16_t>(header.seq - static_cast<uint16_t>(rxNumber));
		if (ahead > seqWindow)
			return Verdict::drop;
		const uint32_t number = rxNumber + ahead;
		if (paired && !rxTagMatches(number >> 16U, header.auth))
			return Verdict::drop;
		if (ahead == 0)
			return Verdict::repeat;
		rxNumber = number;
		return Verdict::take;
	}

	/**
	 * @brief Checks a PING or CLOSE of the session.
	 */
	bool checkControl(const Header& header) const { return !paired || sameBytes(header.auth, rxControl, authSize); }

	/**
	 * @return The seq of the last DATA taken, 0 before the first.
	 */
	uint16_t lastTaken() const { return static_cast<uint16_t>(rxNumber); }

   private:
	bool rxTagMatches(const uint32_t epoch, const uint8_t* const tag)
	{
		if (!rxEpochValid || epoch != rxEpoch)
		{
			rxEpochValid = deriveTag(key, rxDirection, sessionNumber, epoch, rxTag);
			rxEpoch = epoch;
			if (!rxEpochValid)
				return false;
		}
		return sameBytes(tag, rxTag, authSize);
	}

	bool paired = false;
	uint8_t key[keySize] = {};
	uint16_t sessionNumber = 0;
	Direction txDirection = Direction::toProbe;
	Direction rxDirection = Direction::toDongle;
	uint32_t rxNumber = 0;
	uint8_t txControl[authSize] = {};
	uint8_t rxControl[authSize] = {};
	uint32_t txEpoch = 0;
	bool txEpochValid = false;
	uint8_t txTag[authSize] = {};
	uint32_t rxEpoch = 0;
	bool rxEpochValid = false;
	uint8_t rxTag[authSize] = {};
};
}  // namespace EspNowLink

#endif /* ESPNOW_PAIRING_HPP */
