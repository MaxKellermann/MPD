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

#include "WaveEncoderPlugin.hxx"
#include "../EncoderAPI.hxx"
#include "util/ByteOrder.hxx"
#include "util/DynamicFifoBuffer.hxx"

#include <cassert>

#include <string.h>

static constexpr uint16_t WAVE_FORMAT_PCM = 1;

class WaveEncoder final : public Encoder {
	unsigned bits;

	DynamicFifoBuffer<uint8_t> buffer;

public:
	explicit WaveEncoder(AudioFormat &audio_format) noexcept;

	/* virtual methods from class Encoder */
	void Write(const void *data, size_t length) override;

	size_t Read(void *dest, size_t length) noexcept override {
		return buffer.Read(static_cast<uint8_t *>(dest), length);
	}
};

class PreparedWaveEncoder final : public PreparedEncoder {
	/* virtual methods from class PreparedEncoder */
	Encoder *Open(AudioFormat &audio_format) override {
		return new WaveEncoder(audio_format);
	}

	[[nodiscard]] const char *GetMimeType() const noexcept override {
		return "audio/wav";
	}
};

struct WaveHeader {
	uint32_t id_riff;
	uint32_t riff_size;
	uint32_t id_wave;
	uint32_t id_fmt;
	uint32_t fmt_size;
	uint16_t format;
	uint16_t channels;
	uint32_t freq;
	uint32_t byterate;
	uint16_t blocksize;
	uint16_t bits;
	uint32_t id_data;
	uint32_t data_size;
};

static void
fill_wave_header(WaveHeader *header, int channels, int bits,
		int freq, int block_size) noexcept
{
	int data_size = 0x0FFFFFFF;

	/* constants */
	header->id_riff = ToLE32(0x46464952);
	header->id_wave = ToLE32(0x45564157);
	header->id_fmt = ToLE32(0x20746d66);
	header->id_data = ToLE32(0x61746164);

	/* wave format */
	header->format = ToLE16(WAVE_FORMAT_PCM);
	header->channels = ToLE16(channels);
	header->bits = ToLE16(bits);
	header->freq = ToLE32(freq);
	header->blocksize = ToLE16(block_size);
	header->byterate = ToLE32(freq * block_size);

	/* chunk sizes (fake data length) */
	header->fmt_size = ToLE32(16);
	header->data_size = ToLE32(data_size);
	header->riff_size = ToLE32(4 + (8 + 16) + (8 + data_size));
}

static PreparedEncoder *
wave_encoder_init([[maybe_unused]] const ConfigBlock &block)
{
	return new PreparedWaveEncoder();
}

WaveEncoder::WaveEncoder(AudioFormat &audio_format) noexcept
	:Encoder(false),
	 buffer(8192)
{
	assert(audio_format.IsValid());

	switch (audio_format.format) {
	case SampleFormat::S8:
		bits = 8;
		break;

	case SampleFormat::S16:
		bits = 16;
		break;

	case SampleFormat::S24_P32:
		bits = 24;
		break;

	case SampleFormat::S32:
		bits = 32;
		break;

	default:
		audio_format.format = SampleFormat::S16;
		bits = 16;
		break;
	}

	auto range = buffer.Write();
	assert(range.size >= sizeof(WaveHeader));
	auto *header = reinterpret_cast<WaveHeader *>(range.data);

	/* create PCM wave header in initial buffer */
	fill_wave_header(header,
			 audio_format.channels,
			 bits,
			 audio_format.sample_rate,
			 (bits / 8) * audio_format.channels);

	buffer.Append(sizeof(*header));
}

static size_t
pcm16_to_wave(uint16_t *dst16, const uint16_t *src16, size_t length)
{
	size_t cnt = length >> 1;
	while (cnt > 0) {
		*dst16++ = ToLE16(*src16++);
		cnt--;
	}
	return length;
}

static size_t
pcm32_to_wave(uint32_t *dst32, const uint32_t *src32, size_t length) noexcept
{
	size_t cnt = length >> 2;
	while (cnt > 0){
		*dst32++ = ToLE32(*src32++);
		cnt--;
	}
	return length;
}

static size_t
pcm24_to_wave(uint8_t *dst8, const uint32_t *src32, size_t length) noexcept
{
	uint32_t value;
	uint8_t *dst_old = dst8;

	length = length >> 2;
	while (length > 0){
		value = *src32++;
		*dst8++ = (value) & 0xFF;
		*dst8++ = (value >> 8) & 0xFF;
		*dst8++ = (value >> 16) & 0xFF;
		length--;
	}
	//correct buffer length
	return (dst8 - dst_old);
}

void
WaveEncoder::Write(const void *src, size_t length)
{
	uint8_t *dst = buffer.Write(length);

	if (IsLittleEndian()) {
		switch (bits) {
		case 8:
		case 16:
		case 32:// optimized cases
			memcpy(dst, src, length);
			break;
		case 24:
			length = pcm24_to_wave(dst, static_cast<const uint32_t *>(src), length);
			break;
		}
	} else {
		switch (bits) {
		case 8:
			memcpy(dst, src, length);
			break;
		case 16:
			length = pcm16_to_wave(reinterpret_cast<uint16_t *>(dst),
					       static_cast<const uint16_t *>(src), length);
			break;
		case 24:
			length = pcm24_to_wave(dst, static_cast<const uint32_t *>(src), length);
			break;
		case 32:
			length = pcm32_to_wave(reinterpret_cast<uint32_t *>(dst),
					       static_cast<const uint32_t *>(src), length);
			break;
		}
	}

	buffer.Append(length);
}

const EncoderPlugin wave_encoder_plugin = {
	"wave",
	wave_encoder_init,
};
