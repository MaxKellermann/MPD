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

#include "SongSticker.hxx"
#include "Sticker.hxx"
#include "Database.hxx"
#include "song/LightSong.hxx"
#include "db/Interface.hxx"
#include "util/Alloc.hxx"
#include "util/ScopeExit.hxx"

#include <string.h>
#include <stdlib.h>

std::string
sticker_song_get_value(StickerDatabase &db,
		       const LightSong &song, const char *name)
{
	const auto uri = song.GetURI();
	return db.LoadValue("song", uri.c_str(), name);
}

void
sticker_song_set_value(StickerDatabase &db,
		       const LightSong &song,
		       const char *name, const char *value)
{
	const auto uri = song.GetURI();
	db.StoreValue("song", uri.c_str(), name, value);
}

bool
sticker_song_delete(StickerDatabase &db, const char *uri)
{
	return db.Delete("song", uri);
}

bool
sticker_song_delete(StickerDatabase &db, const LightSong &song)
{
	return sticker_song_delete(db, song.GetURI().c_str());
}

bool
sticker_song_delete_value(StickerDatabase &db,
			  const LightSong &song, const char *name)
{
	const auto uri = song.GetURI();
	return db.DeleteValue("song", uri.c_str(), name);
}

Sticker
sticker_song_get(StickerDatabase &db, const LightSong &song)
{
	const auto uri = song.GetURI();
	return db.Load("song", uri.c_str());
}

namespace {
struct sticker_song_find_data {
	const Database *db;
	const char *base_uri;
	size_t base_uri_length;

	void (*func)(const LightSong &song, const char *value,
		     void *user_data);
	void *user_data;
};
} // namespace

static void
sticker_song_find_cb(const char *uri, const char *value, void *user_data)
{
	auto *data =
		static_cast<struct sticker_song_find_data *>(user_data);

	if (memcmp(uri, data->base_uri, data->base_uri_length) != 0)
		/* should not happen, ignore silently */
		return;

	const Database *db = data->db;
	try {
		const LightSong *song = db->GetSong(uri);
		data->func(*song, value, data->user_data);
		db->ReturnSong(song);
	} catch (...) {
	}
}

void
sticker_song_find(StickerDatabase &sticker_database, const Database &db,
		  const char *base_uri, const char *name,
		  StickerOperator op, const char *value,
		  void (*func)(const LightSong &song, const char *value,
			       void *user_data),
		  void *user_data)
{
	struct sticker_song_find_data data;
	data.db = &db;
	data.func = func;
	data.user_data = user_data;

	char *allocated;
	data.base_uri = base_uri;
	if (*data.base_uri != 0)
		/* append slash to base_uri */
		data.base_uri = allocated =
			xstrcatdup(data.base_uri, "/");
	else
		/* searching in root directory - no trailing slash */
		allocated = nullptr;

	AtScopeExit(allocated) { free(allocated); };

	data.base_uri_length = strlen(data.base_uri);

	sticker_database.Find("song", data.base_uri, name, op, value,
			      sticker_song_find_cb, &data);
}
