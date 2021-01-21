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

#include "Rva2.hxx"
#include "ReplayGainInfo.hxx"

#include <id3tag.h>

#include <cstdint>

#include <string.h>

enum class Rva2Channel : uint8_t {
	OTHER = 0x00,
	MASTER_VOLUME = 0x01,
	FRONT_RIGHT = 0x02,
	FRONT_LEFT = 0x03,
	BACK_RIGHT = 0x04,
	BACK_LEFT = 0x05,
	FRONT_CENTRE = 0x06,
	BACK_CENTRE = 0x07,
	SUBWOOFER = 0x08
};

struct Rva2Data {
	Rva2Channel type;
	uint8_t volume_adjustment[2];
	uint8_t peak_bits;
};

static inline id3_length_t
rva2_peak_bytes(const Rva2Data &data)
{
	return (data.peak_bits + 7) / 8;
}

static inline int
rva2_fixed_volume_adjustment(const Rva2Data &data)
{
	signed int voladj_fixed;
	voladj_fixed = (data.volume_adjustment[0] << 8) |
		data.volume_adjustment[1];
	voladj_fixed |= -(voladj_fixed & 0x8000);
	return voladj_fixed;
}

static inline float
rva2_float_volume_adjustment(const Rva2Data &data)
{
	/*
	 * "The volume adjustment is encoded as a fixed point decibel
	 * value, 16 bit signed integer representing (adjustment*512),
	 * giving +/- 64 dB with a precision of 0.001953125 dB."
	 */

	return float(rva2_fixed_volume_adjustment(data)) / float(512);
}

static inline bool
rva2_apply_data(ReplayGainInfo &rgi,
		const Rva2Data &data, const id3_latin1_t *id)
{
	if (data.type != Rva2Channel::MASTER_VOLUME)
		return false;

	float volume_adjustment = rva2_float_volume_adjustment(data);

	if (strcmp(reinterpret_cast<const char *>(id), "album") == 0)  {
		rgi.album.gain = volume_adjustment;
	} else if (strcmp(reinterpret_cast<const char *>(id), "track") == 0) {
		rgi.track.gain = volume_adjustment;
	} else {
		rgi.album.gain = volume_adjustment;
		rgi.track.gain = volume_adjustment;
	}

	return true;
}

static bool
rva2_apply_frame(ReplayGainInfo &replay_gain_info,
		 const struct id3_frame *frame)
{
	const id3_latin1_t *id = id3_field_getlatin1(id3_frame_field(frame, 0));
	id3_length_t length;
	const id3_byte_t *data =
		id3_field_getbinarydata(id3_frame_field(frame, 1), &length);

	if (id == nullptr || data == nullptr)
		return false;

	/*
	 * "The 'identification' string is used to identify the
	 * situation and/or device where this adjustment should apply.
	 * The following is then repeated for every channel
	 *
	 *   Type of channel         $xx
	 *   Volume adjustment       $xx xx
	 *   Bits representing peak  $xx
	 *   Peak volume             $xx (xx ...)"
	 */

	while (length >= 4) {
		const Rva2Data &d = *reinterpret_cast<const Rva2Data *>(data);
		unsigned int peak_bytes = rva2_peak_bytes(d);
		if (4 + peak_bytes > length)
			break;

		if (rva2_apply_data(replay_gain_info, d, id))
			return true;

		data   += 4 + peak_bytes;
		length -= 4 + peak_bytes;
	}

	return false;
}

bool
tag_rva2_parse(const struct id3_tag *tag,
	       ReplayGainInfo &replay_gain_info) noexcept
{
	bool found = false;

	/* Loop through all RVA2 frames as some programs (e.g. mp3gain) store
	   track and album gain in separate tags */
	const struct id3_frame *frame;
	for (unsigned i = 0;
	     (frame = id3_tag_findframe(tag, "RVA2", i)) != nullptr;
	     ++i)
		if (rva2_apply_frame(replay_gain_info, frame))
			found = true;

	return found;
}
