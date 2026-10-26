/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "NetServer.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>

#include <algorithm>

#include "NetworkSettings.hpp"
#include "SwdTap.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

namespace
{
constexpr const char* tag = "net_server";
constexpr const char* probeName = "NETWORK PROBE ESP32-S3";

/* Appends a string field: u8 length, then the bytes */
uint8_t* putString(uint8_t* out, const char* const text)
{
	const size_t length = std::min<size_t>(strlen(text), UINT8_MAX);
	*out++ = static_cast<uint8_t>(length);
	memcpy(out, text, length);
	return out + length;
}

void configureClientSocket(const int sock)
{
	const int one = 1;
	const int keepaliveIdle = 5;
	const int keepaliveInterval = 2;
	const int keepaliveCount = 3;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepaliveIdle, sizeof(keepaliveIdle));
	setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepaliveInterval, sizeof(keepaliveInterval));
	setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepaliveCount, sizeof(keepaliveCount));
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
}

bool isRequest(const NetworkProbe::Type type)
{
	return type < NetworkProbe::Type::RESP;
}
}  // namespace

void NetServer::start(HostLink& hostLink, const int gdbListen, SwoCapture& swoCapture)
{
	static_assert(NetworkProbe::helloResponseFixedSize + 3U * (1U + UINT8_MAX) + NetworkProbe::helloSwoSize <= localFrameSize - NetworkProbe::frameOverhead - NetworkProbe::respHeaderSize, "a HELLO answer must fit");
	static_assert(NetworkProbe::discoverResponseFixedSize + 3U * (1U + UINT8_MAX) <= localFrameSize - NetworkProbe::frameOverhead - NetworkProbe::respHeaderSize, "a DISCOVER answer must fit");
	static_assert(dataTargetPayload <= HostLink::maxPayload, "an SWO_DATA frame must fit");

	link = &hostLink;
	swo = &swoCapture;
	gdbListenSocket = gdbListen;

	NetworkSettings::getDeviceId(probeId, sizeof(probeId));

	const esp_vfs_eventfd_config_t eventfdConfig = ESP_VFS_EVENTD_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfdConfig));
	link->netWakeFd = eventfd(0, 0);
	if (link->netWakeFd < 0)
		ESP_LOGE(tag, "eventfd() failed: errno %d", errno);

	while (true)
	{
		listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listenSocket >= 0)
		{
			const int one = 1;
			setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

			sockaddr_in address = {};
			address.sin_family = AF_INET;
			address.sin_port = htons(CONFIG_NETWORK_PROBE_PORT);
			address.sin_addr.s_addr = htonl(INADDR_ANY);
			if (bind(listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 && listen(listenSocket, 1) == 0)
				break;
			close(listenSocket);
			listenSocket = -1;
		}
		ESP_LOGE(tag, "cannot listen on port %d: errno %d", CONFIG_NETWORK_PROBE_PORT, errno);
		vTaskDelay(pdMS_TO_TICKS(1000));
	}

	ESP_LOGI(tag, "NETWORK PROBE protocol listening on TCP port %d, id %s", CONFIG_NETWORK_PROBE_PORT, probeId);
	openDiscoverySocket();
	xTaskCreatePinnedToCore(taskEntry, "net", taskStackSize, this, taskPriority, nullptr, taskCore);
}

void NetServer::taskEntry(void* const arg)
{
	static_cast<NetServer*>(arg)->run();
}

