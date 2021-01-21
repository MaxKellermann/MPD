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

#include "TwolameEncoderPlugin.hxx"
#include "../EncoderAPI.hxx"
#include "pcm/AudioFormat.hxx"
#include "util/NumberParser.hxx"
#include "util/RuntimeError.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <twolame.h>

#include <cassert>
#include <stdexcept>

#include <string.h>

class TwolameEncoder final : public Encoder {
	const AudioFormat audio_format;

	twolame_options *options;

	unsigned char output_buffer[32768];
	size_t output_buffer_length = 0;
	size_t output_buffer_position = 0;

	/**
	 * Call libtwolame's flush function when the output_buffer is
	 * empty?
	 */
	bool flush = false;

public:
	TwolameEncoder(const AudioFormat _audio_format,
		       twolame_options *_options)
		:Encoder(false),
		 audio_format(_audio_format), options(_options) {}
	~TwolameEncoder() noexcept override;

	/* virtual methods from class Encoder */

	void End() override {
		flush = true;
	}

	void Flush() override {
		flush = true;
	}

	void Write(const void *data, size_t length) override;
	size_t Read(void *dest, size_t length) noexcept override;
};

class PreparedTwolameEncoder final : public PreparedEncoder {
	float quality;
	int bitrate;

public:
	explicit PreparedTwolameEncoder(const ConfigBlock &block);

	/* virtual methods from class PreparedEncoder */
	Encoder *Open(AudioFormat &audio_format) override;

	[[nodiscard]] const char *GetMimeType() const noexcept override {
		return  "audio/mpeg";
	}
};

static constexpr Domain twolame_encoder_domain("twolame_encoder");

PreparedTwolameEncoder::PreparedTwolameEncoder(const ConfigBlock &block)
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
twolame_encoder_init(const ConfigBlock &block)
{
	FormatDebug(twolame_encoder_domain,
		    "libtwolame version %s", get_twolame_version());

	return new PreparedTwolameEncoder(block);
}

static void
twolame_encoder_setup(twolame_options *options, float quality, int bitrate,
		      const AudioFormat &audio_format)
{
	if (quality >= -1.0f) {
		/* a quality was configured (VBR) */

		if (0 != twolame_set_VBR(options, true))
			throw std::runtime_error("error setting twolame VBR mode");

		if (0 != twolame_set_VBR_q(options, quality))
			throw std::runtime_error("error setting twolame VBR quality");
	} else {
		/* a bit rate was configured */

		if (0 != twolame_set_brate(options, bitrate))
			throw std::runtime_error("error setting twolame bitrate");
	}

	if (0 != twolame_set_num_channels(options, audio_format.channels))
		throw std::runtime_error("error setting twolame num channels");

	if (0 != twolame_set_in_samplerate(options,
					   audio_format.sample_rate))
		throw std::runtime_error("error setting twolame sample rate");

	if (0 > twolame_init_params(options))
		throw std::runtime_error("error initializing twolame params");
}

Encoder *
PreparedTwolameEncoder::Open(AudioFormat &audio_format)
{
	audio_format.format = SampleFormat::S16;
	audio_format.channels = 2;

	auto options = twolame_init();
	if (options == nullptr)
		throw std::runtime_error("twolame_init() failed");

	try {
		twolame_encoder_setup(options, quality, bitrate,
				      audio_format);
	} catch (...) {
		twolame_close(&options);
		throw;
	}

	return new TwolameEncoder(audio_format, options);
}

TwolameEncoder::~TwolameEncoder() noexcept
{
	twolame_close(&options);
}

void
TwolameEncoder::Write(const void *data, size_t length)
{
	const auto *src = static_cast<const int16_t*>(data);

	assert(output_buffer_position == output_buffer_length);

	const unsigned num_frames = length / audio_format.GetFrameSize();

	int bytes_out = twolame_encode_buffer_interleaved(options,
							  src, num_frames,
							  output_buffer,
							  sizeof(output_buffer));
	if (bytes_out < 0)
		throw std::runtime_error("twolame encoder failed");

	output_buffer_length = size_t(bytes_out);
	output_buffer_position = 0;
}

size_t
TwolameEncoder::Read(void *dest, size_t length) noexcept
{
	assert(output_buffer_position <= output_buffer_length);

	if (output_buffer_position == output_buffer_length && flush) {
		int ret = twolame_encode_flush(options, output_buffer,
					       sizeof(output_buffer));
		if (ret > 0) {
			output_buffer_length = size_t(ret);
			output_buffer_position = 0;
		}

		flush = false;
	}


	const size_t remainning = output_buffer_length - output_buffer_position;
	if (length > remainning)
		length = remainning;

	memcpy(dest, output_buffer + output_buffer_position, length);

	output_buffer_position += length;

	return length;
}

const EncoderPlugin twolame_encoder_plugin = {
	"twolame",
	twolame_encoder_init,
};
