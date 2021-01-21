/*
 * Copyright 2003-2021 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "PeakBuffer.hxx"
#include "DynamicFifoBuffer.hxx"

#include <algorithm>
#include <cassert>

#include <string.h>

PeakBuffer::~PeakBuffer()
{
	delete normal_buffer;
	delete peak_buffer;
}

bool
PeakBuffer::empty() const noexcept
{
	return (normal_buffer == nullptr || normal_buffer->empty()) &&
		(peak_buffer == nullptr || peak_buffer->empty());
}

WritableBuffer<void>
PeakBuffer::Read() const noexcept
{
	if (normal_buffer != nullptr) {
		const auto p = normal_buffer->Read();
		if (!p.empty())
			return p.ToVoid();
	}

	if (peak_buffer != nullptr) {
		const auto p = peak_buffer->Read();
		if (!p.empty())
			return p.ToVoid();
	}

	return nullptr;
}

void
PeakBuffer::Consume(size_t length) noexcept
{
	if (normal_buffer != nullptr && !normal_buffer->empty()) {
		normal_buffer->Consume(length);
		return;
	}

	if (peak_buffer != nullptr && !peak_buffer->empty()) {
		peak_buffer->Consume(length);
		if (peak_buffer->empty()) {
			delete peak_buffer;
			peak_buffer = nullptr;
		}

		return;
	}
}

static size_t
AppendTo(DynamicFifoBuffer<uint8_t> &buffer,
	 const void *data, size_t length) noexcept
{
	assert(data != nullptr);
	assert(length > 0);

	size_t total = 0;

	do {
		const auto p = buffer.Write();
		if (p.empty())
			break;

		const size_t nbytes = std::min(length, p.size);
		memcpy(p.data, data, nbytes);
		buffer.Append(nbytes);

		data = static_cast<const uint8_t *>(data) + nbytes;
		length -= nbytes;
		total += nbytes;
	} while (length > 0);

	return total;
}

bool
PeakBuffer::Append(const void *data, size_t length)
{
	if (length == 0)
		return true;

	if (peak_buffer != nullptr && !peak_buffer->empty()) {
		size_t nbytes = AppendTo(*peak_buffer, data, length);
		return nbytes == length;
	}

	if (normal_buffer == nullptr)
		normal_buffer = new DynamicFifoBuffer<uint8_t>(normal_size);

	size_t nbytes = AppendTo(*normal_buffer, data, length);
	if (nbytes > 0) {
		data = static_cast<const uint8_t *>(data) + nbytes;
		length -= nbytes;
		if (length == 0)
			return true;
	}

	if (peak_buffer == nullptr) {
		if (peak_size > 0)
			peak_buffer = new DynamicFifoBuffer<uint8_t>(peak_size);
		if (peak_buffer == nullptr)
			return false;
	}

	nbytes = AppendTo(*peak_buffer, data, length);
	return nbytes == length;
}