void NetServer::run()
{
	/* Here, not in start(): the DMA interrupt is allocated on the calling core */
	swo->init();

	while (true)
	{
		serviceLink();
		swo->poll();
		if (clientSocket >= 0 && !pumpTx())
			closeClient();

		const bool watchGdb = !link->gdbRequested.load(std::memory_order_acquire) && !link->gdbActive.load(std::memory_order_acquire);
		fd_set readSet;
		fd_set writeSet;
		FD_ZERO(&readSet);
		FD_ZERO(&writeSet);
		FD_SET(listenSocket, &readSet);
		int maxSocket = listenSocket;
		if (link->netWakeFd >= 0)
		{
			FD_SET(link->netWakeFd, &readSet);
			maxSocket = std::max(maxSocket, link->netWakeFd);
		}
		if (watchGdb)
		{
			FD_SET(gdbListenSocket, &readSet);
			maxSocket = std::max(maxSocket, gdbListenSocket);
		}
		if (discoverySocket >= 0)
		{
			FD_SET(discoverySocket, &readSet);
			maxSocket = std::max(maxSocket, discoverySocket);
		}
		if (clientSocket >= 0)
		{
			FD_SET(clientSocket, &readSet);
			/* Only while a frame is stuck in a full window, else select() would return at once */
			if (txSent < txLength)
				FD_SET(clientSocket, &writeSet);
			maxSocket = std::max(maxSocket, clientSocket);
		}

		/* Samples and SWO bytes are flushed on time, so wake up regularly while they may come; the eventfd covers the rest */
		const bool samplesMayCome = clientSocket >= 0 && (link->streaming.load(std::memory_order_acquire) || link->ring.available() > 0 || swo->isCapturing());
		timeval timeout = {};
		timeout.tv_usec = pollMs * 1000U;
		const int ready = select(maxSocket + 1, &readSet, &writeSet, nullptr, samplesMayCome ? &timeout : nullptr);
		if (ready < 0)
		{
			ESP_LOGW(tag, "select() failed: errno %d", errno);
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}

		if (link->netWakeFd >= 0 && FD_ISSET(link->netWakeFd, &readSet))
		{
			uint64_t count = 0;
			read(link->netWakeFd, &count, sizeof(count));
		}
		if (FD_ISSET(listenSocket, &readSet))
			acceptClient();
		if (discoverySocket >= 0 && FD_ISSET(discoverySocket, &readSet))
			answerDiscovery();
		if (watchGdb && FD_ISSET(gdbListenSocket, &readSet))
		{
			if (clientSocket >= 0)
				refuseGdbClient();
			else
			{
				/* The SWD owner task accepts it; until its session ends, the GDB port is its own */
				link->gdbRequested.store(true, std::memory_order_release);
				link->wakeProbe();
			}
		}
		if (clientSocket >= 0 && FD_ISSET(clientSocket, &readSet) && !receive())
			closeClient();
	}
}

void NetServer::acceptClient()
{
	sockaddr_in peer;
	socklen_t peerLength = sizeof(peer);
	const int sock = accept(listenSocket, reinterpret_cast<sockaddr*>(&peer), &peerLength);
	if (sock < 0)
	{
		ESP_LOGW(tag, "accept() failed: errno %d", errno);
		return;
	}

	/* A new client takes over: the old connection may be a half-open leftover of a crashed host */
	if (clientSocket >= 0)
	{
		ESP_LOGI(tag, "a new client takes over the session");
		closeClient();
	}

	configureClientSocket(sock);
	char peerName[16];
	inet_ntoa_r(peer.sin_addr, peerName, sizeof(peerName));
	ESP_LOGI(tag, "client connected from %s", peerName);
	clientSocket = sock;
	rxLength = 0;
	txLength = 0;
	txSent = 0;
	localLength = 0;
	frameSeq = 0;
}

void NetServer::closeClient()
{
	if (clientSocket >= 0)
	{
		shutdown(clientSocket, SHUT_RDWR);
		close(clientSocket);
		ESP_LOGI(tag, "client disconnected");
	}
	clientSocket = -1;
	rxLength = 0;
	txLength = 0;
	txSent = 0;
	localLength = 0;
	if (requestOutstanding)
		dropResponse = true;
	swo->stop();

	/* The SWD owner task stops streaming and releases the target; the samples left belong to no one */
	link->sessionGeneration.fetch_add(1, std::memory_order_acq_rel);
	link->ring.discardAll();
	link->wakeProbe();
}

void NetServer::refuseGdbClient()
{
	const int sock = accept(gdbListenSocket, nullptr, nullptr);
	if (sock >= 0)
	{
		close(sock);
		ESP_LOGW(tag, "refused a GDB client: an MCUViewer session is active");
	}
}

