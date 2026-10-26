/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SWORING_HPP
#define SWORING_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <atomic>

/**
 * @brief Lock-free ring of SWO chunks, from the DMA callback (the only producer) to the net task (the only consumer).
 *
 * A chunk is what one DMA callback delivers. It is stored as a record: u32 offset of its first byte in the SWO stream,
 * u32 length, then the bytes. A chunk that does not fit is dropped whole, so the offsets show where bytes are missing.
 *
 * head and tail count the bytes ever written and read, records included, so head - tail is the fill level also across
 * the wrap of the counters; the capacity is a power of two for the same reason.
 */
class SwoRing
{
   public:
	/**
	 * @brief Takes the storage. Call once, before the producer runs.
	 * @param storageCapacity A power of two.
	 */
	void init(uint8_t* storageBuffer, size_t storageCapacity)
	{
		storage = storageBuffer;
		capacity = static_cast<uint32_t>(storageCapacity);
	}

	/**
	 * @brief Producer: stores a chunk.
	 * @param offset Stream offset of the first byte.
	 * @return False when it does not fit; nothing is stored then.
	 */
	bool write(uint32_t offset, const uint8_t* data, uint32_t length)
	{
		const uint32_t position = head.load(std::memory_order_relaxed);
		const uint32_t used = position - tail.load(std::memory_order_acquire);
		if (length == 0 || length > capacity || recordHeaderSize + length > capacity - used)
			return false;

		uint8_t header[recordHeaderSize];
		putU32(header, offset);
		putU32(header + 4, length);
		copyIn(position, header, recordHeaderSize);
		copyIn(position + recordHeaderSize, data, length);
		head.store(position + recordHeaderSize + length, std::memory_order_release);
		return true;
	}

	/**
	 * @brief Consumer: bytes in the ring, record headers included.
	 */
	uint32_t used() const
	{
		return head.load(std::memory_order_acquire) - tail.load(std::memory_order_relaxed);
	}

	/**
	 * @brief Consumer: copies the oldest bytes not read yet, up to maxLength, and stops where bytes are missing in the
	 * stream, so the bytes it returns are consecutive.
	 * @param first Set to the stream offset of the first byte copied.
	 * @return The number of bytes copied, 0 when the ring is empty.
	 */
	size_t read(uint8_t* out, size_t maxLength, uint32_t& first)
	{
		size_t copied = 0;
		while (copied < maxLength && loadRecord())
		{
			const uint32_t offset = recordOffset + recordTaken;
			if (copied == 0)
				first = offset;
			else if (offset != first + copied)
				break;

			const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(maxLength - copied), recordLength - recordTaken);
			const uint32_t position = tail.load(std::memory_order_relaxed);
			copyOut(position + recordHeaderSize + recordTaken, out + copied, count);
			copied += count;
			recordTaken += count;
			if (recordTaken == recordLength)
			{
				tail.store(position + recordHeaderSize + recordLength, std::memory_order_release);
				recordLoaded = false;
			}
		}
		return copied;
	}

	/**
	 * @brief Consumer: drops everything. Call only while the producer does not write.
	 */
	void discardAll()
	{
		tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
		recordLoaded = false;
	}

   private:
	static constexpr uint32_t recordHeaderSize = 8; /**< u32 offset, u32 length. */

	static void putU32(uint8_t* out, uint32_t value)
	{
		memcpy(out, &value, sizeof(value));
	}

	static uint32_t getU32(const uint8_t* in)
	{
		uint32_t value;
		memcpy(&value, in, sizeof(value));
		return value;
	}

	/* Copies at a ring position, across the end of the storage */
	void copyIn(uint32_t position, const uint8_t* data, uint32_t length)
	{
		const uint32_t index = position & (capacity - 1U);
		const uint32_t first = std::min(length, capacity - index);
		memcpy(storage + index, data, first);
		memcpy(storage, data + first, length - first);
	}

	void copyOut(uint32_t position, uint8_t* data, uint32_t length) const
	{
		const uint32_t index = position & (capacity - 1U);
		const uint32_t first = std::min(length, capacity - index);
		memcpy(data, storage + index, first);
		memcpy(data + first, storage, length - first);
	}

	/* Consumer: makes the record at tail the current one */
	bool loadRecord()
	{
		if (recordLoaded)
			return true;
		const uint32_t position = tail.load(std::memory_order_relaxed);
		if (head.load(std::memory_order_acquire) == position)
			return false;
		uint8_t header[recordHeaderSize];
		copyOut(position, header, recordHeaderSize);
		recordOffset = getU32(header);
		recordLength = getU32(header + 4);
		recordTaken = 0;
		recordLoaded = true;
		return true;
	}

	uint8_t* storage = nullptr;
	uint32_t capacity = 0;
	std::atomic<uint32_t> head{0};
	std::atomic<uint32_t> tail{0};

	/* Consumer: the record at tail, of which recordTaken bytes were read */
	bool recordLoaded = false;
	uint32_t recordOffset = 0;
	uint32_t recordLength = 0;
	uint32_t recordTaken = 0;
};

#endif /* SWORING_HPP */
