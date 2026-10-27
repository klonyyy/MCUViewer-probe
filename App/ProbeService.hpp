/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PROBESERVICE_HPP
#define PROBESERVICE_HPP

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "HostLink.hpp"
#include "TargetLink.hpp"

/**
 * @brief The SWD owner task's side of the NETWORK PROBE protocol: executes the requests the net task hands over through
 * HostLink, and runs the streaming loop, which samples the plan and fills the ring.
 *
 * While streaming, the loop polls HostLink between samples and executes requests there, so a READ or WRITE delays the
 * next sample instead of running in parallel with the plan. A READ_BULK takes the samples that come due between its
 * 1 KiB pages.
 */
class ProbeService
{
   public:
	/**
	 * @brief Allocates the sample ring and attaches to the link. Call once, from the SWD owner task, before the net task
	 * starts.
	 */
	void init(HostLink& hostLink);

	/**
	 * @brief Handles everything that is pending: the end of a session and requests. While streaming it returns only
	 * once streaming stopped. Call from the SWD owner task whenever it is woken.
	 */
	void service();

   private:
	static constexpr uint32_t maxFailedSamples = 100; /**< Failed samples in a row after which streaming stops. */
	static constexpr uint32_t quiesceTimeoutMs = 500;

	/**
	 * @brief Stops streaming and releases the target if the session changed since the last call.
	 * @return True if a session ended.
	 */
	bool checkSession();

	void stopStreaming();

	/**
	 * @brief Executes the request in the slot, writes the response and hands it back to the net task.
	 */
	void handleRequest();

	/* Request handlers: they write the response data at link->responseData, set dataLength, return the status */
	NetworkProbe::Status handleConnect(const uint8_t* payload, uint16_t length, uint16_t& dataLength);
	NetworkProbe::Status handleDisconnect(uint16_t length);
	NetworkProbe::Status handleRead(const uint8_t* payload, uint16_t length, uint16_t& dataLength);
	NetworkProbe::Status handleWrite(const uint8_t* payload, uint16_t length, uint16_t& dataLength);
	NetworkProbe::Status handleReadList(const uint8_t* payload, uint16_t length, uint16_t& dataLength);
	NetworkProbe::Status handleReadBulk(const uint8_t* payload, uint16_t length, uint16_t& dataLength);
	NetworkProbe::Status handleReset(uint16_t length, uint16_t& dataLength);
	NetworkProbe::Status handleGetStatus(uint16_t length, uint16_t& dataLength);
	NetworkProbe::Status handleSetPlan(const uint8_t* payload, uint16_t length);
	NetworkProbe::Status handleStart(uint16_t length);
	NetworkProbe::Status handleStop(uint16_t length);

	/**
	 * @brief Maps the last TargetLink failure onto a status; for TARGET_ERROR also writes the fault byte.
	 */
	NetworkProbe::Status targetFailure(uint16_t& dataLength);

	/**
	 * @brief Waits until the net task has taken a chunk of the READ_BULK answer, sampling meanwhile while streaming.
	 * @return The chunk to fill next, or nullptr when the session ended.
	 */
	HostLink::BulkChunk* nextBulkChunk();

	/**
	 * @brief The sampling loop, until streaming stops.
	 */
	void stream();

	/**
	 * @brief Takes the next sample if its time has come.
	 */
	void sampleIfDue();

	/**
	 * @brief Takes one sample into the ring.
	 * @param time Microseconds since START.
	 */
	void takeSample(uint32_t time);

	/**
	 * @brief Lets the net task discard the ring, so its slot size can change.
	 * @return False when the net task did not answer in time.
	 */
	bool quiesceRing();

	HostLink* link = nullptr;
	uint32_t handledGeneration = 0;

	/* Plan, valid when blockCount > 0 */
	std::array<TargetLink::Block, HostLink::maxBlocks> blocks;
	size_t blockCount = 0;
	uint16_t planId = 0;
	uint32_t periodUs = 0;
	uint16_t bytesPerSample = 0; /**< Sample record with t and padding. */
	size_t dataBytes = 0;		 /**< Data of all blocks, without t and padding. */

	/* Streaming */
	bool streaming = false;
	int64_t startTime = 0;
	int64_t nextTime = 0;
	uint32_t sampleIndex = 0;
	uint32_t failedInARow = 0;
};

#endif /* PROBESERVICE_HPP */
