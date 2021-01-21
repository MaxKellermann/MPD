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

#include "FullyBufferedSocket.hxx"
#include "net/SocketError.hxx"
#include "util/Compiler.h"

#include <cassert>

#include <string.h>

FullyBufferedSocket::ssize_t
FullyBufferedSocket::DirectWrite(const void *data, size_t length) noexcept
{
	const auto nbytes = GetSocket().Write(static_cast<const char *>(data), length);
	if (gcc_unlikely(nbytes < 0)) {
		const auto code = GetSocketError();
		if (IsSocketErrorAgain(code))
			return 0;

		idle_event.Cancel();
		event.Cancel();

		if (IsSocketErrorClosed(code))
			OnSocketClosed();
		else
			OnSocketError(std::make_exception_ptr(MakeSocketError(code, "Failed to send to socket")));
	}

	return nbytes;
}

bool
FullyBufferedSocket::Flush() noexcept
{
	assert(IsDefined());

	const auto data = output.Read();
	if (data.empty()) {
		idle_event.Cancel();
		event.CancelWrite();
		return true;
	}

	auto nbytes = DirectWrite(data.data, data.size);
	if (gcc_unlikely(nbytes <= 0))
		return nbytes == 0;

	output.Consume(nbytes);

	if (output.empty()) {
		idle_event.Cancel();
		event.CancelWrite();
	}

	return true;
}

bool
FullyBufferedSocket::Write(const void *data, size_t length) noexcept
{
	assert(IsDefined());

	if (length == 0)
		return true;

	const bool was_empty = output.empty();

	if (!output.Append(data, length)) {
		OnSocketError(std::make_exception_ptr(std::runtime_error("Output buffer is full")));
		return false;
	}

	if (was_empty)
		idle_event.Schedule();
	return true;
}

void
FullyBufferedSocket::OnSocketReady(unsigned flags) noexcept
{
	if (flags & SocketEvent::WRITE) {
		assert(!output.empty());
		assert(!idle_event.IsPending());

		if (!Flush())
			return;
	}

	BufferedSocket::OnSocketReady(flags);
}

void
FullyBufferedSocket::OnIdle() noexcept
{
	if (Flush() && !output.empty())
		event.ScheduleWrite();
}
