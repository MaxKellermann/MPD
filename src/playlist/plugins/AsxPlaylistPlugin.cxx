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

#include "AsxPlaylistPlugin.hxx"
#include "../PlaylistPlugin.hxx"
#include "../MemorySongEnumerator.hxx"
#include "tag/Builder.hxx"
#include "tag/Table.hxx"
#include "util/ASCII.hxx"
#include "util/StringView.hxx"
#include "lib/expat/ExpatParser.hxx"

/**
 * This is the state object for our XML parser.
 */
struct AsxParser {
	/**
	 * The list of songs (in reverse order because that's faster
	 * while adding).
	 */
	std::forward_list<DetachedSong> songs;

	/**
	 * The current position in the XML file.
	 */
	enum {
		ROOT, ENTRY,
		TAG,
	} state{ROOT};

	/**
	 * The current tag within the "entry" element.  This is only
	 * valid if state==ENTRY.  TAG_NUM_OF_ITEM_TYPES means there
	 * is no (known) tag.
	 */
	TagType tag_type;

	/**
	 * The current song URI.  It is set by the "ref" element.
	 */
	std::string location;

	TagBuilder tag_builder;

	std::string value;
};

static constexpr struct tag_table asx_tag_elements[] = {
	/* is that correct?  or should it be COMPOSER or PERFORMER? */
	{ "author", TAG_ARTIST },

	{ "title", TAG_TITLE },
	{ nullptr, TAG_NUM_OF_ITEM_TYPES }
};

static void XMLCALL
asx_start_element(void *user_data, const XML_Char *element_name,
		  const XML_Char **atts)
{
	auto *parser = static_cast<AsxParser *>(user_data);
	parser->value.clear();

	switch (parser->state) {
	case AsxParser::ROOT:
		if (StringEqualsCaseASCII(element_name, "entry")) {
			parser->state = AsxParser::ENTRY;
			parser->location.clear();
		}

		break;

	case AsxParser::ENTRY:
		if (StringEqualsCaseASCII(element_name, "ref")) {
			const char *href =
				ExpatParser::GetAttributeCase(atts, "href");
			if (href != nullptr)
				parser->location = href;
		} else {
			parser->tag_type = tag_table_lookup_i(asx_tag_elements,
							      element_name);
			if (parser->tag_type != TAG_NUM_OF_ITEM_TYPES)
				parser->state = AsxParser::TAG;
		}

		break;

	case AsxParser::TAG:
		break;
	}
}

static void XMLCALL
asx_end_element(void *user_data, const XML_Char *element_name)
{
	auto *parser = static_cast<AsxParser *>(user_data);

	switch (parser->state) {
	case AsxParser::ROOT:
		break;

	case AsxParser::ENTRY:
		if (StringEqualsCaseASCII(element_name, "entry")) {
			if (!parser->location.empty())
				parser->songs.emplace_front(std::move(parser->location),
							    parser->tag_builder.Commit());

			parser->state = AsxParser::ROOT;
		}

		break;

	case AsxParser::TAG:
		if (!parser->value.empty())
			parser->tag_builder.AddItem(parser->tag_type,
						    StringView(parser->value.data(),
							       parser->value.length()));
		parser->state = AsxParser::ENTRY;
		break;
	}

	parser->value.clear();
}

static void XMLCALL
asx_char_data(void *user_data, const XML_Char *s, int len)
{
	auto *parser = static_cast<AsxParser *>(user_data);

	switch (parser->state) {
	case AsxParser::ROOT:
	case AsxParser::ENTRY:
		break;

	case AsxParser::TAG:
		parser->value.append(s, len);
		break;
	}
}

/*
 * The playlist object
 *
 */

static std::unique_ptr<SongEnumerator>
asx_open_stream(InputStreamPtr &&is)
{
	AsxParser parser;

	{
		ExpatParser expat(&parser);
		expat.SetElementHandler(asx_start_element, asx_end_element);
		expat.SetCharacterDataHandler(asx_char_data);
		expat.Parse(*is);
	}

	parser.songs.reverse();
	return std::make_unique<MemorySongEnumerator>(std::move(parser.songs));
}

static const char *const asx_suffixes[] = {
	"asx",
	nullptr
};

static const char *const asx_mime_types[] = {
	"video/x-ms-asf",
	nullptr
};

const PlaylistPlugin asx_playlist_plugin =
	PlaylistPlugin("asx", asx_open_stream)
	.WithSuffixes(asx_suffixes)
	.WithMimeTypes(asx_mime_types);
