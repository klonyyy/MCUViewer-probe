/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SAMPLERING_HPP
#define SAMPLERING_HPP

#include <stddef.h>
#include <stdint.h>

#include <atomic>

/**
 * @brief Lock-free ring of fixed-size sample slots, from the SWD owner task (the only producer) to the net task (the
 * only consumer).
 *
 * head and tail count the slots ever written and read, so head - tail is the fill level also across the wrap of the
 * counters; the number of slots is a power of two for the same reason. The slot size changes only through
 * configure(), while the consumer has nothing to read and does not look, see HostLink's quiesce handshake.
 */
class SampleRing
{
   public:
	/**
	 * @brief Takes the storage. Call once, before both tasks run.
	 */
	void init(uint8_t* storageBuffer, size_t storageCapacity)
	{
		storage = storageBuffer;
		capacity = storageCapacity;
	}

	/**
	 * @brief Producer: sets the slot size, using as many slots as fit, rounded down to a power of two.
	 * @return The number of slots, 0 if not even one fits.
	 */
	uint32_t configure(size_t newSlotSize)
	{
		uint32_t count = 1;
		while (static_cast<size_t>(count) * 2U * newSlotSize <= capacity && count < (1U << 30))
			count *= 2U;
		slotSize = newSlotSize;
		slots = (newSlotSize <= capacity) ? count : 0;
		return slots;
	}

	/**
	 * @brief Returns the size of each slot.
	 */
	size_t getSlotSize() const
	{
		return slotSize;
	}

	/**
	 * @brief Returns the total storage capacity.
	 */
	size_t getCapacity() const
	{
		return capacity;
	}

	/**
	 * @brief Producer: the next free slot, or nullptr when the ring is full.
	 */
	uint8_t* beginWrite()
	{
		const uint32_t position = head.load(std::memory_order_relaxed);
		if (!slots || position - tail.load(std::memory_order_acquire) >= slots)
			return nullptr;
		return storage + static_cast<size_t>(position & (slots - 1U)) * slotSize;
	}

	/**
	 * @brief Producer: publishes the slot beginWrite() returned.
	 */
	void commitWrite()
	{
		head.store(head.load(std::memory_order_relaxed) + 1U, std::memory_order_release);
	}

	/**
	 * @brief Consumer: the number of slots ready to read.
	 */
	uint32_t available() const
	{
		return head.load(std::memory_order_acquire) - tail.load(std::memory_order_relaxed);
	}

	/**
	 * @brief Consumer: slot index of the ones ready to read, index < available().
	 * @param index The index of the slot to peek at.
	 */
	const uint8_t* peek(uint32_t index) const
	{
		return storage + static_cast<size_t>((tail.load(std::memory_order_relaxed) + index) & (slots - 1U)) * slotSize;
	}

	/**
	 * @brief Consumer: frees the oldest count slots.
	 * @param count The number of slots to release.
	 */
	void release(uint32_t count)
	{
		tail.store(tail.load(std::memory_order_relaxed) + count, std::memory_order_release);
	}

	/**
	 * @brief Consumer: frees every slot ready to read.
	 */
	void discardAll()
	{
		tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
	}

   private:
	uint8_t* storage = nullptr;
	size_t capacity = 0;
	size_t slotSize = 0;
	uint32_t slots = 0;
	std::atomic<uint32_t> head{0};
	std::atomic<uint32_t> tail{0};
};

#endif /* SAMPLERING_HPP */
