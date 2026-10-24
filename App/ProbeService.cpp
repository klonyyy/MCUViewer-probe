/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ProbeService.hpp"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

extern "C"
{
#include "general.h"
#include "platform.h"
}

namespace
{
constexpr const char* tag = "probe_service";

/* A ring slot: u32 sample index for the net task, then the sample record as DATA carries it: u32 t, data, padding */
constexpr size_t slotIndexSize = 4;
constexpr size_t fallbackRingBytes = 32 * 1024;
}  // namespace

void ProbeService::init(HostLink& hostLink)
{
	link = &hostLink;

	size_t ringBytes = static_cast<size_t>(CONFIG_NETWORK_PROBE_STREAM_BUFFER_KB) * 1024U;
	uint8_t* storage = static_cast<uint8_t*>(heap_caps_malloc(ringBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
	if (!storage)
	{
		ESP_LOGW(tag, "no PSRAM for a %u KiB sample buffer, using %u KiB of internal RAM", CONFIG_NETWORK_PROBE_STREAM_BUFFER_KB, fallbackRingBytes / 1024U);
		ringBytes = fallbackRingBytes;
		storage = static_cast<uint8_t*>(heap_caps_malloc(ringBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
	}
	if (!storage)
		ringBytes = 0;
	link->ring.init(storage, ringBytes);
	ESP_LOGI(tag, "sample buffer %u KiB", ringBytes / 1024U);
}

void ProbeService::service()
{
	checkSession();
	while (link->slotState.load(std::memory_order_acquire) == HostLink::SlotState::request)
	{
		checkSession();
		handleRequest();
		if (streaming)
			stream();
	}
}

bool ProbeService::checkSession()
{
	const uint32_t generation = link->sessionGeneration.load(std::memory_order_acquire);
	if (generation == handledGeneration)
		return false;
	handledGeneration = generation;
	stopStreaming();
	blockCount = 0;
	TargetLink::disconnect();
	return true;
}

void ProbeService::stopStreaming()
{
	if (!streaming)
		return;
	ESP_LOGI(tag, "streaming stopped after %" PRIu32 " samples", sampleIndex);
	streaming = false;
	link->streaming.store(false, std::memory_order_release);
	/* The net task flushes what is left in the ring */
	link->wakeNet();
}

void ProbeService::handleRequest()
{
	const uint8_t* const payload = link->requestPayload;
	const uint16_t length = link->requestLength;
	uint16_t dataLength = 0;
	NetworkProbe::Status status = NetworkProbe::Status::UNSUPPORTED;

	switch (link->requestType)
	{
		case NetworkProbe::Type::CONNECT:
			status = handleConnect(payload, length, dataLength);
			break;
		case NetworkProbe::Type::DISCONNECT:
			status = handleDisconnect(length);
			break;
		case NetworkProbe::Type::READ:
			status = handleRead(payload, length, dataLength);
			break;
		case NetworkProbe::Type::WRITE:
			status = handleWrite(payload, length, dataLength);
			break;
		case NetworkProbe::Type::READ_LIST:
			status = handleReadList(payload, length, dataLength);
			break;
		case NetworkProbe::Type::RESET:
			status = handleReset(length, dataLength);
			break;
		case NetworkProbe::Type::GET_STATUS:
			status = handleGetStatus(length, dataLength);
			break;
		case NetworkProbe::Type::SET_PLAN:
			status = handleSetPlan(payload, length);
			break;
		case NetworkProbe::Type::START:
			status = handleStart(length);
			break;
		case NetworkProbe::Type::STOP:
			status = handleStop(length);
			break;
		default:
			break;
	}

	ESP_LOGD(tag, "request 0x%02x seq %u: status %u, %u data bytes", NetworkProbe::toByte(link->requestType), link->requestSeq, NetworkProbe::toByte(status), dataLength);
	link->responseStatus = status;
	link->responseLength = dataLength;
	link->slotState.store(HostLink::SlotState::response, std::memory_order_release);
	link->wakeNet();
}

NetworkProbe::Status ProbeService::handleConnect(const uint8_t* const payload, const uint16_t length, uint16_t& dataLength)
{
	if (length != NetworkProbe::connectRequestSize || payload[5] != 0)
		return NetworkProbe::Status::MALFORMED;

	stopStreaming();
	TargetLink::ConnectInfo info = {};
	if (!TargetLink::connect(NetworkProbe::getU32(payload), payload[4], info))
		return targetFailure(dataLength);

	uint8_t* const data = link->responseData;
	NetworkProbe::putU32(data, info.dpidr);
	NetworkProbe::putU32(data + 4, info.targetid);
	NetworkProbe::putU32(data + 8, info.apIdr);
	NetworkProbe::putU32(data + 12, info.frequency);
	dataLength = NetworkProbe::connectResponseSize;
	return NetworkProbe::Status::OK;
}

NetworkProbe::Status ProbeService::handleDisconnect(const uint16_t length)
{
	if (length != 0)
		return NetworkProbe::Status::MALFORMED;
	stopStreaming();
	TargetLink::disconnect();
	return NetworkProbe::Status::OK;
}

NetworkProbe::Status ProbeService::handleRead(const uint8_t* const payload, const uint16_t length, uint16_t& dataLength)
{
	if (length != NetworkProbe::readRequestSize)
		return NetworkProbe::Status::MALFORMED;
	const uint32_t address = NetworkProbe::getU32(payload);
	const uint16_t readLength = NetworkProbe::getU16(payload + 4);
	if (readLength == 0 || readLength > HostLink::maxRead)
		return NetworkProbe::Status::MALFORMED;

	if (!TargetLink::read(address, link->responseData, readLength))
		return targetFailure(dataLength);
	dataLength = readLength;
	return NetworkProbe::Status::OK;
}

NetworkProbe::Status ProbeService::handleWrite(const uint8_t* const payload, const uint16_t length, uint16_t& dataLength)
{
	if (length < NetworkProbe::writeRequestHeaderSize)
		return NetworkProbe::Status::MALFORMED;
	const uint32_t address = NetworkProbe::getU32(payload);
	const uint16_t writeLength = NetworkProbe::getU16(payload + 4);
	if (writeLength == 0 || writeLength > HostLink::maxRead || length != NetworkProbe::writeRequestHeaderSize + writeLength)
		return NetworkProbe::Status::MALFORMED;

	if (!TargetLink::write(address, payload + NetworkProbe::writeRequestHeaderSize, writeLength))
		return targetFailure(dataLength);
	return NetworkProbe::Status::OK;
}

NetworkProbe::Status ProbeService::handleReadList(const uint8_t* const payload, const uint16_t length, uint16_t& dataLength)
{
	static_assert(HostLink::maxList * (1U + 4U) <= sizeof(HostLink::responseData), "a READ_LIST response must fit");

	if (length < 1)
		return NetworkProbe::Status::MALFORMED;
	const uint8_t count = payload[0];
	if (count == 0 || length != 1U + count * NetworkProbe::readListEntrySize)
		return NetworkProbe::Status::MALFORMED;
	for (uint8_t i = 0; i < count; ++i)
	{
		const uint8_t size = payload[1U + i * NetworkProbe::readListEntrySize + 4U];
		if (size != 1 && size != 2 && size != 4)
			return NetworkProbe::Status::MALFORMED;
	}
	if (!TargetLink::isConnected())
		return NetworkProbe::Status::NOT_CONNECTED;

	uint8_t* out = link->responseData;
	for (uint8_t i = 0; i < count; ++i)
	{
		const uint8_t* const entry = payload + 1U + i * NetworkProbe::readListEntrySize;
		const uint8_t size = entry[4];
		const bool ok = TargetLink::read(NetworkProbe::getU32(entry), out + 1, size);
		out[0] = NetworkProbe::toByte(ok ? NetworkProbe::Status::OK : NetworkProbe::Status::TARGET_ERROR);
		if (!ok)
			memset(out + 1, 0, size);
		out += 1U + size;
	}
	dataLength = static_cast<uint16_t>(out - link->responseData);
	return NetworkProbe::Status::OK;
}

NetworkProbe::Status ProbeService::handleReset(const uint16_t length, uint16_t& dataLength)
{
	if (length != 0)
		return NetworkProbe::Status::MALFORMED;
	if (!TargetLink::reset())
		return targetFailure(dataLength);
	/* The samples taken during the reset failed; that is no reason to give up on the target */
	failedInARow = 0;
	return NetworkProbe::Status::OK;
}

NetworkProbe::Status ProbeService::handleGetStatus(const uint16_t length, uint16_t& dataLength)
{
	if (length != 0)
		return NetworkProbe::Status::MALFORMED;

	uint8_t* const data = link->responseData;
	memset(data, 0, NetworkProbe::statusResponseSize);
	data[0] = NetworkProbe::toByte(streaming ? NetworkProbe::Link::STREAMING : (TargetLink::isConnected() ? NetworkProbe::Link::CONNECTED : NetworkProbe::Link::DISCONNECTED));
	NetworkProbe::putU32(data + 4, platform_max_frequency_get());
	NetworkProbe::putU32(data + 8, TargetLink::getErrorCount());
	NetworkProbe::putU32(data + 12, static_cast<uint32_t>(esp_timer_get_time() / 1000));
	dataLength = NetworkProbe::statusResponseSize;
	return NetworkProbe::Status::OK;
}

NetworkProbe::Status ProbeService::handleSetPlan(const uint8_t* const payload, const uint16_t length)
{
	stopStreaming();
	blockCount = 0;

	if (length < NetworkProbe::planHeaderSize)
		return NetworkProbe::Status::MALFORMED;
	const uint8_t count = payload[6];
	if (count == 0 || count > HostLink::maxBlocks || length != NetworkProbe::planHeaderSize + count * NetworkProbe::planBlockSize)
		return NetworkProbe::Status::MALFORMED;

	size_t bytes = 0;
	for (uint8_t i = 0; i < count; ++i)
	{
		const uint8_t* const entry = payload + NetworkProbe::planHeaderSize + i * NetworkProbe::planBlockSize;
		TargetLink::Block block = {NetworkProbe::getU32(entry), NetworkProbe::getU16(entry + 4), entry[6]};
		if (block.accessSize != 1 && block.accessSize != 2 && block.accessSize != 4)
			return NetworkProbe::Status::MALFORMED;
		const uint32_t blockBytes = static_cast<uint32_t>(block.count) * block.accessSize;
		/* Aligned, and inside one 1 KiB page of TAR auto-increment. A bad address can otherwise stall in BMD's retries */
		if (block.count == 0 || block.address % block.accessSize != 0 || (block.address & ~(NetworkProbe::blockBoundary - 1U)) != ((block.address + blockBytes - 1U) & ~(NetworkProbe::blockBoundary - 1U)))
			return NetworkProbe::Status::MALFORMED;
		bytes += blockBytes;
		blocks[i] = block;
	}
	const size_t recordBytes = (NetworkProbe::sampleTimeSize + bytes + 3U) & ~static_cast<size_t>(3U);
	if (recordBytes > HostLink::maxSampleBytes)
		return NetworkProbe::Status::MALFORMED;

	planId = NetworkProbe::getU16(payload);
	periodUs = NetworkProbe::getU32(payload + 2);
	blockCount = count;
	dataBytes = bytes;
	bytesPerSample = static_cast<uint16_t>(recordBytes);
	ESP_LOGI(tag, "plan %u: %u blocks, %u bytes per sample, period %" PRIu32 " us", planId, blockCount, bytesPerSample, periodUs);
	return NetworkProbe::Status::OK;
}

NetworkProbe::Status ProbeService::handleStart(const uint16_t length)
{
	if (length != 0 || blockCount == 0)
		return NetworkProbe::Status::MALFORMED;
	if (!TargetLink::isConnected())
		return NetworkProbe::Status::NOT_CONNECTED;

	stopStreaming();
	/* Zero the counters before the handshake, so the net task never reports those of the previous run */
	link->droppedSamples.store(0, std::memory_order_relaxed);
	link->sampleErrors.store(0, std::memory_order_relaxed);
	if (!quiesceRing())
		return NetworkProbe::Status::BUSY;
	if (!link->ring.configure(slotIndexSize + bytesPerSample))
		return NetworkProbe::Status::MALFORMED;

	link->streamPlanId.store(planId, std::memory_order_relaxed);
	link->streamGeneration.store(handledGeneration, std::memory_order_relaxed);
	link->streaming.store(true, std::memory_order_release);
	streaming = true;
	sampleIndex = 0;
	failedInARow = 0;
	startTime = esp_timer_get_time();
	nextTime = startTime;
	ESP_LOGI(tag, "streaming plan %u", planId);
	return NetworkProbe::Status::OK;
}

NetworkProbe::Status ProbeService::handleStop(const uint16_t length)
{
	if (length != 0)
		return NetworkProbe::Status::MALFORMED;
	stopStreaming();
	return NetworkProbe::Status::OK;
}

NetworkProbe::Status ProbeService::targetFailure(uint16_t& dataLength)
{
	switch (TargetLink::getError())
	{
		case TargetLink::Error::notConnected:
			return NetworkProbe::Status::NOT_CONNECTED;
		case TargetLink::Error::noTarget:
		case TargetLink::Error::unsupportedDp:
		case TargetLink::Error::noPowerUp:
		case TargetLink::Error::noMemAp:
			return NetworkProbe::Status::NO_TARGET;
		default:
			link->responseData[0] = TargetLink::getFault() ? TargetLink::getFault() : NetworkProbe::toByte(NetworkProbe::Fault::OTHER);
			dataLength = 1;
			return NetworkProbe::Status::TARGET_ERROR;
	}
}

bool ProbeService::quiesceRing()
{
	const uint32_t request = link->ringRequest.fetch_add(1, std::memory_order_acq_rel) + 1U;
	link->wakeNet();
	for (uint32_t waited = 0; link->ringAck.load(std::memory_order_acquire) != request; ++waited)
	{
		if (waited >= quiesceTimeoutMs)
		{
			ESP_LOGW(tag, "the net task did not release the sample buffer");
			return false;
		}
		platform_delay(1);
	}
	return true;
}

void ProbeService::stream()
{
	while (streaming)
	{
		/* Between two samples: the session, requests, a waiting GDB client */
		if (checkSession() || link->gdbRequested.load(std::memory_order_acquire))
		{
			stopStreaming();
			break;
		}
		if (link->slotState.load(std::memory_order_acquire) == HostLink::SlotState::request)
		{
			handleRequest();
			continue;
		}

		const int64_t now = esp_timer_get_time();
		if (now < nextTime)
			continue;

		takeSample(static_cast<uint32_t>(now - startTime));

		/* Behind by a period or more: take the next sample at once, but do not catch up with a burst */
		nextTime += periodUs;
		if (nextTime <= now)
			nextTime = now;
	}
}

void ProbeService::takeSample(const uint32_t time)
{
	const uint32_t index = sampleIndex++;
	uint8_t* const slot = link->ring.beginWrite();
	if (!slot)
	{
		link->droppedSamples.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	NetworkProbe::putU32(slot, index);
	NetworkProbe::putU32(slot + slotIndexSize, time);
	uint8_t* const data = slot + slotIndexSize + NetworkProbe::sampleTimeSize;
	if (TargetLink::readBlocks(blocks.data(), blockCount, data))
	{
		memset(data + dataBytes, 0, bytesPerSample - NetworkProbe::sampleTimeSize - dataBytes);
		link->ring.commitWrite();
		failedInARow = 0;
		return;
	}

	link->sampleErrors.fetch_add(1, std::memory_order_relaxed);
	if (++failedInARow >= maxFailedSamples)
	{
		ESP_LOGW(tag, "%" PRIu32 " samples in a row failed, the target is lost", failedInARow);
		link->targetLostCount.fetch_add(1, std::memory_order_release);
		stopStreaming();
	}
}
