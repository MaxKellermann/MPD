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

#include "Id3Picture.hxx"
#include "Handler.hxx"
#include "util/ByteOrder.hxx"
#include "util/ConstBuffer.hxx"
#include "util/StringView.hxx"

#include <cstdint>
#include <string>

static StringView
ReadString(ConstBuffer<uint8_t> &src) noexcept
{
	if (src.size < 4)
		return nullptr;

	const size_t length = FromBE32(*reinterpret_cast<const uint32_t *>(src.data));
	src.skip_front(4);

	if (src.size < length)
		return nullptr;

	StringView result(reinterpret_cast<const char *>(src.data), length);
	src.skip_front(length);
	return result;
}

void
ScanId3Apic(ConstBuffer<void> _buffer, TagHandler &handler) noexcept
{
	auto buffer = ConstBuffer<uint8_t>::FromVoid(_buffer);
	if (buffer.size < 4)
		return;

	buffer.skip_front(4); /* picture type */

	const auto mime_type = ReadString(buffer);
	if (mime_type.IsNull())
		return;

	/* description */
	if (ReadString(buffer).IsNull())
		return;

	if (buffer.size < 20)
		return;

	buffer.skip_front(16);

	const size_t image_size = FromBE32(*reinterpret_cast<const uint32_t *>(buffer.data));
	buffer.skip_front(4);

	if (buffer.size < image_size)
		return;

	ConstBuffer<void> image(buffer.data, image_size);

	// TODO: don't copy MIME type, pass StringView to TagHandler::OnPicture()
	handler.OnPicture(std::string(mime_type.data, mime_type.size).c_str(),
			  image);
}
