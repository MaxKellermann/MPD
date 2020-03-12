/*
 * Copyright 2003-2020 The Music Player Daemon Project
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

#include "config.h"
#include "AllCommands.hxx"
#include "CommandError.hxx"
#include "Request.hxx"
#include "QueueCommands.hxx"
#include "TagCommands.hxx"
#include "PlayerCommands.hxx"
#include "PlaylistCommands.hxx"
#include "StorageCommands.hxx"
#include "DatabaseCommands.hxx"
#include "FileCommands.hxx"
#include "OutputCommands.hxx"
#include "MessageCommands.hxx"
#include "NeighborCommands.hxx"
#include "ClientCommands.hxx"
#include "PartitionCommands.hxx"
#include "FingerprintCommands.hxx"
#include "OtherCommands.hxx"
#include "Permission.hxx"
#include "tag/Type.h"
#include "Partition.hxx"
#include "Instance.hxx"
#include "client/Client.hxx"
#include "client/Response.hxx"
#include "util/Tokenizer.hxx"
#include "util/StringAPI.hxx"

#ifdef ENABLE_SQLITE
#include "StickerCommands.hxx"
#endif

#include <iterator>

#include <assert.h>
#include <string.h>

/*
 * The most we ever use is for search/find, and that limits it to the
 * number of tags we can have.  Add one for the command, and one extra
 * to catch errors clients may send us
 */
#define COMMAND_ARGV_MAX	(2+(TAG_NUM_OF_ITEM_TYPES*2))

/* if min: -1 don't check args *
 * if max: -1 no max args      */
struct command {
	const char *cmd;
	unsigned permission;
	int min;
	int max;
	CommandResult (*handler)(Client &client, Request request, Response &response);
};

/* don't be fooled, this is the command handler for "commands" command */
static CommandResult
handle_commands(Client &client, Request request, Response &response);

static CommandResult
handle_not_commands(Client &client, Request request, Response &response);

/**
 * The command registry.
 *
 * This array must be sorted!
 */
