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

#include "RssPlaylistPlugin.hxx"
#include "../PlaylistPlugin.hxx"
#include "../MemorySongEnumerator.hxx"
#include "tag/Builder.hxx"
#include "util/ASCII.hxx"
#include "util/StringView.hxx"
#include "lib/expat/ExpatParser.hxx"

/**
 * This is the state object for the our XML parser.
 */
struct RssParser {
	/**
	 * The list of songs (in reverse order because that's faster
	 * while adding).
	 */
	std::forward_list<DetachedSong> songs;

	/**
	 * The current position in the XML file.
	 */
	enum {
		ROOT, ITEM,
	} state{ROOT};

	/**
	 * The current tag within the "entry" element.  This is only
	 * valid if state==ITEM.  TAG_NUM_OF_ITEM_TYPES means there
	 * is no (known) tag.
	 */
	TagType tag_type;

	/**
	 * The current song URI.  It is set by the "enclosure"
	 * element.
	 */
	std::string location;

	TagBuilder tag_builder;

	RssParser() = default;
};

static void XMLCALL
rss_start_element(void *user_data, const XML_Char *element_name,
		  const XML_Char **atts)
{
	auto *parser = static_cast<RssParser *>(user_data);

	switch (parser->state) {
	case RssParser::ROOT:
		if (StringEqualsCaseASCII(element_name, "item")) {
			parser->state = RssParser::ITEM;
			parser->location.clear();
			parser->tag_type = TAG_NUM_OF_ITEM_TYPES;
		}

		break;

	case RssParser::ITEM:
		if (StringEqualsCaseASCII(element_name, "enclosure")) {
			const char *href =
				ExpatParser::GetAttributeCase(atts, "url");
			if (href != nullptr)
				parser->location = href;
		} else if (StringEqualsCaseASCII(element_name, "title"))
			parser->tag_type = TAG_TITLE;
		else if (StringEqualsCaseASCII(element_name, "itunes:author"))
			parser->tag_type = TAG_ARTIST;

		break;
	}
}

static void XMLCALL
rss_end_element(void *user_data, const XML_Char *element_name)
{
	auto *parser = static_cast<RssParser *>(user_data);

	switch (parser->state) {
	case RssParser::ROOT:
		break;

	case RssParser::ITEM:
		if (StringEqualsCaseASCII(element_name, "item")) {
			if (!parser->location.empty())
				parser->songs.emplace_front(std::move(parser->location),
							    parser->tag_builder.Commit());

			parser->state = RssParser::ROOT;
		} else
			parser->tag_type = TAG_NUM_OF_ITEM_TYPES;

		break;
	}
}

static void XMLCALL
rss_char_data(void *user_data, const XML_Char *s, int len)
{
	auto *parser = static_cast<RssParser *>(user_data);

	switch (parser->state) {
	case RssParser::ROOT:
		break;

	case RssParser::ITEM:
		if (parser->tag_type != TAG_NUM_OF_ITEM_TYPES)
			parser->tag_builder.AddItem(parser->tag_type,
						    StringView(s, len));

		break;
	}
}

/*
 * The playlist object
 *
 */

static std::unique_ptr<SongEnumerator>
rss_open_stream(InputStreamPtr &&is)
{
	RssParser parser;

	{
		ExpatParser expat(&parser);
		expat.SetElementHandler(rss_start_element, rss_end_element);
		expat.SetCharacterDataHandler(rss_char_data);
		expat.Parse(*is);
	}

	parser.songs.reverse();
	return std::make_unique<MemorySongEnumerator>(std::move(parser.songs));
}

static const char *const rss_suffixes[] = {
	"rss",
	nullptr
};

static const char *const rss_mime_types[] = {
	"application/rss+xml",
	"application/xml",
	"text/xml",
	nullptr
};

const PlaylistPlugin rss_playlist_plugin =
	PlaylistPlugin("rss", rss_open_stream)
	.WithSuffixes(rss_suffixes)
	.WithMimeTypes(rss_mime_types);
