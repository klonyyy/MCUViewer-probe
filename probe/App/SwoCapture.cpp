/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SwoCapture.hpp"

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hal/uart_ll.h"
#include "sdkconfig.h"

namespace
{
constexpr const char* tag = "swo";

constexpr uart_port_t swoUart = UART_NUM_1;

/* The largest buffer one DMA descriptor takes (DMA_DESCRIPTOR_BUFFER_MAX_SIZE), aligned down to 64 bytes, the largest
   alignment GDMA asks for. The driver makes max_receive_internal_mem / this many descriptors, and splits every receive
   buffer evenly over them. */
constexpr size_t dmaDescriptorLimit = 4032;
constexpr size_t dmaAlignment = 64;

/* Idle time after which UHCI ends a transfer, in bit times: the largest the UART takes. It bounds the transfers, and
   so the interrupts, to one per 1023 bit times, and delays the bytes before a pause by as much: 0.2 ms at 5 MBaud */
constexpr uint32_t rxIdleBits = UART_RX_IDLE_THRHD_V;

constexpr uint32_t errorMask = UART_INTR_FRAM_ERR | UART_INTR_PARITY_ERR | UART_INTR_RXFIFO_OVF | UART_INTR_BRK_DET;

/* The largest power of two at or below value */
constexpr size_t floorPowerOfTwo(const size_t value)
{
	size_t result = 1;
	while (result * 2U <= value)
		result *= 2U;
	return result;
}
}  // namespace

