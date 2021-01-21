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

#include "TidalTagScanner.hxx"
#include "TidalErrorParser.hxx"
#include "lib/yajl/Callbacks.hxx"
#include "tag/Builder.hxx"
#include "tag/Tag.hxx"

#include <cassert>

using Wrapper = Yajl::CallbacksWrapper<TidalTagScanner::ResponseParser>;
static constexpr yajl_callbacks parse_callbacks = {
	nullptr,
	nullptr,
	Wrapper::Integer,
	nullptr,
	nullptr,
	Wrapper::String,
	Wrapper::StartMap,
	Wrapper::MapKey,
	Wrapper::EndMap,
	nullptr,
	nullptr,
};

class TidalTagScanner::ResponseParser final : public YajlResponseParser {
	enum class State {
		NONE,
		TITLE,
		DURATION,
		ARTIST,
		ARTIST_NAME,
		ALBUM,
		ALBUM_TITLE,
	} state = State::NONE;

	unsigned map_depth = 0;

	TagBuilder tag;

public:
	explicit ResponseParser() noexcept
		:YajlResponseParser(&parse_callbacks, nullptr, this) {}

	Tag GetTag() {
		return tag.Commit();
	}

	/* yajl callbacks */
	bool Integer(long long value) noexcept;
	bool String(StringView value) noexcept;
	bool StartMap() noexcept;
	bool MapKey(StringView value) noexcept;
	bool EndMap() noexcept;
};

static std::string
MakeTrackUrl(const char *base_url, const char *track_id)
{
	return std::string(base_url)
		+ "/tracks/"
		+ track_id
		// TODO: configurable countryCode?
		+ "?countryCode=US";
}

TidalTagScanner::TidalTagScanner(CurlGlobal &curl,
				 const char *base_url, const char *token,
				 const char *track_id,
				 RemoteTagHandler &_handler)
	:request(curl, MakeTrackUrl(base_url, track_id).c_str(), *this),
	 handler(_handler)
{
	request_headers.Append((std::string("X-Tidal-Token:")
				+ token).c_str());
	request.SetOption(CURLOPT_HTTPHEADER, request_headers.Get());
}

TidalTagScanner::~TidalTagScanner() noexcept
{
	request.StopIndirect();
}

std::unique_ptr<CurlResponseParser>
TidalTagScanner::MakeParser(unsigned status,
			      std::multimap<std::string, std::string> &&headers)
{
	if (status != 200)
		return std::make_unique<TidalErrorParser>(status, headers);

	auto i = headers.find("content-type");
	if (i == headers.end() || i->second.find("/json") == i->second.npos)
		throw std::runtime_error("Not a JSON response from Tidal");

	return std::make_unique<ResponseParser>();
}

void
TidalTagScanner::FinishParser(std::unique_ptr<CurlResponseParser> p)
{
	assert(dynamic_cast<ResponseParser *>(p.get()) != nullptr);
	auto &rp = (ResponseParser &)*p;
	handler.OnRemoteTag(rp.GetTag());
}

void
TidalTagScanner::OnError(std::exception_ptr e) noexcept
{
	handler.OnRemoteTagError(e);
}

inline bool
TidalTagScanner::ResponseParser::Integer(long long value) noexcept
{
	switch (state) {
	case State::NONE:
	case State::TITLE:
	case State::ARTIST:
	case State::ARTIST_NAME:
	case State::ALBUM:
	case State::ALBUM_TITLE:
		break;

	case State::DURATION:
		if (map_depth == 1 && value > 0)
			tag.SetDuration(SignedSongTime::FromS(unsigned(value)));
		break;
	}

	return true;
}

inline bool
TidalTagScanner::ResponseParser::String(StringView value) noexcept
{
	switch (state) {
	case State::NONE:
	case State::DURATION:
	case State::ARTIST:
	case State::ALBUM:
		break;

	case State::TITLE:
		if (map_depth == 1)
			tag.AddItem(TAG_TITLE, value);
		break;

	case State::ARTIST_NAME:
		if (map_depth == 2)
			tag.AddItem(TAG_ARTIST, value);
		break;

	case State::ALBUM_TITLE:
		if (map_depth == 2)
			tag.AddItem(TAG_ALBUM, value);
		break;
	}

	return true;
}

inline bool
TidalTagScanner::ResponseParser::StartMap() noexcept
{
	++map_depth;
	return true;
}

inline bool
TidalTagScanner::ResponseParser::MapKey(StringView value) noexcept
{
	switch (map_depth) {
	case 1:
		if (value.Equals("title"))
			state = State::TITLE;
		else if (value.Equals("duration"))
			state = State::DURATION;
		else if (value.Equals("artist"))
			state = State::ARTIST;
		else if (value.Equals("album"))
			state = State::ALBUM;
		else
			state = State::NONE;
		break;

	case 2:
		switch (state) {
		case State::NONE:
		case State::TITLE:
		case State::DURATION:
			break;

		case State::ARTIST:
		case State::ARTIST_NAME:
			if (value.Equals("name"))
				state = State::ARTIST_NAME;
			else
				state = State::ARTIST;
			break;

		case State::ALBUM:
		case State::ALBUM_TITLE:
			if (value.Equals("title"))
				state = State::ALBUM_TITLE;
			else
				state = State::ALBUM;
			break;
		}
		break;
	}

	return true;
}

inline bool
TidalTagScanner::ResponseParser::EndMap() noexcept
{
	switch (map_depth) {
	case 2:
		state = State::NONE;
		break;
	}

	--map_depth;

	return true;
}
