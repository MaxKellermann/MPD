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
#include "Buffer.hxx"
#include "Error.hxx"
#include "pcm/Interleave.hxx"
#include "util/ConstBuffer.hxx"

extern "C" {
#include <libavutil/frame.h>
}

#include <cassert>
#include <new> // for std::bad_alloc

namespace Ffmpeg {

ConstBuffer<void>
InterleaveFrame(const AVFrame &frame, FfmpegBuffer &buffer)
{
	assert(frame.nb_samples > 0);

	const AVSampleFormat format = AVSampleFormat(frame.format);
	const unsigned channels = frame.channels;
	const std::size_t n_frames = frame.nb_samples;

	int plane_size;
	const int data_size =
		av_samples_get_buffer_size(&plane_size, channels,
					   n_frames, format, 1);
	assert(data_size != 0);
	if (data_size < 0)
		throw MakeFfmpegError(data_size);

	void *output_buffer;
	if (av_sample_fmt_is_planar(format) && channels > 1) {
		output_buffer = buffer.GetT<uint8_t>(data_size);
		if (output_buffer == nullptr)
			/* Not enough memory - shouldn't happen */
			throw std::bad_alloc();

		PcmInterleave(output_buffer,
			      ConstBuffer<const void *>(reinterpret_cast<const void *const*>(frame.extended_data),
							channels),
			      n_frames,
			      av_get_bytes_per_sample(format));
	} else {
		output_buffer = frame.extended_data[0];
	}

	return { output_buffer, size_t(data_size) };
}

} // namespace Ffmpeg
