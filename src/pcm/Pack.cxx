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

#include "Pack.hxx"
#include "util/ByteOrder.hxx"

static void
pack_sample(uint8_t *dest, const int32_t *src0) noexcept
{
	const auto *src = reinterpret_cast<const uint8_t *>(src0);

	if (IsBigEndian())
		++src;

	*dest++ = *src++;
	*dest++ = *src++;
	*dest++ = *src++;
}

void
pcm_pack_24(uint8_t *dest, const int32_t *src, const int32_t *src_end) noexcept
{
	/* duplicate loop to help the compiler's optimizer (constant
	   parameter to the pack_sample() inline function) */

	while (src < src_end) {
		pack_sample(dest, src++);
		dest += 3;
	}
}

/**
 * Construct a signed 24 bit integer from three bytes into a int32_t.
 */
static constexpr int32_t
ConstructS24(uint8_t low, uint8_t mid, uint8_t high) noexcept
{
	return int32_t(low) | (int32_t(mid) << 8) | (int32_t(high) << 16) |
		/* extend the sign bit */
		(high & 0x80 ? ~int32_t(0xffffff) : 0);
}

/**
 * Read a packed signed little-endian 24 bit integer.
 */
gcc_pure
static int32_t
ReadS24LE(const uint8_t *src) noexcept
{
	return ConstructS24(src[0], src[1], src[2]);
}

/**
 * Read a packed signed big-endian 24 bit integer.
 */
gcc_pure
static int32_t
ReadS24BE(const uint8_t *src) noexcept
{
	return ConstructS24(src[2], src[1], src[0]);
}

/**
 * Read a packed signed native-endian 24 bit integer.
 */
gcc_pure
static int32_t
ReadS24(const uint8_t *src) noexcept
{
	return IsBigEndian() ? ReadS24BE(src) : ReadS24LE(src);
}

void
pcm_unpack_24(int32_t *dest,
	      const uint8_t *src, const uint8_t *src_end) noexcept
{
	while (src < src_end) {
		*dest++ = ReadS24(src);
		src += 3;
	}
}

void
pcm_unpack_24be(int32_t *dest,
		const uint8_t *src, const uint8_t *src_end) noexcept
{
	while (src < src_end) {
		*dest++ = ReadS24BE(src);
		src += 3;
	}
}
