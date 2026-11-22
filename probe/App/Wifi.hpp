/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WIFI_HPP
#define WIFI_HPP

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <mutex>

#include "NetworkSettings.hpp"
#include "SetupPortal.hpp"
#include "esp_bit_defs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

/**
 * @brief Wi-Fi: the station on the saved networks, setup mode, and the mDNS announcement.
 *
 * At boot the probe joins one of the saved networks (SavedNetworks): the strongest one in range, then the others by
 * signal strength should it fail. If none is saved, or none gives an IP address in time, the probe enters setup mode:
 * an access point named after the device id serves the setup page (SetupPortal), and the station keeps trying the
 * saved networks in the background. A network chosen on the setup page is tried while the access point stays up, added
 * to the saved ones once it gives an IP address, and the access point closes a minute later. On its network the probe
 * reconnects whenever the link drops; when no saved network answers, it rests before the next round, roundPauseMinMs
 * at first and twice as long each time up to roundPauseMaxMs, with the radio on the channel of the network joined
 * last, so that a dongle finds the probe on one channel most of the time.
 *
 * An ESP-NOW session holds the radio (holdRadio()): while it lasts, the station neither scans nor connects by itself,
 * as either takes the radio off the session's channel, and an attempt under way stops. What was due waits until
 * releaseDelayMs after the session. Joins and scans from the setup page still go ahead: someone asked for them.
 *
 * Field mode (RadioSettings), chosen on the setup page, takes effect at the next start: the station starts but joins
 * nothing and scans for nothing, and its radio stays on the field channel for ESP-NOW alone; there is no access point
 * either.
 *
 * The setup (BOOT) button acts when it is let go, by how long it was held: a short press accepts a pairing request
 * (Pairing); 3 to 10 s leaves field mode and keeps the saved networks, or out of field mode forgets them; from 10 s on
 * it arms the erase of every pairing, which a short press within eraseConfirmMs confirms. Presses right after a release
 * are ignored, so a bouncing contact does not confirm anything.
 *
 * The Wi-Fi task owns all of it: the Wi-Fi events and the setup page only queue commands for it, the setup button and
 * the timeouts are polled by it.
 */
class Wifi
{
   public:
	/**
	 * @brief The last join from the setup page, or the saved network once it gave an address in setup mode.
	 */
	enum class JoinState : uint8_t
	{
		idle,
		connecting,
		connected,
		failed,
	};

	/**
	 * @brief What the setup page shows.
	 */
	struct Status
	{
		bool setupMode = false;
		JoinState state = JoinState::idle;
		char ssid[NetworkSettings::maxSsidLength + 1] = {};			/**< Network of the join. */
		char hostname[NetworkSettings::maxHostnameLength + 1] = {}; /**< Hostname of the join, else the saved one. */
		char savedSsid[NetworkSettings::maxSsidLength + 1] = {}; /**< The saved network joined last. */
		uint8_t savedCount = 0;
		char ip[16] = {};	 /**< Station address, empty without one. */
		char error[96] = {}; /**< Why the join failed. */
	};

	struct Network
	{
		char ssid[NetworkSettings::maxSsidLength + 1];
		int8_t rssi;
		bool secured;
	};

	static constexpr size_t maxNetworks = 20;

	/**
	 * @brief Brings up the network: joins a saved network, or enters setup mode if none is saved or none gave an IP
	 * address in time. Also starts mDNS. Blocks until the network is usable.
	 */
	void start();

	/**
	 * @brief Tries a network chosen on the setup page, in the background; getStatus() tells how it goes.
	 * @param settings Valid settings, see NetworkSettings::validate().
	 */
	void join(const NetworkSettings& settings);

	Status getStatus();

	/**
	 * @brief Starts a scan for networks unless one is under way. Without force, only if there was none yet.
	 */
	void requestScan(bool force);

	/**
	 * @brief Copies the networks of the last scan, strongest first, one entry per SSID.
	 * @param scanning Set while a scan is under way.
	 * @return Number of networks copied.
	 */
	size_t getNetworks(Network* networks, size_t maxCount, bool& scanning);

	/**
	 * @brief The net task: an ESP-NOW session starts on channel, the dongle's, or ends, with channel 0. A scan of the
	 * station's own stops at once, so the hold does not wait for it.
	 */
	void holdRadio(uint8_t channel);