void NetServer::openDiscoverySocket()
{
	discoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (discoverySocket >= 0)
	{
		/* Needed to receive broadcasts when lwIP filters them per socket */
		const int one = 1;
		setsockopt(discoverySocket, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons(NetworkProbe::discoveryPort);
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(discoverySocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
		{
			fcntl(discoverySocket, F_SETFL, fcntl(discoverySocket, F_GETFL, 0) | O_NONBLOCK);
			ESP_LOGI(tag, "answering discovery on UDP port %u", NetworkProbe::discoveryPort);
			return;
		}
		close(discoverySocket);
		discoverySocket = -1;
	}
	ESP_LOGE(tag, "cannot open UDP port %u for discovery: errno %d", NetworkProbe::discoveryPort, errno);
}

void NetServer::answerDiscovery()
{
	/* One byte more than a DISCOVER frame, so a longer datagram shows by its size */
	uint8_t request[NetworkProbe::frameOverhead + NetworkProbe::discoverRequestSize + 1];
	sockaddr_in peer;
	socklen_t peerLength = sizeof(peer);
	const ssize_t received = recvfrom(discoverySocket, request, sizeof(request), 0, reinterpret_cast<sockaddr*>(&peer), &peerLength);
	if (received != static_cast<ssize_t>(NetworkProbe::frameOverhead + NetworkProbe::discoverRequestSize))
		return;

	NetworkProbe::FrameHeader header;
	if (!NetworkProbe::decodeHeader(request, header) || header.type != NetworkProbe::Type::DISCOVER || header.length != NetworkProbe::discoverRequestSize)
		return;
	if (NetworkProbe::getU32(request + NetworkProbe::headerSize + header.length) != esp_rom_crc32_le(0, request, NetworkProbe::headerSize + header.length))
		return;

	uint8_t* const frame = discoveryFrame.data();
	frame[NetworkProbe::headerSize] = NetworkProbe::toByte(NetworkProbe::Type::DISCOVER);
	frame[NetworkProbe::headerSize + 1] = NetworkProbe::toByte(NetworkProbe::Status::OK);
	const uint16_t dataLength = buildDiscover(frame + NetworkProbe::headerSize + NetworkProbe::respHeaderSize);
	const size_t length = sealFrame(frame, NetworkProbe::Type::RESP, NetworkProbe::respHeaderSize + dataLength, header.seq);
	/* Non-blocking: with the send buffer full the answer is dropped, and the host asks again */
	sendto(discoverySocket, frame, length, 0, reinterpret_cast<const sockaddr*>(&peer), peerLength);

	char peerName[16];
	inet_ntoa_r(peer.sin_addr, peerName, sizeof(peerName));
	ESP_LOGD(tag, "answered DISCOVER from %s", peerName);
}

uint16_t NetServer::buildDiscover(uint8_t* const data) const
{
	NetworkProbe::Session session = NetworkProbe::Session::FREE;
	if (link->gdbRequested.load(std::memory_order_acquire) || link->gdbActive.load(std::memory_order_acquire))
		session = NetworkProbe::Session::GDB;
	else if (clientSocket >= 0)
		session = NetworkProbe::Session::HOST;

	NetworkProbe::putU16(data, NetworkProbe::protocolVersion);
	NetworkProbe::putU16(data + 2, CONFIG_NETWORK_PROBE_PORT);
	data[4] = NetworkProbe::toByte(session);
	data[5] = 0;

	uint8_t* out = data + NetworkProbe::discoverResponseFixedSize;
	out = putString(out, probeName);
	out = putString(out, esp_app_get_description()->version);
	out = putString(out, probeId);
	return static_cast<uint16_t>(out - data);
}

bool NetServer::receive()
{
	const ssize_t received = recv(clientSocket, rxBuffer.data() + rxLength, rxBuffer.size() - rxLength, 0);
	if (received < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
	if (received == 0)
		return false;
	rxLength += static_cast<size_t>(received);

	size_t offset = 0;
	while (rxLength - offset >= NetworkProbe::headerSize)
	{
		const uint8_t* const frame = rxBuffer.data() + offset;
		NetworkProbe::FrameHeader header;
		if (!NetworkProbe::decodeHeader(frame, header))
		{
			ESP_LOGW(tag, "bad frame magic or version, closing the connection");
			return false;
		}
		if (header.length > HostLink::maxPayload)
		{
			ESP_LOGW(tag, "frame payload of %u bytes exceeds %u, closing the connection", header.length, HostLink::maxPayload);
			return false;
		}

		const size_t frameSize = NetworkProbe::frameOverhead + header.length;
		if (rxLength - offset < frameSize)
			break;
		if (NetworkProbe::getU32(frame + NetworkProbe::headerSize + header.length) != esp_rom_crc32_le(0, frame, NetworkProbe::headerSize + header.length))
		{
			ESP_LOGW(tag, "frame CRC mismatch, closing the connection");
			return false;
		}
		if (!handleFrame(header, frame + NetworkProbe::headerSize))
			return false;
		offset += frameSize;
	}

	/* Keep the start of an incomplete frame; the buffer holds a whole frame of the largest size */
	memmove(rxBuffer.data(), rxBuffer.data() + offset, rxLength - offset);
	rxLength -= offset;
	return true;
}

bool NetServer::handleFrame(const NetworkProbe::FrameHeader& header, const uint8_t* const payload)
{
	if (header.type == NetworkProbe::Type::HELLO)
	{
		if (header.length != NetworkProbe::helloRequestSize)
			return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::MALFORMED, 0);
		ESP_LOGI(tag, "HELLO from a host speaking protocol %u", NetworkProbe::getU16(payload));
		return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::OK, buildHello(localData()));
	}
	if (!isRequest(header.type))
		return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::UNSUPPORTED, 0);
	if (link->gdbRequested.load(std::memory_order_acquire) || link->gdbActive.load(std::memory_order_acquire))
		return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::BUSY, 0);
	if (requestOutstanding || link->slotState.load(std::memory_order_acquire) != HostLink::SlotState::empty)
		return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::BUSY, 0);
	if (header.type == NetworkProbe::Type::SWO_START || header.type == NetworkProbe::Type::SWO_STOP)
		return handleSwoRequest(header, payload);

	link->requestType = header.type;
	link->requestSeq = header.seq;
	link->requestLength = header.length;
	memcpy(link->requestPayload, payload, header.length);
	requestOutstanding = true;
	dropResponse = false;
	link->slotState.store(HostLink::SlotState::request, std::memory_order_release);
	link->wakeProbe();
	return true;
}

