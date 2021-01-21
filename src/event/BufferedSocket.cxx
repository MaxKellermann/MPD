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

#include "BufferedSocket.hxx"
#include "net/SocketError.hxx"
#include "util/Compiler.h"

#include <stdexcept>

BufferedSocket::ssize_t
BufferedSocket::DirectRead(void *data, size_t length) noexcept
{
	const auto nbytes = GetSocket().Read(static_cast<char *>(data), length);
	if (gcc_likely(nbytes > 0))
		return nbytes;

	if (nbytes == 0) {
		OnSocketClosed();
		return -1;
	}

	const auto code = GetSocketError();
	if (IsSocketErrorAgain(code))
		return 0;

	if (IsSocketErrorClosed(code))
		OnSocketClosed();
	else
		OnSocketError(std::make_exception_ptr(MakeSocketError(code, "Failed to receive from socket")));
	return -1;
}

bool
BufferedSocket::ReadToBuffer() noexcept
{
	assert(IsDefined());

	const auto buffer = input.Write();
	assert(!buffer.empty());

	const auto nbytes = DirectRead(buffer.data, buffer.size);
	if (nbytes > 0)
		input.Append(nbytes);

	return nbytes >= 0;
}

bool
BufferedSocket::ResumeInput() noexcept
{
	assert(IsDefined());

	while (true) {
		const auto buffer = input.Read();
		if (buffer.empty()) {
			event.ScheduleRead();
			return true;
		}

		const auto result = OnSocketInput(buffer.data, buffer.size);
		switch (result) {
		case InputResult::MORE:
			if (input.IsFull()) {
				OnSocketError(std::make_exception_ptr(std::runtime_error("Input buffer is full")));
				return false;
			}

			event.ScheduleRead();
			return true;

		case InputResult::PAUSE:
			event.CancelRead();
			return true;

		case InputResult::AGAIN:
			continue;

		case InputResult::CLOSED:
			return false;
		}
	}
}

void
BufferedSocket::OnSocketReady(unsigned flags) noexcept
{
	assert(IsDefined());

	if (gcc_unlikely(flags & (SocketEvent::ERROR|SocketEvent::HANGUP))) {
		OnSocketClosed();
		return;
	}

	if (flags & SocketEvent::READ) {
		assert(!input.IsFull());

		if (!ReadToBuffer() || !ResumeInput())
			return;

		if (!input.IsFull())
			event.ScheduleRead();
	}
}