bool SwoCapture::init()
{
#if CONFIG_NETWORK_PROBE_SWO_GPIO < 0
	ESP_LOGI(tag, "SWO not configured");
	return false;
#else
	size_t ringBytes = floorPowerOfTwo(static_cast<size_t>(CONFIG_NETWORK_PROBE_SWO_BUFFER_KB) * 1024U);
	uint8_t* storage = static_cast<uint8_t*>(heap_caps_malloc(ringBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
	if (!storage)
	{
		ESP_LOGW(tag, "no PSRAM for a %u KiB SWO buffer, using %u KiB of internal RAM", ringBytes / 1024U, fallbackRingBytes / 1024U);
		ringBytes = fallbackRingBytes;
		storage = static_cast<uint8_t*>(heap_caps_malloc(ringBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
	}
	dmaBuffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(dmaAlignment, dmaBufferSize, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
	if (!storage || !dmaBuffer)
	{
		ESP_LOGE(tag, "no memory for the SWO buffers, SWO disabled");
		heap_caps_free(storage);
		heap_caps_free(dmaBuffer);
		dmaBuffer = nullptr;
		return false;
	}
	ring.init(storage, ringBytes);

	/* SWO in NRZ encoding is plain UART: 8 data bits, no parity, 1 stop bit, idle high */
	uart_config_t uartConfig = {};
	uartConfig.baud_rate = static_cast<int>(maxBaud);
	uartConfig.data_bits = UART_DATA_8_BITS;
	uartConfig.parity = UART_PARITY_DISABLE;
	uartConfig.stop_bits = UART_STOP_BITS_1;
	uartConfig.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
	uartConfig.source_clk = UART_SCLK_DEFAULT;
	esp_err_t result = uart_param_config(swoUart, &uartConfig);
	if (result == ESP_OK)
		result = uart_set_pin(swoUart, UART_PIN_NO_CHANGE, CONFIG_NETWORK_PROBE_SWO_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	if (result != ESP_OK)
	{
		ESP_LOGE(tag, "cannot set up UART%d on GPIO%d: %s, SWO disabled", swoUart, CONFIG_NETWORK_PROBE_SWO_GPIO, esp_err_to_name(result));
		return false;
	}
	/* The line reads idle while the target does not drive it */
	gpio_pullup_en(static_cast<gpio_num_t>(CONFIG_NETWORK_PROBE_SWO_GPIO));
	/* No API sets it without the UART driver, which would take the FIFO from UHCI */
	uart_ll_set_rx_idle_thr(UART_LL_GET_HW(swoUart), rxIdleBits);

	uhci_controller_config_t uhciConfig = {};
	uhciConfig.uart_port = swoUart;
	/* The driver always sets up a TX channel; SWO never transmits */
	uhciConfig.tx_trans_queue_depth = 1;
	uhciConfig.max_transmit_size = dmaAlignment;
	uhciConfig.max_receive_internal_mem = dmaNodeCount * dmaDescriptorLimit;
	uhciConfig.dma_burst_size = 32;
	uhciConfig.rx_eof_flags.idle_eof = 1;
	/* Allocates the DMA interrupt on this core, the net task's */
	result = uhci_new_controller(&uhciConfig, &controller);
	if (result == ESP_OK)
	{
		uhci_event_callbacks_t callbacks = {};
		callbacks.on_rx_trans_event = onReceive;
		result = uhci_register_event_callbacks(controller, &callbacks, this);
	}
	if (result != ESP_OK || !receive())
	{
		ESP_LOGE(tag, "cannot set up the SWO DMA: %s, SWO disabled", esp_err_to_name(result));
		return false;
	}

	available = true;
	ESP_LOGI(tag, "SWO on GPIO%d: UART%d with DMA, %u KiB buffer, %" PRIu32 " to %" PRIu32 " baud", CONFIG_NETWORK_PROBE_SWO_GPIO, swoUart, ringBytes / 1024U, minBaud, maxBaud);
	return true;
#endif
}

uint32_t SwoCapture::start(const uint32_t baud)
{
	stop();

	uint32_t actual = 0;
	if (uart_set_baudrate(swoUart, baud) != ESP_OK || uart_get_baudrate(swoUart, &actual) != ESP_OK)
		ESP_LOGW(tag, "cannot set %" PRIu32 " baud", baud);
	uart_ll_clr_intsts_mask(UART_LL_GET_HW(swoUart), errorMask);
	streamOffset.store(0, std::memory_order_relaxed);
	droppedBytes.store(0, std::memory_order_relaxed);
	errors = 0;
	capturing.store(true, std::memory_order_release);
	ESP_LOGI(tag, "capture started at %" PRIu32 " baud (%" PRIu32 " asked)", actual, baud);
	return actual;
}

void SwoCapture::stop()
{
	if (!capturing.load(std::memory_order_acquire))
		return;
	capturing.store(false, std::memory_order_release);
	ring.discardAll();
	ESP_LOGI(tag, "capture stopped: %" PRIu32 " bytes, %" PRIu32 " dropped, %" PRIu32 " errors, %" PRIu32 " DMA chunks since boot", streamOffset.load(std::memory_order_relaxed), droppedBytes.load(std::memory_order_relaxed), errors, chunks.load(std::memory_order_relaxed));
}

void SwoCapture::poll()
{
	if (!available)
		return;
	if (receiveStopped.load(std::memory_order_acquire) && receive())
		receiveStopped.store(false, std::memory_order_release);

	/* The error interrupts stay disabled; their raw flags are set all the same */
	uart_dev_t* const uart = UART_LL_GET_HW(swoUart);
	const uint32_t raw = uart_ll_get_intraw_mask(uart) & errorMask;
	if (!raw)
		return;
	uart_ll_clr_intsts_mask(uart, raw);
	if (capturing.load(std::memory_order_acquire))
		errors++;
}

bool SwoCapture::receive()
{
	return uhci_receive(controller, dmaBuffer, dmaBufferSize) == ESP_OK;
}

bool SwoCapture::onReceive(const uhci_controller_handle_t, const uhci_rx_event_data_t* const event, void* const context)
{
	SwoCapture& self = *static_cast<SwoCapture*>(context);
	self.chunks.fetch_add(1, std::memory_order_relaxed);

	/* get length of received data */
	const uint32_t length = static_cast<uint32_t>(event->recv_size);

	/* if there is received data and capturing is active */
	if (length && self.capturing.load(std::memory_order_acquire))
	{	
		/* get the current stream offset */
		const uint32_t offset = self.streamOffset.load(std::memory_order_relaxed);
		/* write the received data to the ring buffer */
		if (!self.ring.write(offset, event->data, length))
			self.droppedBytes.fetch_add(length, std::memory_order_relaxed);
		/* update the stream offset */
		self.streamOffset.store(offset + length, std::memory_order_relaxed);
	}

	/* The transfer ended: the line went idle, or the DMA failed and there is no data. The driver has stopped the
	   channel, so start the next transfer before the UART's FIFO fills */
	if ((event->flags.totally_received || !event->data) && !self.receive())
		self.receiveStopped.store(true, std::memory_order_release);
	return false;
}
