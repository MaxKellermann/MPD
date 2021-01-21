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

#include "Interleave.hxx"

#include <string.h>

static void
GenericPcmInterleave(uint8_t *gcc_restrict dest,
		     ConstBuffer<const uint8_t *> src,
		     size_t n_frames, size_t sample_size) noexcept
{
	for (size_t frame = 0; frame < n_frames; ++frame) {
		for (size_t channel = 0; channel < src.size; ++channel) {
			memcpy(dest, src[channel] + frame * sample_size,
			       sample_size);
			dest += sample_size;
		}
	}
}

template<typename T>
static void
PcmInterleaveStereo(T *gcc_restrict dest,
		    const T *gcc_restrict src1,
		    const T *gcc_restrict src2,
		    size_t n_frames) noexcept
{
	for (size_t i = 0; i != n_frames; ++i) {
		*dest++ = *src1++;
		*dest++ = *src2++;
	}
}

template<typename T>
static void
PcmInterleaveT(T *gcc_restrict dest,
	       const ConstBuffer<const T *> src,
	       size_t n_frames) noexcept
{
	switch (src.size) {
	case 2:
		PcmInterleaveStereo(dest, src[0], src[1], n_frames);
		return;
	}

	for (const auto *s : src) {
		auto *d = dest++;

		for (const auto *const s_end = s + n_frames;
		     s != s_end; ++s, d += src.size)
			*d = *s;
	}
}

static void
PcmInterleave16(int16_t *gcc_restrict dest,
		const ConstBuffer<const int16_t *> src,
		size_t n_frames) noexcept
{
	PcmInterleaveT(dest, src, n_frames);
}

void
PcmInterleave32(int32_t *gcc_restrict dest,
		const ConstBuffer<const int32_t *> src,
		size_t n_frames) noexcept
{
	PcmInterleaveT(dest, src, n_frames);
}

void
PcmInterleave(void *gcc_restrict dest,
	      ConstBuffer<const void *> src,
	      size_t n_frames, size_t sample_size) noexcept
{
	switch (sample_size) {
	case 2:
		PcmInterleave16(static_cast<int16_t *>(dest),
				ConstBuffer<const int16_t *>(reinterpret_cast<const int16_t *const*>(src.data),
							     src.size),
				n_frames);
		break;

	case 4:
		PcmInterleave32(static_cast<int32_t *>(dest),
				ConstBuffer<const int32_t *>(reinterpret_cast<const int32_t *const*>(src.data),
							     src.size),
				n_frames);
		break;

	default:
		GenericPcmInterleave(static_cast<uint8_t *>(dest),
				     ConstBuffer<const uint8_t *>(reinterpret_cast<const uint8_t *const*>(src.data),
								  src.size),
				     n_frames, sample_size);
	}
}
