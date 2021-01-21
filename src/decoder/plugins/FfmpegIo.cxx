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

/* necessary because libavutil/common.h uses UINT64_C */
#define __STDC_CONSTANT_MACROS

#include "FfmpegIo.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"

AvioStream::~AvioStream()
{
	if (io != nullptr) {
		av_free(io->buffer);
		av_free(io);
	}
}

inline int
AvioStream::Read(void *dest, int size)
{
	return decoder_read(client, input, dest, size);
}

inline int64_t
AvioStream::Seek(int64_t pos, int whence)
{
	switch (whence) {
	case SEEK_SET:
		break;

	case SEEK_CUR:
		pos += input.GetOffset();
		break;

	case SEEK_END:
		if (!input.KnownSize())
			return -1;

		pos += input.GetSize();
		break;

	case AVSEEK_SIZE:
		if (!input.KnownSize())
			return -1;

		return input.GetSize();

	default:
		return -1;
	}

	try {
		input.LockSeek(pos);
		return input.GetOffset();
	} catch (...) {
		return -1;
	}
}

int
AvioStream::_Read(void *opaque, uint8_t *buf, int size)
{
	AvioStream &stream = *static_cast<AvioStream *>(opaque);

	return stream.Read(buf, size);
}

int64_t
AvioStream::_Seek(void *opaque, int64_t pos, int whence)
{
	AvioStream &stream = *static_cast<AvioStream *>(opaque);

	return stream.Seek(pos, whence);
}

bool
AvioStream::Open()
{
	constexpr size_t BUFFER_SIZE = 8192;
	auto buffer = static_cast<unsigned char *>(av_malloc(BUFFER_SIZE));
	if (buffer == nullptr)
		return false;

	io = avio_alloc_context(buffer, BUFFER_SIZE,
				false, this,
				_Read, nullptr,
				input.IsSeekable() ? _Seek : nullptr);
	/* If avio_alloc_context() fails, who frees the buffer?  The
	   libavformat API documentation does not specify this, it
	   only says that AVIOContext.buffer must be freed in the end,
	   however no AVIOContext exists in that failure code path. */
	return io != nullptr;
}