	/**
	 * @brief Whether the station has the radio off the channel it rests on: it scans, or connects on its own. Callable
	 * from any task. The net task does not answer ESP-NOW discovery then: the answer would show the dongle a channel the
	 * probe does not stay on.
	 */
	bool isRoaming() const { return roaming.load(std::memory_order_relaxed); }

	/**
	 * @brief Whether dongles may reach the probe, see RadioSettings. Callable from any task.
	 */
	bool isEspNowEnabled() const { return espNowEnabled.load(std::memory_order_relaxed); }

	RadioSettings getRadio();

	/**
	 * @brief The setup page: saves the radio settings. ESP-NOW is turned on or off at once; field mode needs a restart,
	 * which follows a moment later, so the page gets its answer first.
	 * @param restarting Set when the probe restarts into field mode.
	 * @return Why the settings were not saved, or nullptr.
	 */
	const char* setRadio(const RadioSettings& settings, bool& restarting);

	const char* getDeviceId() const { return deviceId; }

	/**
	 * @brief Copies the hostname the probe uses now on DHCP and mDNS, without ".local". Callable from any task.
	 * @param hostname Receives the hostname.
	 * @param size Size of hostname, with the terminator.
	 */
	void getHostname(char* hostname, size_t size);

   private:
	static constexpr EventBits_t readyBit = BIT0; /**< The station has an address, or the setup access point is up. */
	static constexpr uint8_t apMaxConnections = 2;
	static constexpr uint32_t taskStackSize = 4096;
	static constexpr uint32_t taskPriority = 5;
	static constexpr int taskCore = 0; /**< With Wi-Fi and lwIP; core 1 belongs to SWD. */
	static constexpr size_t queueLength = 16;
	static constexpr uint32_t postTimeoutMs = 100;
	static constexpr uint32_t reconnectDelayMs = 1000; /**< After esp_wifi_connect() itself failed. */
	static constexpr uint32_t roundPauseMinMs = 2000;  /**< Between rounds over the saved networks, while none answers. */
	static constexpr uint32_t roundPauseMaxMs = 30000;
	static constexpr uint32_t releaseDelayMs = 5000; /**< After an ESP-NOW session: a host that comes back finds the probe where it was. */
	static constexpr uint32_t setupRetryMs = 30000;	 /**< How often setup mode tries the saved network. */
	static constexpr uint32_t setupCloseMs = 60000;	   /**< The access point stays this long after the station got an address. */
	static constexpr uint32_t joinTimeoutMs = 45000;
	static constexpr uint8_t maxJoinAttempts = 3; /**< A wrong password ends the join at the first one. */
	static constexpr uint32_t restartDelayMs = 1000; /**< After the setup page turned field mode on: its answer goes first. */
	static constexpr uint32_t buttonPollMs = 50;
	static constexpr uint32_t shortPressMaxMs = 1000;  /**< A release before it: a short press. */
	static constexpr uint32_t buttonHoldMs = 3000;	   /**< Released from here on: the networks, or field mode. */
	static constexpr uint32_t eraseHoldMs = 10000;	   /**< Released from here on: the pairing erase is armed. */
	static constexpr uint32_t eraseConfirmMs = 5000;   /**< For the short press that confirms the erase. */
	static constexpr uint32_t buttonLockoutMs = 300;   /**< After a release: no new press, against contact bounce. */
	static constexpr uint16_t maxScanRecords = 32;

	enum class Command : uint8_t
	{
		staStart,
		staConnected,	 /**< value: the channel. */
		staDisconnected, /**< value: the reason. */
		gotIp,			 /**< value: the address. */
		apStart,
		join, /**< The settings are in requested. */
		scan,
		radioHold, /**< value: the channel of the ESP-NOW session, 0 when it ended. */
		restart,   /**< Into field mode, after restartDelayMs. */
	};

	struct Message
	{
		Command command;
		uint32_t value;
	};

	/**
	 * @brief A point in time the Wi-Fi task acts at; zero while disarmed.
	 */
	struct Deadline
	{
		int64_t at = 0;

		void arm(uint32_t ms);
		void disarm() { at = 0; }

		/**
		 * @return True once the deadline passed; it is disarmed then.
		 */
		bool due(int64_t now);
	};

	static void eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
	void handleEvent(esp_event_base_t base, int32_t id, void* data);
	void post(Command command, uint32_t value = 0);