bool NetServer::handleSwoRequest(const NetworkProbe::FrameHeader& header, const uint8_t* const payload)
{
	if (!swo->isAvailable())
		return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::UNSUPPORTED, 0);

	if (header.type == NetworkProbe::Type::SWO_STOP)
	{
		if (header.length != 0)
			return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::MALFORMED, 0);
		swo->stop();
		return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::OK, 0);
	}

	if (header.length != NetworkProbe::swoStartRequestSize || payload[5] != 0)
		return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::MALFORMED, 0);
	if (payload[4] != NetworkProbe::toByte(NetworkProbe::SwoEncoding::NRZ))
		return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::UNSUPPORTED, 0);
	const uint32_t baud = NetworkProbe::getU32(payload);
	if (baud < SwoCapture::minBaud || baud > SwoCapture::maxBaud)
		return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::MALFORMED, 0);

	/* The ring is empty now, so no SWO_DATA of the previous capture follows the answer */
	const uint32_t actual = swo->start(baud);
	reportedSwoDropped = 0;
	reportedSwoErrors = 0;
	NetworkProbe::putU32(localData(), actual);
	return queueLocalResponse(header.seq, header.type, NetworkProbe::Status::OK, NetworkProbe::swoStartResponseSize);
}

bool NetServer::queueLocalResponse(const uint16_t seq, const NetworkProbe::Type request, const NetworkProbe::Status status, const uint16_t dataLength)
{
	if (localLength)
	{
		ESP_LOGW(tag, "the client sent a request before the previous answer, closing the connection");
		return false;
	}
	uint8_t* const frame = localFrame.data();
	frame[NetworkProbe::headerSize] = NetworkProbe::toByte(request);
	frame[NetworkProbe::headerSize + 1] = NetworkProbe::toByte(status);
	localLength = sealFrame(frame, NetworkProbe::Type::RESP, NetworkProbe::respHeaderSize + dataLength, seq);
	return true;
}

uint16_t NetServer::buildHello(uint8_t* const data) const
{
	NetworkProbe::putU16(data, NetworkProbe::protocolVersion);
	NetworkProbe::putU16(data + 2, HostLink::maxPayload);
	NetworkProbe::putU32(data + 4, NetworkProbe::capSwd | NetworkProbe::capReset | NetworkProbe::capStream | (swo->isAvailable() ? NetworkProbe::capSwo : 0U));
	NetworkProbe::putU16(data + 8, HostLink::maxRead);
	data[10] = HostLink::maxList;
	data[11] = HostLink::maxBlocks;
	NetworkProbe::putU16(data + 12, HostLink::maxSampleBytes);
	NetworkProbe::putU16(data + 14, 0);
	NetworkProbe::putU32(data + 16, SwdTap::minFrequency);
	NetworkProbe::putU32(data + 20, SwdTap::getMaxFrequency());

	uint8_t* out = data + NetworkProbe::helloResponseFixedSize;
	out = putString(out, probeName);
	out = putString(out, esp_app_get_description()->version);
	out = putString(out, probeId);
	/* Fields of the capabilities after the strings, in the order of their bits */
	if (swo->isAvailable())
	{
		NetworkProbe::putU32(out, SwoCapture::minBaud);
		NetworkProbe::putU32(out + 4, SwoCapture::maxBaud);
		out += NetworkProbe::helloSwoSize;
	}
	return static_cast<uint16_t>(out - data);
}

