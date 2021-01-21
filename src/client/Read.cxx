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

#include "Client.hxx"
#include "Config.hxx"
#include "Partition.hxx"
#include "Instance.hxx"
#include "util/StringStrip.hxx"

#include <cstring>

BufferedSocket::InputResult
Client::OnSocketInput(void *data, size_t length) noexcept
{
	if (background_command)
		return InputResult::PAUSE;

	char *p = static_cast<char *>(data);
	char *newline = static_cast<char *>(std::memchr(p, '\n', length));
	if (newline == nullptr)
		return InputResult::MORE;

	timeout_event.Schedule(client_timeout);

	BufferedSocket::ConsumeInput(newline + 1 - p);

	/* skip whitespace at the end of the line */
	char *end = StripRight(p, newline);

	/* terminate the string at the end of the line */
	*end = 0;

	CommandResult result = ProcessLine(p);
	switch (result) {
	case CommandResult::OK:
	case CommandResult::IDLE:
	case CommandResult::BACKGROUND:
	case CommandResult::ERROR:
		break;

	case CommandResult::KILL:
		partition->instance.Break();
		Close();
		return InputResult::CLOSED;

	case CommandResult::FINISH:
		if (Flush())
			Close();
		return InputResult::CLOSED;

	case CommandResult::CLOSE:
		Close();
		return InputResult::CLOSED;
	}

	if (IsExpired()) {
		Close();
		return InputResult::CLOSED;
	}

	return InputResult::AGAIN;
}
