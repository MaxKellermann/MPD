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

#include "StickerCommands.hxx"
#include "Request.hxx"
#include "SongPrint.hxx"
#include "db/Interface.hxx"
#include "sticker/Sticker.hxx"
#include "sticker/SongSticker.hxx"
#include "sticker/Print.hxx"
#include "client/Client.hxx"
#include "client/Response.hxx"
#include "Partition.hxx"
#include "Instance.hxx"
#include "util/StringAPI.hxx"
#include "util/ScopeExit.hxx"

namespace {
struct sticker_song_find_data {
	Response &r;
	const char *name;
};
} // namespace

static void
sticker_song_find_print_cb(const LightSong &song, const char *value,
			   void *user_data)
{
	auto *data =
		static_cast<struct sticker_song_find_data *>(user_data);

	song_print_uri(data->r, song);
	sticker_print_value(data->r, data->name, value);
}

static CommandResult
handle_sticker_song(Response &r, Partition &partition,
		    StickerDatabase &sticker_database,
		    Request args)
{
	const Database &db = partition.GetDatabaseOrThrow();

	const char *const cmd = args.front();

	/* get song song_id key */
	if (args.size == 4 && StringIsEqual(cmd, "get")) {
		const LightSong *song = db.GetSong(args[2]);
		assert(song != nullptr);
		AtScopeExit(&db, song) { db.ReturnSong(song); };

		const auto value = sticker_song_get_value(sticker_database,
							  *song, args[3]);
		if (value.empty()) {
			r.Error(ACK_ERROR_NO_EXIST, "no such sticker");
			return CommandResult::ERROR;
		}

		sticker_print_value(r, args[3], value.c_str());

		return CommandResult::OK;
	/* list song song_id */
	} else if (args.size == 3 && StringIsEqual(cmd, "list")) {
		const LightSong *song = db.GetSong(args[2]);
		assert(song != nullptr);
		AtScopeExit(&db, song) { db.ReturnSong(song); };

		const auto sticker = sticker_song_get(sticker_database, *song);
		sticker_print(r, sticker);

		return CommandResult::OK;
	/* set song song_id id key */
	} else if (args.size == 5 && StringIsEqual(cmd, "set")) {
		const LightSong *song = db.GetSong(args[2]);
		assert(song != nullptr);
		AtScopeExit(&db, song) { db.ReturnSong(song); };

		sticker_song_set_value(sticker_database, *song,
				       args[3], args[4]);
		return CommandResult::OK;
	/* delete song song_id [key] */
	} else if ((args.size == 3 || args.size == 4) &&
		   StringIsEqual(cmd, "delete")) {
		const LightSong *song = db.GetSong(args[2]);
		assert(song != nullptr);
		AtScopeExit(&db, song) { db.ReturnSong(song); };

		bool ret = args.size == 3
			? sticker_song_delete(sticker_database, *song)
			: sticker_song_delete_value(sticker_database, *song,
						    args[3]);
		if (!ret) {
			r.Error(ACK_ERROR_NO_EXIST, "no such sticker");
			return CommandResult::ERROR;
		}

		return CommandResult::OK;
	/* find song dir key */
	} else if ((args.size == 4 || args.size == 6) &&
		   StringIsEqual(cmd, "find")) {
		/* "sticker find song a/directory name" */

		const char *const base_uri = args[2];

		StickerOperator op = StickerOperator::EXISTS;
		const char *value = nullptr;

		if (args.size == 6) {
			/* match the value */

			const char *op_s = args[4];
			value = args[5];

			if (StringIsEqual(op_s, "="))
				op = StickerOperator::EQUALS;
			else if (StringIsEqual(op_s, "<"))
				op = StickerOperator::LESS_THAN;
			else if (StringIsEqual(op_s, ">"))
				op = StickerOperator::GREATER_THAN;
			else {
				r.Error(ACK_ERROR_ARG, "bad operator");
				return CommandResult::ERROR;
			}
		}

		struct sticker_song_find_data data = {
			r,
			args[3],
		};

		sticker_song_find(sticker_database, db, base_uri, data.name,
				  op, value,
				  sticker_song_find_print_cb, &data);

		return CommandResult::OK;
	} else {
		r.Error(ACK_ERROR_ARG, "bad request");
		return CommandResult::ERROR;
	}
}

CommandResult
handle_sticker(Client &client, Request args, Response &r)
{
	assert(args.size >= 3);

	auto &instance = client.GetInstance();
	if (!instance.HasStickerDatabase()) {
		r.Error(ACK_ERROR_UNKNOWN, "sticker database is disabled");
		return CommandResult::ERROR;
	}

	auto &sticker_database = *instance.sticker_database;

	if (StringIsEqual(args[1], "song"))
		return handle_sticker_song(r, client.GetPartition(),
					   sticker_database,
					   args);
	else {
		r.Error(ACK_ERROR_ARG, "unknown sticker domain");
		return CommandResult::ERROR;
	}
}