void NetServer::serviceLink()
{
	const uint32_t ringRequest = link->ringRequest.load(std::memory_order_acquire);
	if (ringRequest != ackedRingRequest)
	{
		link->ring.discardAll();
		ackedRingRequest = ringRequest;
		reportedDropped = 0;
		reportedErrors = 0;
		reportedLost = link->targetLostCount.load(std::memory_order_acquire);
		link->ringAck.store(ringRequest, std::memory_order_release);
	}

	/* An answer for a client that is gone */
	if ((clientSocket < 0 || dropResponse) && link->slotState.load(std::memory_order_acquire) == HostLink::SlotState::response)
	{
		requestOutstanding = false;
		dropResponse = false;
		link->slotState.store(HostLink::SlotState::empty, std::memory_order_release);
	}
}

bool NetServer::pumpTx()
{
	const int64_t now = esp_timer_get_time();
	while (true)
	{
		if (txSent == txLength && !nextFrame(now))
			return true;

		while (txSent < txLength)
		{
			const ssize_t sent = send(clientSocket, txFrame.data() + txSent, txLength - txSent, 0);
			if (sent < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return true;
				if (errno == EINTR)
					continue;
				return false;
			}
			txSent += static_cast<size_t>(sent);
		}
	}
}

bool NetServer::nextFrame(const int64_t now)
{
	txLength = 0;
	txSent = 0;

	if (localLength)
	{
		memcpy(txFrame.data(), localFrame.data(), localLength);
		txLength = localLength;
		localLength = 0;
		return true;
	}

	if (requestOutstanding && !dropResponse && link->slotState.load(std::memory_order_acquire) == HostLink::SlotState::response)
	{
		uint8_t* const frame = txFrame.data();
		frame[NetworkProbe::headerSize] = NetworkProbe::toByte(link->requestType);
		frame[NetworkProbe::headerSize + 1] = NetworkProbe::toByte(link->responseStatus);
		memcpy(frame + NetworkProbe::headerSize + NetworkProbe::respHeaderSize, link->responseData, link->responseLength);
		txLength = sealFrame(frame, NetworkProbe::Type::RESP, NetworkProbe::respHeaderSize + link->responseLength, link->requestSeq);
		requestOutstanding = false;
		link->slotState.store(HostLink::SlotState::empty, std::memory_order_release);
		return true;
	}

	if (buildEvent(now) || buildSwoEvent(now))
		return true;

	/* DATA and SWO_DATA take turns, so neither waits behind a backlog of the other */
	swoTurn = !swoTurn;
	return swoTurn ? (buildSwoData(now) || buildData(now)) : (buildData(now) || buildSwoData(now));
}

bool NetServer::buildEvent(const int64_t now)
{
	if (link->streamGeneration.load(std::memory_order_acquire) != link->sessionGeneration.load(std::memory_order_acquire))
		return false;

	const uint32_t lost = link->targetLostCount.load(std::memory_order_acquire);
	if (lost != reportedLost)
	{
		reportedLost = lost;
		buildEventFrame(NetworkProbe::Event::TARGET_LOST, 0);
		return true;
	}
	const uint32_t dropped = link->droppedSamples.load(std::memory_order_relaxed);
	if (dropped != reportedDropped && now - lastOverflowEvent >= eventIntervalUs)
	{
		reportedDropped = dropped;
		lastOverflowEvent = now;
		buildEventFrame(NetworkProbe::Event::BUFFER_OVERFLOW, dropped);
		return true;
	}
	const uint32_t errors = link->sampleErrors.load(std::memory_order_relaxed);
	if (errors != reportedErrors && now - lastErrorEvent >= eventIntervalUs)
	{
		reportedErrors = errors;
		lastErrorEvent = now;
		buildEventFrame(NetworkProbe::Event::SAMPLE_ERRORS, errors);
		return true;
	}
	return false;
}

