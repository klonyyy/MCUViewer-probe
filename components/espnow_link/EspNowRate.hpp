/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: MIT
 *
 * The PHY rate of ESP-NOW unicasts. ESP-NOW sends to a peer at one fixed rate, 1 Mb/s unless set, and never adapts it,
 * so each sender moves its unicasts up and down a ladder of rates by itself, from what its radio gets acknowledged.
 * Header-only C++17, shared by the probe and the dongle firmware.
 */

#ifndef ESPNOW_RATE_HPP
#define ESPNOW_RATE_HPP

#include <algorithm>
#include <cstdint>

#include "esp_now.h"
#include "esp_wifi_types.h"

namespace EspNowLink
{
/*
 * The rungs, measured in Tools/espnow_bench/RESULTS.md: MCS3 (16-QAM) carries about 2 MB/s, 11 Mb/s about 1 MB/s.
 * Espressif's long range mode at 500 kb/s carries about 50 kB/s, with the most range, but it is left out for now: a
 * 1470-byte packet takes about 24 ms at it, so a probe with data to send keeps its transmitter on at full power nearly
 * all the time, and the supply of the probes at hand gives out after a second or so of it. Behind two walls they reset
 * over and over. With a supply that carries it, it goes back in as the last rung; WIFI_PROTOCOL_LR stays on for it.
 * 1 Mb/s is left out too: at it the sender never heard an acknowledgement.
 *
 * A sender steps down a rung when stepDownAfter packets in a row went unacknowledged, and climbs back once it held a
 * rung for a while: minHoldMs at first. A rung climbed to steps down again at its first failure; when it did not last
 * the hold, the next climb waits twice as long, up to maxHoldMs, so a rate that works now and then is not retried at
 * every packet.
 *
 * Quiet: at the lowest rung a try takes long, with the radio's own retries in it, and tries back to back keep the
 * transmitter on at full power, more than the supply of a probe may carry: one that powers its target too lost power
 * after about a second of it. So a sender whose try at the lowest rung got no acknowledgement waits before the next
 * one, quietFactor times as long as the try took and at least minQuietMs, or until it hears from the peer, which shows
 * that the peer is there. The transmitter is then on for at most a fifth of the time while the peer does not answer.
 */
struct Rate
{
	const char* name;
	wifi_phy_mode_t mode;
	wifi_phy_rate_t rate;
	uint8_t halfMbps; /**< The rate in 0.5 Mb/s, as GET_STATUS and DONGLE_STATUS tell it. */
};

constexpr Rate rates[] = {
	{"MCS3", WIFI_PHY_MODE_HT20, WIFI_PHY_RATE_MCS3_LGI, 52},
	{"11M", WIFI_PHY_MODE_11B, WIFI_PHY_RATE_11M_S, 22},
	/* {"LR500", WIFI_PHY_MODE_LR, WIFI_PHY_RATE_LORA_500K, 1}: out for now, see above */
};
constexpr uint8_t rungCount = sizeof(rates) / sizeof(rates[0]);
constexpr uint8_t stepDownAfter = 2;
constexpr uint32_t minHoldMs = 1000;
constexpr uint32_t maxHoldMs = 32000;
constexpr uint32_t minQuietMs = 100;
constexpr int64_t quietFactor = 4;
/** For esp_wifi_set_protocol() on the station interface, which ESP-NOW sends on: every rung, and long range. */
constexpr uint8_t protocols = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR;

/**
 * @brief The rung a sender sends to one peer at.
 */
class RateLadder
{
   public:
	/**
	 * @brief Back to the top rung, as for a peer not sent to before.
	 */
	void reset() { *this = RateLadder{}; }

	/**
	 * @brief Takes the outcome of a unicast to the peer.
	 * @param acknowledged Whether the peer's radio acknowledged it.
	 * @return Whether the rung changed: the peer's rate must be set again, with setPeerRate().
	 */
	bool update(const bool acknowledged, const int64_t nowUs)
	{
		const bool held = nowUs - changedUs >= static_cast<int64_t>(holdMs) * 1000;
		if (acknowledged)
		{
			failures = 0;
			trial = false;
			if (rung == 0 || !held)
				return false;
			rung--;
			trial = true;
			climbed = true;
			changedUs = nowUs;
			return true;
		}
		if (rung == rungCount - 1 || ++failures < (trial ? 1 : stepDownAfter))
			return false;
		holdMs = climbed && !held ? std::min(holdMs * 2, maxHoldMs) : minHoldMs;
		rung++;
		failures = 0;
		trial = false;
		climbed = false;
		changedUs = nowUs;
		return true;
	}

	uint8_t getRung() const { return rung; }

	/**
	 * @brief Whether a try that failed here is followed by quiet, see quietUs().
	 */
	bool atBottom() const { return rung == rungCount - 1; }

   private:
	uint8_t rung = 0;
	uint8_t failures = 0; /**< In a row, on this rung. */
	bool trial = false;	  /**< Climbed to, and nothing acknowledged on it yet. */
	bool climbed = false; /**< Reached by climbing, not by stepping down. */
	uint32_t holdMs = minHoldMs;
	int64_t changedUs = 0;
};

/**
 * @brief How long a sender stays quiet after a try at the lowest rung that got no acknowledgement.
 * @param tryUs How long the try took, from esp_now_send() to its send callback.
 */
inline int64_t quietUs(const int64_t tryUs)
{
	return std::max(static_cast<int64_t>(minQuietMs) * 1000, quietFactor * tryUs);
}

/**
 * @brief Sets the rate ESP-NOW sends to a peer at: a rung of the ladder.
 */
inline esp_err_t setPeerRate(const uint8_t* const mac, const uint8_t rung)
{
	esp_now_rate_config_t config = {};
	config.phymode = rates[rung].mode;
	config.rate = rates[rung].rate;
	return esp_now_set_peer_rate_config(mac, &config);
}
}  // namespace EspNowLink

#endif /* ESPNOW_RATE_HPP */
