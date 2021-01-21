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

#include "Export.hxx"
#include "Order.hxx"
#include "Pack.hxx"
#include "Silence.hxx"
#include "util/ByteReverse.hxx"
#include "util/ConstBuffer.hxx"
#include "util/WritableBuffer.hxx"

#include <cassert>

#include <string.h>

void
PcmExport::Open(SampleFormat sample_format, unsigned _channels,
		Params params) noexcept
{
	assert(audio_valid_sample_format(sample_format));

	src_sample_format = sample_format;
	channels = _channels;
	alsa_channel_order = params.alsa_channel_order;

#ifdef ENABLE_DSD
	assert(params.dsd_mode != DsdMode::DOP ||
	       audio_valid_channel_count(_channels));

	dsd_mode = sample_format == SampleFormat::DSD
		? params.dsd_mode
		: DsdMode::NONE;

	switch (dsd_mode) {
	case DsdMode::NONE:
		break;

	case DsdMode::U16:
		dsd16_converter.Open(_channels);

		/* after the conversion to DSD_U16, the DSD samples
		   are stuffed inside fake 16 bit samples */
		sample_format = SampleFormat::S16;
		break;

	case DsdMode::U32:
		dsd32_converter.Open(_channels);

		/* after the conversion to DSD_U32, the DSD samples
		   are stuffed inside fake 32 bit samples */
		sample_format = SampleFormat::S32;
		break;

	case DsdMode::DOP:
		dop_converter.Open(_channels);

		/* after the conversion to DoP, the DSD
		   samples are stuffed inside fake 24 bit samples */
		sample_format = SampleFormat::S24_P32;
		break;
	}
#endif

	shift8 = params.shift8 && sample_format == SampleFormat::S24_P32;
	pack24 = params.pack24 && sample_format == SampleFormat::S24_P32;

	assert(!shift8 || !pack24);

	reverse_endian = 0;
	if (params.reverse_endian) {
		size_t sample_size = pack24
			? 3
			: sample_format_size(sample_format);
		assert(sample_size <= 0xff);

		if (sample_size > 1)
			reverse_endian = sample_size;
	}

	/* prepare a moment of silence for GetSilence() */
	char buffer[sizeof(silence_buffer)];
	const size_t buffer_size = GetInputBlockSize();
	assert(buffer_size < sizeof(buffer));
	PcmSilence({buffer, buffer_size}, src_sample_format);
	auto s = Export({buffer, buffer_size});
	assert(s.size < sizeof(silence_buffer));
	silence_size = s.size;
	memcpy(silence_buffer, s.data, s.size);
}

void
PcmExport::Reset() noexcept
{
#ifdef ENABLE_DSD
	switch (dsd_mode) {
	case DsdMode::NONE:
		break;

	case DsdMode::U16:
		dsd16_converter.Reset();
		break;

	case DsdMode::U32:
		dsd32_converter.Reset();
		break;

	case DsdMode::DOP:
		dop_converter.Reset();
		break;
	}
#endif
}

size_t
PcmExport::GetOutputFrameSize() const noexcept
{
	if (pack24)
		/* packed 24 bit samples (3 bytes per sample) */
		return channels * 3;

#ifdef ENABLE_DSD
	switch (dsd_mode) {
	case DsdMode::NONE:
		break;

	case DsdMode::U16:
		return channels * 2;

	case DsdMode::U32:
		return channels * 4;

	case DsdMode::DOP:
		/* the DSD-over-USB draft says that DSD 1-bit samples
		   are enclosed within 24 bit samples, and MPD's
		   representation of 24 bit is padded to 32 bit (4
		   bytes per sample) */
		return channels * 4;
	}
#endif

	return GetInputFrameSize();
}

size_t
PcmExport::GetInputBlockSize() const noexcept
{
#ifdef ENABLE_DSD
	switch (dsd_mode) {
	case DsdMode::NONE:
		break;

	case DsdMode::U16:
		return dsd16_converter.GetInputBlockSize();

	case DsdMode::U32:
		return dsd32_converter.GetInputBlockSize();

	case DsdMode::DOP:
		return dop_converter.GetInputBlockSize();
	}
#endif

	return GetInputFrameSize();
}