static constexpr struct command commands[] = {
	{ "add", PERMISSION_ADD, 1, 1, handle_add },
	{ "addid", PERMISSION_ADD, 1, 2, handle_addid },
	{ "addtagid", PERMISSION_ADD, 3, 3, handle_addtagid },
	{ "albumart", PERMISSION_READ, 2, 2, handle_album_art },
	{ "channels", PERMISSION_READ, 0, 0, handle_channels },
	{ "clear", PERMISSION_CONTROL, 0, 0, handle_clear },
	{ "clearerror", PERMISSION_CONTROL, 0, 0, handle_clearerror },
	{ "cleartagid", PERMISSION_ADD, 1, 2, handle_cleartagid },
	{ "close", PERMISSION_NONE, -1, -1, handle_close },
	{ "commands", PERMISSION_NONE, 0, 0, handle_commands },
	{ "config", PERMISSION_ADMIN, 0, 0, handle_config },
	{ "consume", PERMISSION_CONTROL, 1, 1, handle_consume },
#ifdef ENABLE_DATABASE
	{ "count", PERMISSION_READ, 1, -1, handle_count },
#endif
	{ "crossfade", PERMISSION_CONTROL, 1, 1, handle_crossfade },
	{ "currentsong", PERMISSION_READ, 0, 0, handle_currentsong },
	{ "decoders", PERMISSION_READ, 0, 0, handle_decoders },
	{ "delete", PERMISSION_CONTROL, 1, 1, handle_delete },
	{ "deleteid", PERMISSION_CONTROL, 1, 1, handle_deleteid },
	{ "delpartition", PERMISSION_ADMIN, 1, 1, handle_delpartition },
	{ "disableoutput", PERMISSION_ADMIN, 1, 1, handle_disableoutput },
	{ "enableoutput", PERMISSION_ADMIN, 1, 1, handle_enableoutput },
#ifdef ENABLE_DATABASE
	{ "find", PERMISSION_READ, 1, -1, handle_find },
	{ "findadd", PERMISSION_ADD, 1, -1, handle_findadd},
#endif
#ifdef ENABLE_CHROMAPRINT
	{ "getfingerprint", PERMISSION_READ, 1, 1, handle_getfingerprint },
#endif
	{ "idle", PERMISSION_READ, 0, -1, handle_idle },
	{ "kill", PERMISSION_ADMIN, -1, -1, handle_kill },
#ifdef ENABLE_DATABASE
	{ "list", PERMISSION_READ, 1, -1, handle_list },
	{ "listall", PERMISSION_READ, 0, 1, handle_listall },
	{ "listallinfo", PERMISSION_READ, 0, 1, handle_listallinfo },
#endif
	{ "listfiles", PERMISSION_READ, 0, 1, handle_listfiles },
#ifdef ENABLE_DATABASE
	{ "listmounts", PERMISSION_READ, 0, 0, handle_listmounts },
#endif
#ifdef ENABLE_NEIGHBOR_PLUGINS
	{ "listneighbors", PERMISSION_READ, 0, 0, handle_listneighbors },
#endif
	{ "listpartitions", PERMISSION_READ, 0, 0, handle_listpartitions },
	{ "listplaylist", PERMISSION_READ, 1, 1, handle_listplaylist },
	{ "listplaylistinfo", PERMISSION_READ, 1, 1, handle_listplaylistinfo },
	{ "listplaylists", PERMISSION_READ, 0, 0, handle_listplaylists },
	{ "load", PERMISSION_ADD, 1, 2, handle_load },
	{ "lsinfo", PERMISSION_READ, 0, 1, handle_lsinfo },
	{ "mixrampdb", PERMISSION_CONTROL, 1, 1, handle_mixrampdb },
	{ "mixrampdelay", PERMISSION_CONTROL, 1, 1, handle_mixrampdelay },
#ifdef ENABLE_DATABASE
	{ "mount", PERMISSION_ADMIN, 2, 2, handle_mount },
#endif
	{ "move", PERMISSION_CONTROL, 2, 2, handle_move },
	{ "moveid", PERMISSION_CONTROL, 2, 2, handle_moveid },
	{ "moveoutput", PERMISSION_ADMIN, 1, 1, handle_moveoutput },
	{ "newpartition", PERMISSION_ADMIN, 1, 1, handle_newpartition },
	{ "next", PERMISSION_CONTROL, 0, 0, handle_next },
	{ "notcommands", PERMISSION_NONE, 0, 0, handle_not_commands },
	{ "outputs", PERMISSION_READ, 0, 0, handle_devices },
	{ "outputset", PERMISSION_ADMIN, 3, 3, handle_outputset },
	{ "partition", PERMISSION_READ, 1, 1, handle_partition },
	{ "password", PERMISSION_NONE, 1, 1, handle_password },
	{ "pause", PERMISSION_CONTROL, 0, 1, handle_pause },
	{ "ping", PERMISSION_NONE, 0, 0, handle_ping },
	{ "play", PERMISSION_CONTROL, 0, 1, handle_play },
	{ "playid", PERMISSION_CONTROL, 0, 1, handle_playid },
	{ "playlist", PERMISSION_READ, 0, 0, handle_playlist },
	{ "playlistadd", PERMISSION_CONTROL, 2, 2, handle_playlistadd },
	{ "playlistclear", PERMISSION_CONTROL, 1, 1, handle_playlistclear },
	{ "playlistdelete", PERMISSION_CONTROL, 2, 2, handle_playlistdelete },
	{ "playlistfind", PERMISSION_READ, 1, -1, handle_playlistfind },
	{ "playlistid", PERMISSION_READ, 0, 1, handle_playlistid },
	{ "playlistinfo", PERMISSION_READ, 0, 1, handle_playlistinfo },
	{ "playlistmove", PERMISSION_CONTROL, 3, 3, handle_playlistmove },
	{ "playlistsearch", PERMISSION_READ, 1, -1, handle_playlistsearch },
	{ "plchanges", PERMISSION_READ, 1, 2, handle_plchanges },
	{ "plchangesposid", PERMISSION_READ, 1, 2, handle_plchangesposid },
	{ "previous", PERMISSION_CONTROL, 0, 0, handle_previous },
	{ "prio", PERMISSION_CONTROL, 2, -1, handle_prio },
	{ "prioid", PERMISSION_CONTROL, 2, -1, handle_prioid },
	{ "random", PERMISSION_CONTROL, 1, 1, handle_random },
	{ "rangeid", PERMISSION_ADD, 2, 2, handle_rangeid },
	{ "readcomments", PERMISSION_READ, 1, 1, handle_read_comments },
	{ "readmessages", PERMISSION_READ, 0, 0, handle_read_messages },
	{ "readpicture", PERMISSION_READ, 2, 2, handle_read_picture },
	{ "rename", PERMISSION_CONTROL, 2, 2, handle_rename },
	{ "repeat", PERMISSION_CONTROL, 1, 1, handle_repeat },
	{ "replay_gain_mode", PERMISSION_CONTROL, 1, 1,
	  handle_replay_gain_mode },
	{ "replay_gain_status", PERMISSION_READ, 0, 0,
	  handle_replay_gain_status },
	{ "rescan", PERMISSION_CONTROL, 0, 1, handle_rescan },
	{ "rm", PERMISSION_CONTROL, 1, 1, handle_rm },
	{ "save", PERMISSION_CONTROL, 1, 1, handle_save },
#ifdef ENABLE_DATABASE
	{ "search", PERMISSION_READ, 1, -1, handle_search },
	{ "searchadd", PERMISSION_ADD, 1, -1, handle_searchadd },
	{ "searchaddpl", PERMISSION_CONTROL, 2, -1, handle_searchaddpl },
#endif
	{ "seek", PERMISSION_CONTROL, 2, 2, handle_seek },
	{ "seekcur", PERMISSION_CONTROL, 1, 1, handle_seekcur },
	{ "seekid", PERMISSION_CONTROL, 2, 2, handle_seekid },
	{ "sendmessage", PERMISSION_CONTROL, 2, 2, handle_send_message },
	{ "setvol", PERMISSION_CONTROL, 1, 1, handle_setvol },
	{ "shuffle", PERMISSION_CONTROL, 0, 1, handle_shuffle },
	{ "single", PERMISSION_CONTROL, 1, 1, handle_single },
	{ "stats", PERMISSION_READ, 0, 0, handle_stats },
	{ "status", PERMISSION_READ, 0, 0, handle_status },
#ifdef ENABLE_SQLITE
	{ "sticker", PERMISSION_ADMIN, 3, -1, handle_sticker },
#endif
	{ "stop", PERMISSION_CONTROL, 0, 0, handle_stop },
	{ "subscribe", PERMISSION_READ, 1, 1, handle_subscribe },
	{ "swap", PERMISSION_CONTROL, 2, 2, handle_swap },
	{ "swapid", PERMISSION_CONTROL, 2, 2, handle_swapid },
	{ "tagstats", PERMISSION_READ, 0, 0, handle_tagstats },
	{ "tagtypes", PERMISSION_READ, 0, -1, handle_tagtypes },
	{ "toggleoutput", PERMISSION_ADMIN, 1, 1, handle_toggleoutput },
#ifdef ENABLE_DATABASE
	{ "unmount", PERMISSION_ADMIN, 1, 1, handle_unmount },
#endif
	{ "unsubscribe", PERMISSION_READ, 1, 1, handle_unsubscribe },
	{ "update", PERMISSION_CONTROL, 0, 1, handle_update },
	{ "urlhandlers", PERMISSION_READ, 0, 0, handle_urlhandlers },
	{ "volume", PERMISSION_CONTROL, 1, 1, handle_volume },
};