	static void taskEntry(void* arg);
	[[noreturn]] void run();
	void handle(const Message& message);
	TickType_t ticksUntilDeadline() const;

	void boot();

	/**
	 * @brief Starts field mode: the station without a network, its radio on the field channel.
	 */
	void startField();
	void enterSetup();
	void leaveSetup();

	/**
	 * @brief Starts connecting the station to a network; a connection must not be under way.
	 */
	void connectStation(const NetworkSettings& settings);

	/**
	 * @brief Starts connecting the station to the saved networks, in the order of their signal; a connection must not
	 * be under way. A failed attempt goes on with the next one, see connectNextSaved().
	 */
	void connectSaved();

	/**
	 * @return False if every saved network of the current round was tried.
	 */
	bool connectNextSaved();

	/**
	 * @brief Starts a round over the saved networks, or while an ESP-NOW session holds the radio, once it lets go.
	 */
	void retrySaved();
	void deferToRelease();

	/**
	 * @brief Puts the idle station's radio on the channel a dongle finds it on: the field channel in field mode, else the
	 * session's, else the last network's.
	 */
	void parkRadio();
	void onRadioHold(uint8_t channel);
	void applyHostname(const char* hostname);
	void onDisconnected(uint32_t reason);
	void onGotIp(uint32_t address);
	void onJoin();
	void onJoinTimeout();

	/**
	 * @brief Ends a join that failed, and goes back to the saved network.
	 */
	void finishJoin(const char* error);
	void scan();
	void startMdns();
	void startButton();
	void pollButton(int64_t now);
	void onButtonReleased(int64_t heldMs, int64_t now);
	void leaveFieldOrForget();

	SetupPortal portal{*this};
	EventGroupHandle_t events = nullptr;
	QueueHandle_t queue = nullptr;
	esp_netif_t* staNetif = nullptr;
	esp_netif_t* apNetif = nullptr;
	char deviceId[NetworkSettings::deviceIdSize] = {};

	/* Wi-Fi task */
	SavedNetworks saved;	   /**< Joined at boot and retried in setup mode. */
	NetworkSettings candidate; /**< What the join tries. */
	NetworkSettings active;	   /**< What the station connects to, or is connected to. */
	uint8_t order[SavedNetworks::maxCount] = {}; /**< Indexes into saved, in the order the current round tries them. */
	size_t orderCount = 0;
	size_t orderNext = 0;
	bool wifiStarted = false;
	bool fieldMode = false;	  /**< Since the start: changes take a restart. */
	uint8_t fieldChannel = 0;
	bool setupMode = false;
	bool staBusy = false;	/**< Connecting or connected: another connection needs a disconnection first. */
	bool staLinked = false; /**< Associated, maybe without an address yet. */
	bool hasIp = false;
	bool joining = false;
	bool joinAfterDisconnect = false; /**< The join waits for the station to let go of the previous connection. */
	uint8_t joinAttempts = 0;
	bool scanPending = false;
	uint32_t roundPauseMs = roundPauseMinMs;
	uint8_t homeChannel = 0; /**< Of the network joined last, 0 before the first. */
	bool radioHeld = false;
	uint8_t holdChannel = 0;
	bool retryOnRelease = false;
	std::atomic<bool> roaming{false};
	std::atomic<bool> ownScan{false}; /**< A scan for connectSaved(), which a hold stops. */
	std::atomic<bool> espNowEnabled{true};
	int64_t buttonPressedAt = 0;
	int64_t buttonReleasedAt = 0;
	int64_t eraseArmedUntil = 0; /**< The pairing erase waits for a short press until then; 0 when not armed. */
	Deadline bootDeadline;
	Deadline retryDeadline;
	Deadline joinDeadline;
	Deadline closeDeadline;
	Deadline buttonDeadline;
	Deadline restartDeadline;

	/* Shared with the setup page, and activeHostname with the net task */
	std::mutex statusLock;
	Status status;
	RadioSettings radio; /**< As saved: field mode as it will be at the next start. */
	char activeHostname[NetworkSettings::maxHostnameLength + 1] = {}; /**< Written only by applyHostname(). */
	NetworkSettings requested; /**< The join from the setup page, for the Wi-Fi task. */
	Network networks[maxNetworks] = {};
	size_t networkCount = 0;
	bool scanRequested = false;
	bool scanned = false;
};

#endif /* WIFI_HPP */
