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

#ifndef MPD_DATABASE_STATS_HXX
#define MPD_DATABASE_STATS_HXX

#include "Chrono.hxx"
#include "tag/Type.h"
#include <array>

struct DatabaseStats {
	/**
	 * Number of songs.
	 */
	unsigned song_count;

	/**
	 * Total duration of all songs (in seconds).
	 */
	std::chrono::duration<std::uint64_t, SongTime::period> total_duration;

	/**
	 * Number of distinct tag values for each of the tags defined in enum TagType.
	 */
	std::array<unsigned, TagType::TAG_NUM_OF_ITEM_TYPES> tag_counts;

	void Clear() {
		song_count = 0;
		total_duration = total_duration.zero();
		tag_counts.fill(0);
	}
};

#endif
