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

#include "OpusHead.hxx"
#include "util/ByteOrder.hxx"

#include <cstdint>

struct OpusHead {
	char signature[8];
	uint8_t version, channels;
	uint16_t pre_skip;
	uint32_t sample_rate;
	uint16_t output_gain;
	uint8_t channel_mapping;
};

bool
ScanOpusHeader(const void *data, size_t size, unsigned &channels_r,
	       signed &output_gain_r, unsigned &pre_skip_r)
{
	const auto *h = static_cast<const OpusHead *>(data);
	if (size < 19 || (h->version & 0xf0) != 0)
		return false;

	output_gain_r = FromLE16S(h->output_gain);

	channels_r = h->channels;
	pre_skip_r = FromLE16(h->pre_skip);
	return true;
}
