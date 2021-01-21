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

#include "CueParser.hxx"
#include "tag/ParseName.hxx"
#include "util/StringView.hxx"
#include "util/CharUtil.hxx"

#include <algorithm>
#include <cassert>

static StringView
cue_next_word(StringView &src) noexcept
{
	assert(!src.empty());
	assert(!IsWhitespaceNotNull(src.front()));

	auto end = std::find_if(src.begin(), src.end(),
				[](char ch){ return IsWhitespaceOrNull(ch); });
	StringView word(src.begin(), end);
	src = src.substr(end);
	return word;
}

static StringView
cue_next_quoted(StringView &src) noexcept
{
	assert(src.data[-1] == '"');

	auto end = src.Find('"');
	if (end == nullptr)
		/* syntax error - ignore it silently */
		return std::exchange(src, nullptr);

	StringView value(src.data, end);
	src = src.substr(end + 1);
	return value;
}

static StringView
cue_next_token(StringView &src) noexcept
{
	src.StripLeft();
	if (src.empty())
		return nullptr;

	return cue_next_word(src);
}

static const StringView
cue_next_value(StringView &src) noexcept
{
	src.StripLeft();
	if (src.empty())
		return nullptr;

	if (src.front() == '"') {
		src.pop_front();
		return cue_next_quoted(src);
	} else
		return cue_next_word(src);
}

static void
cue_add_tag(TagBuilder &tag, TagType type, StringView src) noexcept
{
	auto value = cue_next_value(src);
	if (value != nullptr)
		tag.AddItem(type, value);

}

static void
cue_parse_rem(StringView src, TagBuilder &tag) noexcept
{
	auto type = cue_next_token(src);
	if (type == nullptr)
		return;

	TagType type2 = tag_name_parse_i(type);
	if (type2 != TAG_NUM_OF_ITEM_TYPES)
		cue_add_tag(tag, type2, src);
}

TagBuilder *
CueParser::GetCurrentTag() noexcept
{
	if (state == HEADER)
		return &header_tag;
	else if (state == TRACK)
		return &song_tag;
	else
		return nullptr;
}

static bool
IsDigit(StringView s) noexcept
{
	return !s.empty() && IsDigitASCII(s.front());
}

static unsigned
cue_next_unsigned(StringView &src) noexcept
{
	if (!IsDigit(src)) {
		src = nullptr;
		return 0;
	}

	unsigned value = 0;

	do {
		char ch = src.front();
		src.pop_front();

		value = value * 10u + unsigned(ch - '0');
	} while (IsDigit(src));

	return value;
}

static int
cue_parse_position(StringView src) noexcept
{
	unsigned minutes = cue_next_unsigned(src);
	if (src.empty() || src.front() != ':')
		return -1;

	src.pop_front();
	unsigned seconds = cue_next_unsigned(src);
	if (src.empty() || src.front() != ':')
		return -1;

	src.pop_front();
	unsigned long frames = cue_next_unsigned(src);
	if (src == nullptr || !src.empty())
		return -1;

	return minutes * 60000 + seconds * 1000 + frames * 1000 / 75;
}

void
CueParser::Commit() noexcept
{
	/* the caller of this library must call cue_parser_get() often
	   enough */
	assert(finished == nullptr);
	assert(!end);

	if (current == nullptr)
		return;

	assert(!current->GetTag().IsDefined());
	current->SetTag(song_tag.Commit());

	finished = std::move(previous);
	previous = std::move(current);
	current.reset();
}

void
CueParser::Feed(StringView src) noexcept
{
	assert(!end);

	auto command = cue_next_token(src);
	if (command == nullptr)
		return;

	if (command.Equals("REM")) {
		TagBuilder *tag = GetCurrentTag();
		if (tag != nullptr)
			cue_parse_rem(src, *tag);
	} else if (command.Equals("PERFORMER")) {
		/* MPD knows a "performer" tag, but it is not a good
		   match for this CUE tag; from the Hydrogenaudio
		   Knowledgebase: "At top-level this will specify the
		   CD artist, while at track-level it specifies the
		   track artist." */

		TagType type = state == TRACK
			? TAG_ARTIST
			: TAG_ALBUM_ARTIST;

		TagBuilder *tag = GetCurrentTag();
		if (tag != nullptr)
			cue_add_tag(*tag, type, src);
	} else if (command.Equals("TITLE")) {
		if (state == HEADER)
			cue_add_tag(header_tag, TAG_ALBUM, src);
		else if (state == TRACK)
			cue_add_tag(song_tag, TAG_TITLE, src);
	} else if (command.Equals("FILE")) {
		Commit();

		const auto new_filename = cue_next_value(src);
		if (new_filename == nullptr)
			return;

		const auto type = cue_next_token(src);
		if (type == nullptr)
			return;

		if (!type.Equals("WAVE") &&
		    !type.Equals("FLAC") && /* non-standard */
		    !type.Equals("MP3") &&
		    !type.Equals("AIFF")) {
			state = IGNORE_FILE;
			return;
		}

		state = WAVE;
		filename = new_filename;
	} else if (state == IGNORE_FILE) {
		return;
	} else if (command.Equals("TRACK")) {
		Commit();

		const auto nr = cue_next_token(src);
		if (nr == nullptr)
			return;

		const auto type = cue_next_token(src);
		if (type == nullptr)
			return;

		if (!type.Equals("AUDIO")) {
			state = IGNORE_TRACK;
			return;
		}

		state = TRACK;
		ignore_index = false;
		current = std::make_unique<DetachedSong>(filename);
		assert(!current->GetTag().IsDefined());

		song_tag = header_tag;
		song_tag.AddItem(TAG_TRACK, nr);

	} else if (state == IGNORE_TRACK) {
		return;
	} else if (state == TRACK && command.Equals("INDEX")) {
		if (ignore_index)
			return;

		const auto nr = cue_next_token(src);
		if (nr == nullptr)
			return;

		const auto position = cue_next_token(src);
		if (position == nullptr)
			return;

		int position_ms = cue_parse_position(position);
		if (position_ms < 0)
			return;

		if (previous != nullptr && previous->GetStartTime().ToMS() < unsigned(position_ms))
			previous->SetEndTime(SongTime::FromMS(position_ms));

		if (current != nullptr)
			current->SetStartTime(SongTime::FromMS(position_ms));

		if (!nr.Equals("00") || previous == nullptr)
			ignore_index = true;
	}
}

void
CueParser::Finish() noexcept
{
	if (end)
		/* has already been called, ignore */
		return;

	Commit();
	end = true;
}

std::unique_ptr<DetachedSong>
CueParser::Get() noexcept
{
	if (finished == nullptr && end) {
		/* cue_parser_finish() has been called already:
		   deliver all remaining (partial) results */
		assert(current == nullptr);

		finished = std::move(previous);
		previous.reset();
	}

	auto result = std::move(finished);
	finished.reset();
	return result;
}
