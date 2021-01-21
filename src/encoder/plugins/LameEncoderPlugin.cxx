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

#include "LameEncoderPlugin.hxx"
#include "../EncoderAPI.hxx"
#include "pcm/AudioFormat.hxx"
#include "util/NumberParser.hxx"
#include "util/ReusableArray.hxx"
#include "util/RuntimeError.hxx"

#include <lame/lame.h>

#include <cassert>
#include <stdexcept>

#include <string.h>

class LameEncoder final : public Encoder {
	const AudioFormat audio_format;

	lame_global_flags *const gfp;

	ReusableArray<unsigned char, 32768> output_buffer;
	unsigned char *output_begin = nullptr, *output_end = nullptr;

public:
	LameEncoder(const AudioFormat _audio_format,
		    lame_global_flags *_gfp) noexcept
		:Encoder(false),
		 audio_format(_audio_format), gfp(_gfp) {}

	~LameEncoder() noexcept override;

	/* virtual methods from class Encoder */
	void Write(const void *data, size_t length) override;
	size_t Read(void *dest, size_t length) noexcept override;
};

class PreparedLameEncoder final : public PreparedEncoder {
	float quality;
	int bitrate;

public:
	explicit PreparedLameEncoder(const ConfigBlock &block);

	/* virtual methods from class PreparedEncoder */
	Encoder *Open(AudioFormat &audio_format) override;

	[[nodiscard]] const char *GetMimeType() const noexcept override {
		return "audio/mpeg";
	}
};

PreparedLameEncoder::PreparedLameEncoder(const ConfigBlock &block)
{
	const char *value;
	char *endptr;

	value = block.GetBlockValue("quality");
	if (value != nullptr) {
		/* a quality was configured (VBR) */

		quality = float(ParseDouble(value, &endptr));

		if (*endptr != '\0' || quality < -1.0f || quality > 10.0f)
			throw FormatRuntimeError("quality \"%s\" is not a number in the "
						 "range -1 to 10",
						 value);

		if (block.GetBlockValue("bitrate") != nullptr)
			throw std::runtime_error("quality and bitrate are both defined");
	} else {
		/* a bit rate was configured */

		value = block.GetBlockValue("bitrate");
		if (value == nullptr)
			throw std::runtime_error("neither bitrate nor quality defined");

		quality = -2.0;
		bitrate = ParseInt(value, &endptr);

		if (*endptr != '\0' || bitrate <= 0)
			throw std::runtime_error("bitrate should be a positive integer");
	}
}

static PreparedEncoder *
lame_encoder_init(const ConfigBlock &block)
{
	return new PreparedLameEncoder(block);
}

static void
lame_encoder_setup(lame_global_flags *gfp, float quality, int bitrate,
		   const AudioFormat &audio_format)
{
	if (quality >= -1.0f) {
		/* a quality was configured (VBR) */

		if (0 != lame_set_VBR(gfp, vbr_rh))
			throw std::runtime_error("error setting lame VBR mode");

		if (0 != lame_set_VBR_q(gfp, int(quality)))
			throw std::runtime_error("error setting lame VBR quality");
	} else {
		/* a bit rate was configured */

		if (0 != lame_set_brate(gfp, bitrate))
			throw std::runtime_error("error setting lame bitrate");
	}

	if (0 != lame_set_num_channels(gfp, audio_format.channels))
		throw std::runtime_error("error setting lame num channels");

	if (0 != lame_set_in_samplerate(gfp, audio_format.sample_rate))
		throw std::runtime_error("error setting lame sample rate");

	if (0 != lame_set_out_samplerate(gfp, audio_format.sample_rate))
		throw std::runtime_error("error setting lame out sample rate");

	if (0 > lame_init_params(gfp))
		throw std::runtime_error("error initializing lame params");
}

Encoder *
PreparedLameEncoder::Open(AudioFormat &audio_format)
{
	audio_format.format = SampleFormat::S16;
	audio_format.channels = 2;

	auto gfp = lame_init();
	if (gfp == nullptr)
		throw std::runtime_error("lame_init() failed");

	try {
		lame_encoder_setup(gfp, quality, bitrate, audio_format);
	} catch (...) {
		lame_close(gfp);
		throw;
	}

	return new LameEncoder(audio_format, gfp);
}

LameEncoder::~LameEncoder() noexcept
{
	lame_close(gfp);
}

void
LameEncoder::Write(const void *data, size_t length)
{
	const auto *src = static_cast<const int16_t*>(data);

	assert(output_begin == output_end);

	const unsigned num_frames = length / audio_format.GetFrameSize();
	const unsigned num_samples = length / audio_format.GetSampleSize();

	/* worst-case formula according to LAME documentation */
	const size_t output_buffer_size = 5 * num_samples / 4 + 7200;
	const auto dest = output_buffer.Get(output_buffer_size);

	/* this is for only 16-bit audio */

	int bytes_out = lame_encode_buffer_interleaved(gfp,
						       const_cast<short *>(src),
						       num_frames,
						       dest, output_buffer_size);

	if (bytes_out < 0)
		throw std::runtime_error("lame encoder failed");

	output_begin = dest;
	output_end = dest + bytes_out;
}

size_t
LameEncoder::Read(void *dest, size_t length) noexcept
{
	const auto begin = output_begin;
	assert(begin <= output_end);
	const size_t remainning = output_end - begin;
	if (length > remainning)
		length = remainning;

	memcpy(dest, begin, length);

	output_begin = begin + length;
	return length;
}

const EncoderPlugin lame_encoder_plugin = {
	"lame",
	lame_encoder_init,
};
