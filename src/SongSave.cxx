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

#include "SongSave.hxx"
#include "pcm/AudioParser.hxx"
#include "db/plugins/simple/Song.hxx"
#include "song/DetachedSong.hxx"
#include "TagSave.hxx"
#include "fs/io/TextFile.hxx"
#include "fs/io/BufferedOutputStream.hxx"
#include "tag/ParseName.hxx"
#include "tag/Tag.hxx"
#include "tag/Builder.hxx"
#include "time/ChronoUtil.hxx"
#include "util/StringAPI.hxx"
#include "util/StringBuffer.hxx"
#include "util/StringStrip.hxx"
#include "util/RuntimeError.hxx"
#include "util/NumberParser.hxx"

#include <stdlib.h>

#define SONG_MTIME "mtime"
#define SONG_END "song_end"

static void
range_save(BufferedOutputStream &os, unsigned start_ms, unsigned end_ms)
{
	if (end_ms > 0)
		os.Format("Range: %u-%u\n", start_ms, end_ms);
	else if (start_ms > 0)
		os.Format("Range: %u-\n", start_ms);
}

void
song_save(BufferedOutputStream &os, const Song &song)
{
	os.Format(SONG_BEGIN "%s\n", song.filename.c_str());

	if (!song.target.empty())
		os.Format("Target: %s\n", song.target.c_str());

	range_save(os, song.start_time.ToMS(), song.end_time.ToMS());

	tag_save(os, song.tag);

	if (song.audio_format.IsDefined())
		os.Format("Format: %s\n", ToString(song.audio_format).c_str());

	if (!IsNegative(song.mtime))
		os.Format(SONG_MTIME ": %li\n",
			  long(std::chrono::system_clock::to_time_t(song.mtime)));
	os.Format(SONG_END "\n");
}

void
song_save(BufferedOutputStream &os, const DetachedSong &song)
{
	os.Format(SONG_BEGIN "%s\n", song.GetURI());

	range_save(os, song.GetStartTime().ToMS(), song.GetEndTime().ToMS());

	tag_save(os, song.GetTag());

	if (!IsNegative(song.GetLastModified()))
		os.Format(SONG_MTIME ": %li\n",
			  long(std::chrono::system_clock::to_time_t(song.GetLastModified())));
	os.Format(SONG_END "\n");
}

DetachedSong
song_load(TextFile &file, const char *uri,
	  std::string *target_r,
	  AudioFormat *audio_format_r)
{
	DetachedSong song(uri);

	TagBuilder tag;

	char *line;
	while ((line = file.ReadLine()) != nullptr &&
	       !StringIsEqual(line, SONG_END)) {
		char *colon = std::strchr(line, ':');
		if (colon == nullptr || colon == line) {
			throw FormatRuntimeError("unknown line in db: %s", line);
		}

		*colon++ = 0;
		const char *value = StripLeft(colon);

		TagType type;
		if ((type = tag_name_parse(line)) != TAG_NUM_OF_ITEM_TYPES) {
			tag.AddItem(type, value);
		} else if (StringIsEqual(line, "Time")) {
			tag.SetDuration(SignedSongTime::FromS(ParseDouble(value)));
		} else if (StringIsEqual(line, "Target")) {
			if (target_r != nullptr)
				*target_r = value;
		} else if (StringIsEqual(line, "Format")) {
			if (audio_format_r != nullptr) {
				try {
					*audio_format_r =
						ParseAudioFormat(value, false);
				} catch (...) {
					/* ignore parser errors */
				}
			}
		} else if (StringIsEqual(line, "Playlist")) {
			tag.SetHasPlaylist(StringIsEqual(value, "yes"));
		} else if (StringIsEqual(line, SONG_MTIME)) {
			song.SetLastModified(std::chrono::system_clock::from_time_t(atoi(value)));
		} else if (StringIsEqual(line, "Range")) {
			char *endptr;

			unsigned start_ms = strtoul(value, &endptr, 10);
			unsigned end_ms = *endptr == '-'
				? strtoul(endptr + 1, nullptr, 10)
				: 0;

			song.SetStartTime(SongTime::FromMS(start_ms));
			song.SetEndTime(SongTime::FromMS(end_ms));
		} else {
			throw FormatRuntimeError("unknown line in db: %s", line);
		}
	}

	song.SetTag(tag.Commit());
	return song;
}
