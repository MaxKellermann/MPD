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
#include "Domain.hxx"
#include "protocol/Result.hxx"
#include "command/AllCommands.hxx"
#include "Log.hxx"
#include "util/StringAPI.hxx"
#include "util/CharUtil.hxx"

#define CLIENT_LIST_MODE_BEGIN "command_list_begin"
#define CLIENT_LIST_OK_MODE_BEGIN "command_list_ok_begin"
#define CLIENT_LIST_MODE_END "command_list_end"

inline CommandResult
Client::ProcessCommandList(bool list_ok,
			   std::list<std::string> &&list) noexcept
{
	unsigned n = 0;

	for (auto &&i : list) {
		char *cmd = &*i.begin();

		FormatDebug(client_domain, "process command \"%s\"", cmd);
		auto ret = command_process(*this, n++, cmd);
		FormatDebug(client_domain, "command returned %i", int(ret));
		if (IsExpired())
			return CommandResult::CLOSE;
		else if (ret != CommandResult::OK)
			return ret;
		else if (list_ok)
			Write("list_OK\n");
	}

	return CommandResult::OK;
}

CommandResult
Client::ProcessLine(char *line) noexcept
{
	assert(!background_command);

	if (!IsLowerAlphaASCII(*line)) {
		/* all valid MPD commands begin with a lower case
		   letter; this could be a badly routed HTTP
		   request */
		FormatWarning(client_domain,
			      "[%u] malformed command \"%s\"",
			      num, line);
		return CommandResult::CLOSE;
	}

	if (StringIsEqual(line, "noidle")) {
		if (idle_waiting) {
			/* send empty idle response and leave idle mode */
			idle_waiting = false;
			command_success(*this);
		}

		/* do nothing if the client wasn't idling: the client
		   has already received the full idle response from
		   IdleNotify(), which he can now evaluate */

		return CommandResult::OK;
	} else if (idle_waiting) {
		/* during idle mode, clients must not send anything
		   except "noidle" */
		FormatWarning(client_domain,
			      "[%u] command \"%s\" during idle",
			      num, line);
		return CommandResult::CLOSE;
	}

	if (cmd_list.IsActive()) {
		if (StringIsEqual(line, CLIENT_LIST_MODE_END)) {
			const unsigned id = num;

			FormatDebug(client_domain,
				    "[%u] process command list",
				    id);

			const bool ok_mode = cmd_list.IsOKMode();
			auto list = cmd_list.Commit();
			cmd_list.Reset();

			auto ret = ProcessCommandList(ok_mode,
						      std::move(list));
			FormatDebug(client_domain,
				    "[%u] process command "
				    "list returned %i", id, int(ret));

			if (ret == CommandResult::OK)
				command_success(*this);

			return ret;
		} else {
			if (!cmd_list.Add(line)) {
				FormatWarning(client_domain,
					      "[%u] command list size "
					      "is larger than the max (%lu)",
					      num,
					      static_cast<unsigned long>(client_max_command_list_size));
				return CommandResult::CLOSE;
			}

			return CommandResult::OK;
		}
	} else {
		if (StringIsEqual(line, CLIENT_LIST_MODE_BEGIN)) {
			cmd_list.Begin(false);
			return CommandResult::OK;
		} else if (StringIsEqual(line, CLIENT_LIST_OK_MODE_BEGIN)) {
			cmd_list.Begin(true);
			return CommandResult::OK;
		} else {
			const unsigned id = num;

			FormatDebug(client_domain,
				    "[%u] process command \"%s\"",
				    id, line);
			auto ret = command_process(*this, 0, line);
			FormatDebug(client_domain,
				    "[%u] command returned %i",
				    id, int(ret));

			if (IsExpired())
				return CommandResult::CLOSE;

			if (ret == CommandResult::OK)
				command_success(*this);

			return ret;
		}
	}
}
