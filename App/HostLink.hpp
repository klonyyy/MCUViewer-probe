/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HOSTLINK_HPP
#define HOSTLINK_HPP

#include <stdint.h>
#include <unistd.h>

#include <atomic>

#include "SampleRing.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "NetworkProbeProto.hpp"

/**
 * @brief State shared by the net task (core 0), which talks to the host, and the SWD owner task (core 1), which talks
 * to the target. Each field says which side writes it. Lock-free, as the SWD owner task polls it between samples.
 *
 * Requests: one at a time. The net task copies a request in and sets slotState to request; the SWD owner task writes
 * the response and sets it to response; the net task sends it and sets it back to empty.
 *
 * Sessions: sessionGeneration counts the ended client sessions. When it changed, the SWD owner task stops streaming and
 * releases the target before it handles anything else, and samples of an older session are never sent.
 */
struct HostLink
{
	static constexpr uint16_t maxPayload = 4096 + 64; /**< Leaves room for the WRITE and RESP headers. */
	static constexpr uint16_t maxRead = 4096;		  /**< Largest READ and WRITE. */
	static constexpr uint8_t maxList = 255;			  /**< READ_LIST entries: count is a u8. */
	static constexpr uint8_t maxBlocks = 128;		  /**< SET_PLAN blocks. */
	static constexpr uint16_t maxSampleBytes = 1024;  /**< Sample record, t included. */

	enum class SlotState : uint8_t
	{
		empty,
		request,
		response,
	};

	std::atomic<SlotState> slotState{SlotState::empty};

	/* Written by the net task while the slot is empty */
	NetworkProbe::Type requestType = NetworkProbe::Type::HELLO;
	uint16_t requestSeq = 0;
	uint16_t requestLength = 0;
	uint8_t requestPayload[maxPayload];

	/* Written by the SWD owner task while the slot holds a request */
	NetworkProbe::Status responseStatus = NetworkProbe::Status::OK;
	uint16_t responseLength = 0;
	uint8_t responseData[maxPayload - NetworkProbe::respHeaderSize];

	/* Net task */
	std::atomic<uint32_t> sessionGeneration{0}; /**< +1 when a client session ends. */
	std::atomic<bool> gdbRequested{false};		/**< A GDB client waits; set by the net task, cleared by the SWD owner task after the GDB session. */

	/* SWD owner task */
	std::atomic<bool> gdbActive{false};		   /**< A GDB session runs; the net task answers BUSY. */
	std::atomic<bool> streaming{false};		   /**< Streaming runs: the net task flushes the ring regularly. */
	std::atomic<uint32_t> streamGeneration{0}; /**< Session the ring's samples belong to. */
	std::atomic<uint16_t> streamPlanId{0};	   /**< Plan the ring's samples belong to. */
	std::atomic<uint32_t> droppedSamples{0};   /**< Since START: the ring was full. */
	std::atomic<uint32_t> sampleErrors{0};	   /**< Since START: the target access failed. */
	std::atomic<uint32_t> targetLostCount{0};  /**< +1 when streaming stopped after too many failed samples in a row. */

	/* Samples, SWD owner task to net task */
	SampleRing ring;

	/*
	 * Quiesce handshake, before the SWD owner task changes the ring's slot size: it increments ringRequest and waits
	 * until the net task has discarded what the ring holds and copied ringRequest to ringAck.
	 */
	std::atomic<uint32_t> ringRequest{0};
	std::atomic<uint32_t> ringAck{0};

	TaskHandle_t probeTask = nullptr; /**< SWD owner task, woken with a task notification. */
	int netWakeFd = -1;				  /**< eventfd the net task waits on. */

	void wakeProbe() { xTaskNotifyGive(probeTask); }

	void wakeNet()
	{
		const uint64_t one = 1;
		write(netWakeFd, &one, sizeof(one));
	}
};

#endif /* HOSTLINK_HPP */
