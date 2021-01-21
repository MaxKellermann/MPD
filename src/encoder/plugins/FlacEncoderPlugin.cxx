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

#include "FlacEncoderPlugin.hxx"
#include "../EncoderAPI.hxx"
#include "pcm/AudioFormat.hxx"
#include "pcm/Buffer.hxx"
#include "util/DynamicFifoBuffer.hxx"
#include "util/RuntimeError.hxx"

#include <FLAC/stream_encoder.h>

#if !defined(FLAC_API_VERSION_CURRENT) || FLAC_API_VERSION_CURRENT <= 7
#error libFLAC is too old
#endif

class FlacEncoder final : public Encoder {
	const AudioFormat audio_format;

	FLAC__StreamEncoder *const fse;

	PcmBuffer expand_buffer;

	/**
	 * This buffer will hold encoded data from libFLAC until it is
	 * picked up with flac_encoder_read().
	 */
	DynamicFifoBuffer<uint8_t> output_buffer;

public:
	FlacEncoder(AudioFormat _audio_format, FLAC__StreamEncoder *_fse);

	~FlacEncoder() noexcept override {
		FLAC__stream_encoder_delete(fse);
	}

	/* virtual methods from class Encoder */
	void End() override {
		(void) FLAC__stream_encoder_finish(fse);
	}

	void Flush() override {
		(void) FLAC__stream_encoder_finish(fse);
	}

	void Write(const void *data, size_t length) override;

	size_t Read(void *dest, size_t length) noexcept override {
		return output_buffer.Read(static_cast<uint8_t *>(dest), length);
	}

private:
	static FLAC__StreamEncoderWriteStatus WriteCallback(const FLAC__StreamEncoder *,
							    const FLAC__byte data[],
							    size_t bytes,
							    [[maybe_unused]] unsigned samples,
							    [[maybe_unused]] unsigned current_frame,
							    void *client_data) noexcept {
		auto &encoder = *static_cast<FlacEncoder *>(client_data);
		encoder.output_buffer.Append(static_cast<const uint8_t *>(data), bytes);
		return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
	}
};

class PreparedFlacEncoder final : public PreparedEncoder {
	const unsigned compression;

public:
	explicit PreparedFlacEncoder(const ConfigBlock &block);

	/* virtual methods from class PreparedEncoder */
	Encoder *Open(AudioFormat &audio_format) override;

	[[nodiscard]] const char *GetMimeType() const noexcept override {
		return  "audio/flac";
	}
};

PreparedFlacEncoder::PreparedFlacEncoder(const ConfigBlock &block)
	:compression(block.GetBlockValue("compression", 5U))
{
}

static PreparedEncoder *
flac_encoder_init(const ConfigBlock &block)
{
	return new PreparedFlacEncoder(block);
}

static void
flac_encoder_setup(FLAC__StreamEncoder *fse, unsigned compression,
		   const AudioFormat &audio_format, unsigned bits_per_sample)
{
	if (!FLAC__stream_encoder_set_compression_level(fse, compression))
		throw FormatRuntimeError("error setting flac compression to %d",
					 compression);

	if (!FLAC__stream_encoder_set_channels(fse, audio_format.channels))
		throw FormatRuntimeError("error setting flac channels num to %d",
					 audio_format.channels);

	if (!FLAC__stream_encoder_set_bits_per_sample(fse, bits_per_sample))
		throw FormatRuntimeError("error setting flac bit format to %d",
					 bits_per_sample);

	if (!FLAC__stream_encoder_set_sample_rate(fse,
						  audio_format.sample_rate))
		throw FormatRuntimeError("error setting flac sample rate to %d",
					 audio_format.sample_rate);
}

FlacEncoder::FlacEncoder(AudioFormat _audio_format, FLAC__StreamEncoder *_fse)
	:Encoder(false),
	 audio_format(_audio_format), fse(_fse),
	 output_buffer(8192)
{
	/* this immediately outputs data through callback */

	auto init_status =
		FLAC__stream_encoder_init_stream(fse,
						 WriteCallback,
						 nullptr, nullptr, nullptr,
						 this);

	if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		throw FormatRuntimeError("failed to initialize encoder: %s\n",
					 FLAC__StreamEncoderInitStatusString[init_status]);
}

Encoder *
PreparedFlacEncoder::Open(AudioFormat &audio_format)
{
	unsigned bits_per_sample;

	/* FIXME: flac should support 32bit as well */
	switch (audio_format.format) {
	case SampleFormat::S8:
		bits_per_sample = 8;
		break;

	case SampleFormat::S16:
		bits_per_sample = 16;
		break;

	case SampleFormat::S24_P32:
		bits_per_sample = 24;
		break;

	default:
		bits_per_sample = 24;
		audio_format.format = SampleFormat::S24_P32;
	}

	/* allocate the encoder */
	auto fse = FLAC__stream_encoder_new();
	if (fse == nullptr)
		throw std::runtime_error("FLAC__stream_encoder_new() failed");

	try {
		flac_encoder_setup(fse, compression,
				   audio_format, bits_per_sample);
	} catch (...) {
		FLAC__stream_encoder_delete(fse);
		throw;
	}

	return new FlacEncoder(audio_format, fse);
}

static inline void
pcm8_to_flac(int32_t *out, const int8_t *in, unsigned num_samples) noexcept
{
	while (num_samples > 0) {
		*out++ = *in++;
		--num_samples;
	}
}

static inline void
pcm16_to_flac(int32_t *out, const int16_t *in, unsigned num_samples) noexcept
{
	while (num_samples > 0) {
		*out++ = *in++;
		--num_samples;
	}
}

void
FlacEncoder::Write(const void *data, size_t length)
{
	void *exbuffer;
	const void *buffer = nullptr;

	/* format conversion */

	const unsigned num_frames = length / audio_format.GetFrameSize();
	const unsigned num_samples = num_frames * audio_format.channels;

	switch (audio_format.format) {
	case SampleFormat::S8:
		exbuffer = expand_buffer.Get(length * 4);
		pcm8_to_flac(static_cast<int32_t *>(exbuffer), static_cast<const int8_t *>(data),
			     num_samples);
		buffer = exbuffer;
		break;

	case SampleFormat::S16:
		exbuffer = expand_buffer.Get(length * 2);
		pcm16_to_flac(static_cast<int32_t *>(exbuffer), static_cast<const int16_t *>(data),
			      num_samples);
		buffer = exbuffer;
		break;

	case SampleFormat::S24_P32:
	case SampleFormat::S32:
		/* nothing need to be done; format is the same for
		   both mpd and libFLAC */
		buffer = data;
		break;

	default:
		gcc_unreachable();
	}

	/* feed samples to encoder */

	if (!FLAC__stream_encoder_process_interleaved(fse,
						      static_cast<const FLAC__int32 *>(buffer),
						      num_frames))
		throw std::runtime_error("flac encoder process failed");
}

const EncoderPlugin flac_encoder_plugin = {
	"flac",
	flac_encoder_init,
};

