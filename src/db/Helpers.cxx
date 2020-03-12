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

#include "Helpers.hxx"
#include "Stats.hxx"
#include "Interface.hxx"
#include "song/LightSong.hxx"
#include "tag/Tag.hxx"
#include "tag/Type.h"

#include <set>

#include <string.h>

struct StringLess {
	gcc_pure
	bool operator()(const char *a, const char *b) const noexcept {
		return strcmp(a, b) < 0;
	}
};

using StringSet  = std::set<const char *, StringLess>;
using TagsValues = std::array<StringSet, TagType::TAG_NUM_OF_ITEM_TYPES>;
using TagsCounts = std::array<unsigned,  TagType::TAG_NUM_OF_ITEM_TYPES>;

static void
StatsVisitTag(DatabaseStats &stats, TagsValues &tags_values,
	      const Tag &tag) noexcept
{
	if (!tag.duration.IsNegative())
		stats.total_duration += tag.duration;

	for (const auto &item : tag) {
		TagType tag_type = item.type;
		if (tag_type < TagType::TAG_NUM_OF_ITEM_TYPES) {
			tags_values[tag_type].emplace(item.value);
		}
	}
}

static void
StatsVisitSong(DatabaseStats &stats, TagsValues &tags_values,
	       const LightSong &song) noexcept
{
	++stats.song_count;

	StatsVisitTag(stats, tags_values, song.tag);
}

DatabaseStats
GetStats(const Database &db, const DatabaseSelection &selection)
{
	DatabaseStats stats;
	stats.Clear();

	TagsValues tags_values;

	using namespace std::placeholders;
	const auto f = std::bind(StatsVisitSong,
				 std::ref(stats),
				 std::ref(tags_values),
				 _1);
	db.Visit(selection, f);

	TagsValues::const_iterator values      = tags_values.begin();
	TagsCounts::iterator count             = stats.tag_counts.begin();
	TagsCounts::const_iterator counts_end  = stats.tag_counts.end();

	static_assert(tags_values.size() == stats.tag_counts.size());

	while (count != counts_end) {
		*count++ = values++->size();
	}

	return stats;
}
