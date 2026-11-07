/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NETSERVER_HPP
#define NETSERVER_HPP

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "EspNowLink.hpp"
#include "EspNowPort.hpp"
#include "HostLink.hpp"
#include "NetworkProbeProto.hpp"
#include "SwoCapture.hpp"

class Wifi;

/**
 * @brief The net task, on core 0: the network side of the NETWORK PROBE protocol.
 *
 * It serves one client at a time, on the protocol port or over ESP-NOW through a dongle; a new client takes over from
 * the old one. It answers HELLO itself, hands every other request to the SWD owner task through HostLink, and sends the
 * answers, a READ_BULK answer as READ_DATA frames while the SWD owner task reads on. It drains the sample ring into DATA
 * frames and reports EVENTs. Answers go out before samples, and sending never blocks the task, so a full TCP window or
 * ESP-NOW window only fills the ring.
 *
 * ESP-NOW: each packet carries one frame, so an ESP-NOW session announces a smaller max_payload and max_read in HELLO,
 * and READ_BULK chunks shrink to match. Sessions follow EspNowLink: they end with CLOSE or after a silence, resends are
 * dropped by their seq, and a frame goes only while the dongle's limit allows it, as over TCP while the window does.
 *
 * SWO needs no SWD, so the net task owns the SWO capture: it answers SWO_START and SWO_STOP itself, and sends the
 * captured bytes in SWO_DATA frames, taking turns with DATA.
 *
 * It also watches the GDB port: a GDB client is handed to the SWD owner task when no NETWORK PROBE client is connected, and
 * refused otherwise. During a GDB session every request but HELLO is answered BUSY.
 *
 * Discovery: it answers DISCOVER and IDENTIFY datagrams on the UDP discovery port, and ESP-NOW packets outside a
 * session, itself, in every session state.
 */
class NetServer
{
   public:
	/**
	 * @brief Opens the protocol port and starts the net task. Call once, from the SWD owner task, after the ring was
	 * allocated.
	 * @param hostLink State shared with the SWD owner task; probeTask must be set.
	 * @param gdbListenSocket The GDB server's listening socket, which the net task only watches.
	 * @param swoCapture Set up by the net task when it starts, so its interrupt lands on the net task's core.
	 * @param network The Wi-Fi, which tells the hostname that DISCOVER answers with.
	 */
	void start(HostLink& hostLink, int gdbListenSocket, SwoCapture& swoCapture, Wifi& network);

   private:
	static constexpr uint32_t taskStackSize = 6144;
	static constexpr uint32_t taskPriority = 5;
	static constexpr int taskCore = 0;				   /**< With Wi-Fi and lwIP; core 1 belongs to SWD. */
	static constexpr size_t dataTargetPayload = 1400;  /**< A DATA frame fills about one TCP segment. */
	static constexpr int64_t dataFlushUs = 10000;	   /**< A DATA frame goes out at least this often while sampling. */
	static constexpr int64_t eventIntervalUs = 100000; /**< OVERFLOW and SAMPLE_ERRORS at most this often. */
	static constexpr uint32_t pollMs = 5;			   /**< Wake-up period while samples may come. */
	static constexpr uint32_t sessionPollMs = 100;	   /**< Wake-up period while an ESP-NOW session may fall silent. */
	static constexpr size_t bufferSize = NetworkProbe::frameOverhead + HostLink::maxPayload;
	static constexpr size_t localFrameSize = 1024; /**< HELLO, BUSY and other answers the net task gives itself. */
	/** A DISCOVER answer, with its four strings at their longest. */
	static constexpr size_t discoveryFrameSize = NetworkProbe::frameOverhead + NetworkProbe::respHeaderSize + NetworkProbe::discoverResponseFixedSize + 4 * (1 + UINT8_MAX);

	enum class Client : uint8_t
	{
		none,
		tcp,
		espnow,
	};

	static void taskEntry(void* arg);
	[[noreturn]] void run();

	bool hasClient() const { return client != Client::none; }
	void acceptClient();

	/**
	 * @brief Takes a new client on, after the one before has gone.
	 * @param maxPayload Largest frame payload the client's transport carries.
	 */
	void startSession(Client kind, uint16_t maxPayload);

	/**
	 * @param tellPeer Over ESP-NOW: send CLOSE, unless the dongle closed the session itself.
	 */
	void closeClient(bool tellPeer = true);
	void refuseGdbClient();

	/**
	 * @brief Handles the ESP-NOW packets that came, and ends a session that fell silent.
	 */
	void serviceEspNow();
	void handlePacket(const EspNowPort::Packet& packet);
	void openEspNowSession(const uint8_t* mac, const EspNowLink::Header& linkHeader);

	/**
	 * @brief Checks that data holds exactly one frame, with a valid CRC.
	 */
	static bool checkFrame(const uint8_t* data, size_t length, NetworkProbe::FrameHeader& header);

	/**
	 * @brief Prepares the answer to a DISCOVER, or to an IDENTIFY with this probe's id, in discoveryFrame.
	 * @return Size of the answer, or 0 when there is none.
	 */
	size_t answerDiscovery(const NetworkProbe::FrameHeader& header, const uint8_t* payload);

	/**
	 * @brief Opens the UDP discovery port. Without it the probe works, but hosts have to know its address.
	 */
	void openDiscoverySocket();