size_t
PcmExport::GetOutputBlockSize() const noexcept
{
#ifdef ENABLE_DSD
	switch (dsd_mode) {
	case DsdMode::NONE:
		break;

	case DsdMode::U16:
		return dsd16_converter.GetOutputBlockSize();

	case DsdMode::U32:
		return dsd32_converter.GetOutputBlockSize();

	case DsdMode::DOP:
		return dop_converter.GetOutputBlockSize();
	}
#endif

	return GetOutputFrameSize();
}

ConstBuffer<void>
PcmExport::GetSilence() const noexcept
{
	return {silence_buffer, silence_size};
}

unsigned
PcmExport::Params::CalcOutputSampleRate(unsigned sample_rate) const noexcept
{
#ifdef ENABLE_DSD
	switch (dsd_mode) {
	case DsdMode::NONE:
		break;

	case DsdMode::U16:
		/* DSD_U16 combines two 8-bit "samples" in one 16-bit
		   "sample" */
		sample_rate /= 2;
		break;

	case DsdMode::U32:
		/* DSD_U32 combines four 8-bit "samples" in one 32-bit
		   "sample" */
		sample_rate /= 4;
		break;

	case DsdMode::DOP:
		/* DoP packs two 8-bit "samples" in one 24-bit
		   "sample" */
		sample_rate /= 2;
		break;
	}
#endif

	return sample_rate;
}

unsigned
PcmExport::Params::CalcInputSampleRate(unsigned sample_rate) const noexcept
{
#ifdef ENABLE_DSD
	switch (dsd_mode) {
	case DsdMode::NONE:
		break;

	case DsdMode::U16:
		sample_rate *= 2;
		break;

	case DsdMode::U32:
		sample_rate *= 4;
		break;

	case DsdMode::DOP:
		sample_rate *= 2;
		break;
	}
#endif

	return sample_rate;
}

ConstBuffer<void>
PcmExport::Export(ConstBuffer<void> data) noexcept
{
	if (alsa_channel_order)
		data = ToAlsaChannelOrder(order_buffer, data,
					  src_sample_format, channels);

#ifdef ENABLE_DSD
	switch (dsd_mode) {
	case DsdMode::NONE:
		break;

	case DsdMode::U16:
		data = dsd16_converter.Convert(ConstBuffer<uint8_t>::FromVoid(data))
			.ToVoid();
		break;

	case DsdMode::U32:
		data = dsd32_converter.Convert(ConstBuffer<uint8_t>::FromVoid(data))
			.ToVoid();
		break;

	case DsdMode::DOP:
		data = dop_converter.Convert(ConstBuffer<uint8_t>::FromVoid(data))
			.ToVoid();
		break;
	}
#endif

	if (pack24) {
		const auto src = ConstBuffer<int32_t>::FromVoid(data);
		const size_t num_samples = src.size;
		const size_t dest_size = num_samples * 3;
		auto *dest = static_cast<uint8_t *>(pack_buffer.Get(dest_size));
		assert(dest != nullptr);

		pcm_pack_24(dest, src.begin(), src.end());

		data.data = dest;
		data.size = dest_size;
	} else if (shift8) {
		const auto src = ConstBuffer<int32_t>::FromVoid(data);

		auto *dest = static_cast<uint32_t *>(pack_buffer.Get(data.size));
		data.data = dest;

		for (auto i : src)
			*dest++ = i << 8;
	}

	if (reverse_endian > 0) {
		assert(reverse_endian >= 2);

		const auto src = ConstBuffer<uint8_t>::FromVoid(data);

		auto *dest = static_cast<uint8_t *>(reverse_buffer.Get(data.size));
		assert(dest != nullptr);
		data.data = dest;

		reverse_bytes(dest, src.begin(), src.end(), reverse_endian);
	}

	return data;
}

size_t
PcmExport::CalcInputSize(size_t size) const noexcept
{
	if (pack24)
		/* 32 bit to 24 bit conversion (4 to 3 bytes) */
		size = (size / 3) * 4;

#ifdef ENABLE_DSD
	switch (dsd_mode) {
	case DsdMode::NONE:
	case DsdMode::U16:
	case DsdMode::U32:
		break;

	case DsdMode::DOP:
		/* DoP doubles the transport size */
		size /= 2;
		break;
	}
#endif

	return size;
}
