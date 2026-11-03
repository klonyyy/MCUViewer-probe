/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SWOCAPTURE_HPP
#define SWOCAPTURE_HPP

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "SwoRing.hpp"
#include "driver/uhci.h"

/**
 * @brief Receives the target's SWO pin in NRZ (UART) encoding, for the net task to send in SWO_DATA frames. The bytes
 * are passed on unchanged; the host decodes them, and sets up the target's trace unit itself over SWD.
 *
 * UART1 receives the pin, and UHCI moves the bytes by DMA into a circular chain of descriptors in internal RAM, so a
 * late interrupt does not lose bytes as long as the chain has room. The DMA callback copies each chunk into a ring in
 * PSRAM: a full descriptor, or what came before the line went idle. An idle line ends the DMA transfer (UHCI's idle
 * EOF), so the callback starts the next one at once; until then the UART's FIFO holds what comes.
 *
 * The DMA runs from init() on, whether a capture runs or not: the driver cannot stop a transfer that has not ended.
 * Outside a capture the callback drops the chunks. A capture therefore can start with bytes received shortly before
 * it, at most one descriptor; the host joins the stream at an arbitrary point anyway.
 *
 * Everything but the DMA callback runs in the net task. The callback's interrupt is allocated on the net task's core,
 * so it never runs while the net task changes the capture state.
 */
class SwoCapture
{
   public:
	static constexpr uint32_t minBaud = 1000;
	static constexpr uint32_t maxBaud = 5000000; /**< The ESP32-S3 UART receives up to 5 MBaud. */

	/**
	 * @brief Sets up the UART, the DMA and the buffers, and starts receiving. Call once, from the net task.
	 * @return False when SWO has no GPIO configured, or a resource is missing; SWO is then not offered.
	 */
	bool init();

	bool isAvailable() const { return available; }
	bool isCapturing() const { return capturing.load(std::memory_order_acquire); }

	/**
	 * @brief Starts a capture, or starts it again: drops what is buffered, sets the baud rate, and restarts the stream
	 * offset and the counters at 0.
	 * @param baud Between minBaud and maxBaud.
	 * @return The baud rate the UART was set to, the closest it can do.
	 */
	uint32_t start(uint32_t baud);

	/**
	 * @brief Ends the capture and drops what is buffered. Harmless when not capturing.
	 */
	void stop();

	/**
	 * @brief Bytes buffered, with some overhead per chunk: enough to decide when to send.
	 */
	size_t getBuffered() const { return ring.used(); }

	/**
	 * @brief Copies the oldest buffered bytes, consecutive in the stream.
	 * @param first Set to the stream offset of the first byte: bytes since start(), including the ones lost.
	 * @return The number of bytes copied, 0 when nothing is buffered.
	 */
	size_t read(uint8_t* out, size_t maxLength, uint32_t& first) { return ring.read(out, maxLength, first); }

	/**
	 * @brief Counts the UART's receive errors since the last call, and restarts the DMA if the callback could not.
	 * Call regularly.
	 */
	void poll();

	/**
	 * @brief Bytes dropped since start() because the buffer was full.
	 */
	uint32_t getDroppedBytes() const { return droppedBytes.load(std::memory_order_relaxed); }

	/**
	 * @brief Receive errors since start(): framing errors, breaks and FIFO overruns. Several errors between two calls of
	 * poll() count as one.
	 */
	uint32_t getErrors() const { return errors; }

   private:
	static constexpr size_t dmaNodeCount = 8;
	static constexpr size_t dmaNodeSize = 2048;
	static constexpr size_t dmaBufferSize = dmaNodeCount * dmaNodeSize;
	static constexpr size_t fallbackRingBytes = 16 * 1024;

	/**
	 * @brief The DMA callback, in interrupt context: stores a chunk while capturing, and starts the next transfer when
	 * this one ended.
	 */
	static bool onReceive(uhci_controller_handle_t controller, const uhci_rx_event_data_t* event, void* context);

	/**
	 * @brief Starts a DMA transfer over the whole descriptor chain.
	 * @return False when the driver refused.
	 */
	bool receive();

	bool available = false;
	uhci_controller_handle_t controller = nullptr;
	uint8_t* dmaBuffer = nullptr;
	SwoRing ring;

	std::atomic<bool> capturing{false};

	/* Written by the DMA callback */
	std::atomic<uint32_t> streamOffset{0};	 /**< Bytes since start(), the dropped ones included. */
	std::atomic<uint32_t> droppedBytes{0};	 /**< Since start(). */
	std::atomic<uint32_t> chunks{0};		 /**< DMA callbacks since boot, also outside a capture. */
	std::atomic<bool> receiveStopped{false}; /**< A transfer ended and the next one could not be started. */

	uint32_t errors = 0; /**< Since start(). */
};

#endif /* SWOCAPTURE_HPP */