bool NetServer::buildSwoEvent(const int64_t now)
{
	if (!swo->isCapturing())
		return false;

	const uint32_t dropped = swo->getDroppedBytes();
	if (dropped != reportedSwoDropped && now - lastSwoOverflowEvent >= eventIntervalUs)
	{
		reportedSwoDropped = dropped;
		lastSwoOverflowEvent = now;
		buildEventFrame(NetworkProbe::Event::SWO_OVERFLOW, dropped);
		return true;
	}
	const uint32_t errors = swo->getErrors();
	if (errors != reportedSwoErrors && now - lastSwoErrorEvent >= eventIntervalUs)
	{
		reportedSwoErrors = errors;
		lastSwoErrorEvent = now;
		buildEventFrame(NetworkProbe::Event::SWO_ERRORS, errors);
		return true;
	}
	return false;
}

void NetServer::buildEventFrame(const NetworkProbe::Event event, const uint32_t value)
{
	uint8_t* const payload = txFrame.data() + NetworkProbe::headerSize;
	payload[0] = NetworkProbe::toByte(event);
	NetworkProbe::putU32(payload + 1, value);
	txLength = sealFrame(txFrame.data(), NetworkProbe::Type::EVENT, NetworkProbe::eventSize, frameSeq++);
}

bool NetServer::buildData(const int64_t now)
{
	SampleRing& ring = link->ring;
	const uint32_t available = ring.available();
	if (!available)
		return false;
	/* Samples of a session that ended are never sent, also not to the client that took over */
	if (link->streamGeneration.load(std::memory_order_acquire) != link->sessionGeneration.load(std::memory_order_acquire))
	{
		ring.discardAll();
		return false;
	}

	/* A slot is u32 sample index, then the record as DATA carries it */
	const size_t record = ring.getSlotSize() - 4U;
	const uint32_t frameSamples = std::max<uint32_t>(1, static_cast<uint32_t>((dataTargetPayload - NetworkProbe::dataHeaderSize) / record));
	if (available < frameSamples && link->streaming.load(std::memory_order_acquire) && now - lastDataTime < dataFlushUs)
		return false;

	/* One batch holds consecutive samples only */
	const uint32_t first = NetworkProbe::getU32(ring.peek(0));
	uint32_t count = 1;
	while (count < std::min(available, frameSamples) && NetworkProbe::getU32(ring.peek(count)) == first + count)
		count++;

	uint8_t* const payload = txFrame.data() + NetworkProbe::headerSize;
	NetworkProbe::putU16(payload, link->streamPlanId.load(std::memory_order_relaxed));
	NetworkProbe::putU32(payload + 2, first);
	NetworkProbe::putU16(payload + 6, static_cast<uint16_t>(count));
	NetworkProbe::putU16(payload + 8, static_cast<uint16_t>(record));
	uint8_t* out = payload + NetworkProbe::dataHeaderSize;
	for (uint32_t i = 0; i < count; ++i)
	{
		memcpy(out, ring.peek(i) + 4U, record);
		out += record;
	}
	ring.release(count);

	lastDataTime = now;
	txLength = sealFrame(txFrame.data(), NetworkProbe::Type::DATA, static_cast<uint16_t>(out - payload), frameSeq++);
	return true;
}

bool NetServer::buildSwoData(const int64_t now)
{
	const size_t buffered = swo->getBuffered();
	if (!buffered || (buffered < dataTargetPayload && now - lastSwoDataTime < dataFlushUs))
		return false;

	/* One frame holds consecutive bytes only */
	uint8_t* const payload = txFrame.data() + NetworkProbe::headerSize;
	uint32_t first = 0;
	const size_t length = swo->read(payload + NetworkProbe::swoDataHeaderSize, dataTargetPayload - NetworkProbe::swoDataHeaderSize, first);
	if (!length)
		return false;
	NetworkProbe::putU32(payload, first);

	lastSwoDataTime = now;
	txLength = sealFrame(txFrame.data(), NetworkProbe::Type::SWO_DATA, static_cast<uint16_t>(NetworkProbe::swoDataHeaderSize + length), frameSeq++);
	return true;
}

size_t NetServer::sealFrame(uint8_t* const frame, const NetworkProbe::Type type, const uint16_t length, const uint16_t seq)
{
	NetworkProbe::encodeHeader(frame, type, length, seq);
	const size_t covered = NetworkProbe::headerSize + length;
	NetworkProbe::putU32(frame + covered, esp_rom_crc32_le(0, frame, covered));
	return covered + NetworkProbe::crcSize;
}
