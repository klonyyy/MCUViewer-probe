/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PAIRING_HPP
#define PAIRING_HPP

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "EspNowPairing.hpp"
#include "NetworkProbeProto.hpp"

/**
 * @brief The dongles paired with the probe, kept in NVS, and the probe's side of pairing (EspNowPairing.hpp), for the
 * net task.
 *
 * A pairing request from a dongle waits for a short press of the BOOT button, which the Wi-Fi task polls and hands over
 * with notifyPress(); the LED blinks yellow meanwhile. The paired dongles are pinned in EspNowPort as encrypted peers.
 * The pairings live in their own NVS namespace, which forgetting the networks leaves alone; only eraseAll() (the BOOT
 * button held for 10 s, then pressed briefly: notifyErase()), forget() and pairing a dongle again change them.
 */
class Pairing
{
   public:
	static constexpr size_t maxDongles = NetworkProbe::maxPairedDongles;

	/**
	 * @brief Loads the pairings and pins their dongles. Call once, from the net task, after EspNowPort::start().
	 * @param wakeFd eventfd of the net task, written by notifyPress() and notifyErase().
	 */
	void start(int wakeFd);

	/**
	 * @return The pair key of mac, or nullptr when it is not paired.
	 */
	const uint8_t* findKey(const uint8_t* mac) const;

	/**
	 * @return Whether any dongle is paired: then only paired dongles get sessions.
	 */
	bool isLocked() const { return count > 0; }

	/**
	 * @brief The pairing byte of a DISCOVER answer.
	 * @param mac The dongle it goes to; nullptr over UDP.
	 */
	NetworkProbe::Pairing stateFor(const uint8_t* mac) const;

	size_t getCount() const { return count; }
	const uint8_t* getMac(size_t index) const { return dongles[index].mac; }

	/**
	 * @brief Forgets a dongle.
	 * @param keepPeer The dongle of the session: its peer stays encrypted until releaseForgotten(), at the session's end.
	 * @return Whether it was paired.
	 */
	bool forget(const uint8_t* mac, bool keepPeer);

	/**
	 * @brief Makes the peer of a dongle forgotten during its session an ordinary one.
	 */
	void releaseForgotten();

	/**
	 * @brief Forgets every dongle; a pairing under way ends too.
	 */
	void eraseAll();

	/**
	 * @brief Takes a PAIR packet addressed to the probe.
	 * @return Whether a dongle was paired just now.
	 */
	bool handlePacket(const uint8_t* mac, const uint8_t* data, size_t length, int64_t now);

	/**
	 * @return The dongle waiting for the BOOT button, or nullptr.
	 */
	const uint8_t* getWaitingDongle() const;

	/**
	 * @brief The BOOT button was pressed briefly: accepts the dongle waiting for it, if one does. Its old pairing, if it
	 * had one, is gone from now on.
	 */
	void confirm(int64_t now);

	/**
	 * @brief Ends a pairing that ran out of time.
	 */
	void service(int64_t now);

	/**
	 * @return Whether a pairing is under way, which service() must watch.
	 */
	bool isBusy() const { return phase != Phase::idle; }

	/* Any task: the BOOT button */
	static void notifyPress();
	static void notifyErase();
	static bool takePress();
	static bool takeErase();

   private:
	struct Dongle
	{
		uint8_t mac[NetworkProbe::macSize];
		uint8_t key[EspNowLink::keySize];
	};

	enum class Phase : uint8_t
	{
		idle,
		waiting,  /**< For the BOOT button. */
		accepted, /**< ACCEPT sent, waiting for DONE. */
		paired,	  /**< Saved: a DONE sent again gets PAIRED again. */
		conflict, /**< Two dongles asked: every request is refused until the window ends. */
	};

	void load();
	bool save() const;
	int find(const uint8_t* mac) const;
	bool add(const uint8_t* mac, const uint8_t* key);
	void pinDongle(const Dongle& dongle);
	void onRequest(const uint8_t* mac, const uint8_t* publicKey, int64_t now);

	/**
	 * @return Whether the dongle got paired.
	 */
	bool onDone(const uint8_t* mac, const uint8_t* confirmField, int64_t now);
	void send(const uint8_t* mac, const uint8_t* data, size_t length);
	void sendSignal(const uint8_t* mac, EspNowLink::PairStep step);
	void sendAccept();
	void refuse(const uint8_t* mac, NetworkProbe::PairFailure reason);
	void finish();
	static void wakeNet();

	static std::atomic<bool> pressPending;
	static std::atomic<bool> erasePending;
	static std::atomic<int> wakeFd;

	Dongle dongles[maxDongles] = {};
	size_t count = 0;
	uint8_t forgottenMac[NetworkProbe::macSize] = {}; /**< Forgotten during its session, still pinned. */
	bool forgottenPinned = false;

	Phase phase = Phase::idle;
	int64_t phaseEnd = 0;
	uint8_t pendingMac[NetworkProbe::macSize] = {};
	uint8_t donglePublic[EspNowLink::publicKeySize] = {};
	EspNowLink::KeyPair keyPair; /**< The probe's, from the press until DONE. */
	uint8_t pendingKey[EspNowLink::keySize] = {};
	uint8_t probeConfirm[EspNowLink::confirmSize] = {};
};

#endif /* PAIRING_HPP */
