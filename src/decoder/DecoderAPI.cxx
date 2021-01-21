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

#include "DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "Log.hxx"

#include <cassert>

size_t
decoder_read(DecoderClient *client,
	     InputStream &is,
	     void *buffer, size_t length) noexcept
{
	assert(buffer != nullptr);

	/* XXX don't allow client==nullptr */
	if (client != nullptr)
		return client->Read(is, buffer, length);

	try {
		return is.LockRead(buffer, length);
	} catch (...) {
		LogError(std::current_exception());
		return 0;
	}
}

size_t
decoder_read_much(DecoderClient *client, InputStream &is,
		  void *_buffer, size_t size) noexcept
{
	auto buffer = static_cast<uint8_t *>(_buffer);

	size_t total = 0;

	while (size > 0 && !is.LockIsEOF()) {
		size_t nbytes = decoder_read(client, is, buffer, size);
		if (nbytes == 0)
			return false;

		total += nbytes;
		buffer += nbytes;
		size -= nbytes;
	}

	return total;
}

bool
decoder_read_full(DecoderClient *client, InputStream &is,
		  void *_buffer, size_t size) noexcept
{
	auto buffer = static_cast<uint8_t *>(_buffer);

	while (size > 0) {
		size_t nbytes = decoder_read(client, is, buffer, size);
		if (nbytes == 0)
			return false;

		buffer += nbytes;
		size -= nbytes;
	}

	return true;
}

bool
decoder_skip(DecoderClient *client, InputStream &is, size_t size) noexcept
{
	while (size > 0) {
		char buffer[1024];
		size_t nbytes = decoder_read(client, is, buffer,
					     std::min(sizeof(buffer), size));
		if (nbytes == 0)
			return false;

		size -= nbytes;
	}

	return true;
}