static constexpr unsigned num_commands = std::size(commands);

gcc_pure
static bool
command_available(gcc_unused const Partition &partition,
		  gcc_unused const struct command *cmd) noexcept
{
#ifdef ENABLE_SQLITE
	if (StringIsEqual(cmd->cmd, "sticker"))
		return partition.instance.HasStickerDatabase();
#endif

#ifdef ENABLE_NEIGHBOR_PLUGINS
	if (StringIsEqual(cmd->cmd, "listneighbors"))
		return neighbor_commands_available(partition.instance);
#endif

	if (StringIsEqual(cmd->cmd, "save") ||
	    StringIsEqual(cmd->cmd, "rm") ||
	    StringIsEqual(cmd->cmd, "rename") ||
	    StringIsEqual(cmd->cmd, "playlistdelete") ||
	    StringIsEqual(cmd->cmd, "playlistmove") ||
	    StringIsEqual(cmd->cmd, "playlistclear") ||
	    StringIsEqual(cmd->cmd, "playlistadd") ||
	    StringIsEqual(cmd->cmd, "listplaylists"))
		return playlist_commands_available();

	return true;
}

static CommandResult
PrintAvailableCommands(Response &r, const Partition &partition,
		     unsigned permission) noexcept
{
	for (const auto & i : commands) {
		const struct command *cmd = &i;

		if (cmd->permission == (permission & cmd->permission) &&
		    command_available(partition, cmd))
			r.Format("command: %s\n", cmd->cmd);
	}

	return CommandResult::OK;
}