	/**
	 * @brief Reads one datagram from the discovery port and answers it if it is a well-formed DISCOVER, or an IDENTIFY
	 * with this probe's id.
	 */
	void answerDatagram();

	uint16_t buildDiscover(uint8_t* data) const;

	/**
	 * @brief Receives what the client sent and handles every complete frame.
	 * @return False when the connection is gone or the stream is corrupt.
	 */
	bool receive();

	/**
	 * @return False when the client broke the protocol.
	 */
	bool handleFrame(const NetworkProbe::FrameHeader& header, const uint8_t* payload);

	/**
	 * @brief Executes SWO_START or SWO_STOP and prepares the answer.
	 * @return False when the client broke the protocol.
	 */
	bool handleSwoRequest(const NetworkProbe::FrameHeader& header, const uint8_t* payload);

	/**
	 * @brief Prepares an answer of the net task's own; its data, if any, is already at localData().
	 * @return False when one is still waiting to be sent.
	 */
	bool queueLocalResponse(uint16_t seq, NetworkProbe::Type request, NetworkProbe::Status status, uint16_t dataLength);

	uint16_t buildHello(uint8_t* data) const;
	uint8_t* localData() { return localFrame.data() + NetworkProbe::headerSize + NetworkProbe::respHeaderSize; }

	/**
	 * @brief Answers the SWD owner task's quiesce request, and drops answers nobody waits for.
	 */
	void serviceLink();

	/**
	 * @brief Sends what is pending, then the next frames, until the socket would block or nothing is left.
	 * @return False on a connection error.
	 */
	bool pumpTx();

	/**
	 * @brief Puts the next frame into txFrame: an answer, an EVENT, or a DATA batch that is due.
	 * @return False when nothing is to be sent.
	 */
	bool nextFrame(int64_t now);

	/**
	 * @brief Puts the oldest filled chunk of the READ_BULK answer into a READ_DATA frame and frees it.
	 * @return False when no chunk is waiting.
	 */
	bool buildBulkData();

	bool buildData(int64_t now);
	bool buildSwoData(int64_t now);
	bool buildEvent(int64_t now);
	bool buildSwoEvent(int64_t now);
	void buildEventFrame(NetworkProbe::Event event, uint32_t value);

	/**
	 * @brief Writes the header and the CRC around a payload that is already in place.
	 * @return Size of the whole frame.
	 */
	static size_t sealFrame(uint8_t* frame, NetworkProbe::Type type, uint16_t length, uint16_t seq);

	HostLink* link = nullptr;
	SwoCapture* swo = nullptr;
	Wifi* wifi = nullptr;
	int listenSocket = -1;
	Client client = Client::none;
	int clientSocket = -1; /**< The TCP client's. */
	int gdbListenSocket = -1;
	int discoverySocket = -1;
	char probeId[32] = {};

	/** A datagram from the discovery port: one byte more than the longest request, an IDENTIFY, so a longer one shows by its size. */
	std::array<uint8_t, NetworkProbe::frameOverhead + 1 + UINT8_MAX + 1> discoveryRequest;
	std::array<uint8_t, discoveryFrameSize> discoveryFrame; /**< An answer to DISCOVER or IDENTIFY; localFrame may hold a session answer meanwhile. */

	std::array<uint8_t, bufferSize> rxBuffer;
	size_t rxLength = 0;

	std::array<uint8_t, bufferSize> txFrame; /**< The frame being sent: txSent of txLength bytes are out. */
	size_t txLength = 0;
	size_t txSent = 0;

	std::array<uint8_t, localFrameSize> localFrame;
	size_t localLength = 0;

	/* The client session's limits, announced in HELLO */
	uint16_t sessionMaxPayload = HostLink::maxPayload;
	uint16_t sessionMaxRead = HostLink::maxRead;

	/* The ESP-NOW client, and the last ESP-NOW session that ended, whose packets are dropped */
	uint8_t espnowPeer[6] = {};
	uint16_t espnowSession = EspNowLink::noSession;
	uint16_t espnowTxSeq = 1; /**< seq of the next DATA sent. */
	uint16_t espnowRxSeq = 0; /**< seq of the last DATA taken. */
	uint16_t espnowLimit = 0; /**< The dongle's: the last seq it has room for. */
	int64_t espnowLastRx = 0;
	uint8_t endedPeer[6] = {};
	uint16_t endedSession = EspNowLink::noSession;

	bool requestOutstanding = false; /**< A request was handed over and its answer is not sent yet. */
	bool dropResponse = false;		 /**< The client of the outstanding request is gone. */
	uint16_t frameSeq = 0;			 /**< seq of DATA and EVENT. */
	int64_t lastDataTime = 0;
	uint32_t ackedRingRequest = 0;
	uint32_t reportedDropped = 0;
	uint32_t reportedErrors = 0;
	uint32_t reportedLost = 0;
	int64_t lastOverflowEvent = 0;
	int64_t lastErrorEvent = 0;

	bool swoTurn = false; /**< SWO_DATA goes before DATA when both are due. */
	int64_t lastSwoDataTime = 0;
	uint32_t reportedSwoDropped = 0;
	uint32_t reportedSwoErrors = 0;
	int64_t lastSwoOverflowEvent = 0;
	int64_t lastSwoErrorEvent = 0;
};

#endif /* NETSERVER_HPP */
