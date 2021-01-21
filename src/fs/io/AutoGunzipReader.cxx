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

#include "AutoGunzipReader.hxx"
#include "GunzipReader.hxx"

AutoGunzipReader::AutoGunzipReader(Reader &_next) noexcept
	:peek(_next) {}

AutoGunzipReader::~AutoGunzipReader() noexcept = default;

gcc_pure
static bool
IsGzip(const uint8_t data[4]) noexcept
{
	return data[0] == 0x1f && data[1] == 0x8b && data[2] == 0x08 &&
		(data[3] & 0xe0) == 0;
}

inline void
AutoGunzipReader::Detect()
{
	const auto *data = static_cast<const uint8_t *>(peek.Peek(4));
	if (data == nullptr) {
		next = &peek;
		return;
	}

	if (IsGzip(data))
		next = (gunzip = std::make_unique<GunzipReader>(peek)).get();
	else
		next = &peek;
}

size_t
AutoGunzipReader::Read(void *data, size_t size)
{
	if (next == nullptr)
		Detect();

	assert(next != nullptr);
	return next->Read(data, size);
}