static CommandResult
PrintUnavailableCommands(Response &r, unsigned permission) noexcept
{
	for (const auto & i : commands) {
		const struct command *cmd = &i;

		if (cmd->permission != (permission & cmd->permission))
			r.Format("command: %s\n", cmd->cmd);
	}

	return CommandResult::OK;
}

/* don't be fooled, this is the command handler for "commands" command */
static CommandResult
handle_commands(Client &client, gcc_unused Request request, Response &r)
{
	return PrintAvailableCommands(r, client.GetPartition(),
				      client.GetPermission());
}

static CommandResult
handle_not_commands(Client &client, gcc_unused Request request, Response &r)
{
	return PrintUnavailableCommands(r, client.GetPermission());
}

void
command_init() noexcept
{
#ifndef NDEBUG
	/* ensure that the command list is sorted */
	for (unsigned i = 0; i < num_commands - 1; ++i)
		assert(strcmp(commands[i].cmd, commands[i + 1].cmd) < 0);
#endif
}

gcc_pure
static const struct command *
command_lookup(const char *name) noexcept
{
	unsigned a = 0, b = num_commands, i;

	/* binary search */
	do {
		i = (a + b) / 2;

		const auto cmp = strcmp(name, commands[i].cmd);
		if (cmp == 0)
			return &commands[i];
		else if (cmp < 0)
			b = i;
		else if (cmp > 0)
			a = i + 1;
	} while (a < b);

	return nullptr;
}

static bool
command_check_request(const struct command *cmd, Response &r,
		      unsigned permission, Request args) noexcept
{
	if (cmd->permission != (permission & cmd->permission)) {
		r.FormatError(ACK_ERROR_PERMISSION,
			      "you don't have permission for \"%s\"",
			      cmd->cmd);
		return false;
	}

	const int min = cmd->min;
	const int max = cmd->max;

	if (min < 0)
		return true;

	if (min == max && unsigned(max) != args.size) {
		r.FormatError(ACK_ERROR_ARG,
			      "wrong number of arguments for \"%s\"",
			      cmd->cmd);
		return false;
	} else if (args.size < unsigned(min)) {
		r.FormatError(ACK_ERROR_ARG,
			      "too few arguments for \"%s\"", cmd->cmd);
		return false;
	} else if (max >= 0 && args.size > unsigned(max)) {
		r.FormatError(ACK_ERROR_ARG,
			      "too many arguments for \"%s\"", cmd->cmd);
		return false;
	} else
		return true;
}

static const struct command *
command_checked_lookup(Response &r, unsigned permission,
		       const char *cmd_name, Request args) noexcept
{
	const struct command *cmd = command_lookup(cmd_name);
	if (cmd == nullptr) {
		r.FormatError(ACK_ERROR_UNKNOWN,
			      "unknown command \"%s\"", cmd_name);
		return nullptr;
	}

	r.SetCommand(cmd->cmd);

	if (!command_check_request(cmd, r, permission, args))
		return nullptr;

	return cmd;
}

CommandResult
command_process(Client &client, unsigned num, char *line) noexcept
{
	Response r(client, num);

	/* get the command name (first word on the line) */
	/* we have to set current_command because Response::Error()
	   expects it to be set */

	Tokenizer tokenizer(line);

	const char *cmd_name;
	try {
		cmd_name = tokenizer.NextWord();
		if (cmd_name == nullptr) {
			r.Error(ACK_ERROR_UNKNOWN, "No command given");
			/* this client does not speak the MPD
			   protocol; kick the connection */
			return CommandResult::FINISH;
		}
	} catch (const std::exception &e) {
		r.Error(ACK_ERROR_UNKNOWN, e.what());
		/* this client does not speak the MPD protocol; kick
		   the connection */
		return CommandResult::FINISH;
	}

	char *argv[COMMAND_ARGV_MAX];
	Request args(argv, 0);

	try {
		/* now parse the arguments (quoted or unquoted) */

		while (true) {
			if (args.size == COMMAND_ARGV_MAX) {
				r.Error(ACK_ERROR_ARG, "Too many arguments");
				return CommandResult::ERROR;
			}

			char *a = tokenizer.NextParam();
			if (a == nullptr)
				break;

			argv[args.size++] = a;
		}

		/* look up and invoke the command handler */

		const struct command *cmd =
			command_checked_lookup(r, client.GetPermission(),
					       cmd_name, args);
		if (cmd == nullptr)
			return CommandResult::ERROR;

		return cmd->handler(client, args, r);
	} catch (...) {
		PrintError(r, std::current_exception());
		return CommandResult::ERROR;
	}
}
