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

#include "XspfPlaylistPlugin.hxx"
#include "../PlaylistPlugin.hxx"
#include "../MemorySongEnumerator.hxx"
#include "song/DetachedSong.hxx"
#include "input/InputStream.hxx"
#include "tag/Builder.hxx"
#include "tag/Table.hxx"
#include "util/StringView.hxx"
#include "lib/expat/ExpatParser.hxx"

#include <string.h>

/**
 * This is the state object for our XML parser.
 */
struct XspfParser {
	/**
	 * The list of songs (in reverse order because that's faster
	 * while adding).
	 */
	std::forward_list<DetachedSong> songs;

	/**
	 * The current position in the XML file.
	 */
	enum {
		ROOT, PLAYLIST, TRACKLIST, TRACK,
		TAG, LOCATION,
	} state = ROOT;

	/**
	 * The current tag within the "track" element.  This is only
	 * valid if state==TRACK.  TAG_NUM_OF_ITEM_TYPES means there
	 * is no (known) tag.
	 */
	TagType tag_type;

	/**
	 * The current song URI.  It is set by the "location" element.
	 */
	std::string location;

	TagBuilder tag_builder;

	std::string value;
};

static constexpr struct tag_table xspf_tag_elements[] = {
	{ "title", TAG_TITLE },

	/* TAG_COMPOSER would be more correct according to the XSPF
	   spec */
	{ "creator", TAG_ARTIST },

	{ "annotation", TAG_COMMENT },
	{ "album", TAG_ALBUM },
	{ "trackNum", TAG_TRACK },
	{ nullptr, TAG_NUM_OF_ITEM_TYPES }
};

static void XMLCALL
xspf_start_element(void *user_data, const XML_Char *element_name,
		   [[maybe_unused]] const XML_Char **atts)
{
	auto *parser = static_cast<XspfParser *>(user_data);
	parser->value.clear();

	switch (parser->state) {
	case XspfParser::ROOT:
		if (strcmp(element_name, "playlist") == 0)
			parser->state = XspfParser::PLAYLIST;

		break;

	case XspfParser::PLAYLIST:
		if (strcmp(element_name, "trackList") == 0)
			parser->state = XspfParser::TRACKLIST;

		break;

	case XspfParser::TRACKLIST:
		if (strcmp(element_name, "track") == 0) {
			parser->state = XspfParser::TRACK;
			parser->location.clear();
		}

		break;

	case XspfParser::TRACK:
		if (strcmp(element_name, "location") == 0)
			parser->state = XspfParser::LOCATION;
		else if (!parser->location.empty()) {
			parser->tag_type = tag_table_lookup(xspf_tag_elements,
							    element_name);
			if (parser->tag_type != TAG_NUM_OF_ITEM_TYPES)
				parser->state = XspfParser::TAG;
		}

		break;

	case XspfParser::TAG:
	case XspfParser::LOCATION:
		break;
	}
}

static void XMLCALL
xspf_end_element(void *user_data, const XML_Char *element_name)
{
	auto *parser = static_cast<XspfParser *>(user_data);

	switch (parser->state) {
	case XspfParser::ROOT:
		break;

	case XspfParser::PLAYLIST:
		if (strcmp(element_name, "playlist") == 0)
			parser->state = XspfParser::ROOT;

		break;

	case XspfParser::TRACKLIST:
		if (strcmp(element_name, "tracklist") == 0)
			parser->state = XspfParser::PLAYLIST;

		break;

	case XspfParser::TRACK:
		if (strcmp(element_name, "track") == 0) {
			if (!parser->location.empty())
				parser->songs.emplace_front(std::move(parser->location),
							    parser->tag_builder.Commit());

			parser->state = XspfParser::TRACKLIST;
		}

		break;

	case XspfParser::TAG:
		if (!parser->value.empty())
			parser->tag_builder.AddItem(parser->tag_type,
						    StringView(parser->value.data(),
							       parser->value.length()));

		parser->state = XspfParser::TRACK;
		break;

	case XspfParser::LOCATION:
		parser->location = std::move(parser->value);
		parser->state = XspfParser::TRACK;
		break;
	}

	parser->value.clear();
}

static void XMLCALL
xspf_char_data(void *user_data, const XML_Char *s, int len)
{
	auto *parser = static_cast<XspfParser *>(user_data);

	switch (parser->state) {
	case XspfParser::ROOT:
	case XspfParser::PLAYLIST:
	case XspfParser::TRACKLIST:
	case XspfParser::TRACK:
		break;

	case XspfParser::TAG:
	case XspfParser::LOCATION:
		parser->value.append(s, len);
		break;
	}
}

/*
 * The playlist object
 *
 */

static std::unique_ptr<SongEnumerator>
xspf_open_stream(InputStreamPtr &&is)
{
	XspfParser parser;

	{
		ExpatParser expat(&parser);
		expat.SetElementHandler(xspf_start_element, xspf_end_element);
		expat.SetCharacterDataHandler(xspf_char_data);
		expat.Parse(*is);
	}

	parser.songs.reverse();
	return std::make_unique<MemorySongEnumerator>(std::move(parser.songs));
}

static const char *const xspf_suffixes[] = {
	"xspf",
	nullptr
};

static const char *const xspf_mime_types[] = {
	"application/xspf+xml",
	nullptr
};

const PlaylistPlugin xspf_playlist_plugin =
	PlaylistPlugin("xspf", xspf_open_stream)
	.WithSuffixes(xspf_suffixes)
	.WithMimeTypes(xspf_mime_types);
