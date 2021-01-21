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

#include "FaadDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "../DecoderBuffer.hxx"
#include "input/InputStream.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "util/ScopeExit.hxx"
#include "util/ConstBuffer.hxx"
#include "util/Domain.hxx"
#include "util/Math.hxx"
#include "Log.hxx"

#include <cassert>
#include <cstring>

#include <neaacdec.h>

static const unsigned adts_sample_rates[] =
    { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
	16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

static constexpr Domain faad_decoder_domain("faad_decoder");

/**
 * Check whether the buffer head is an AAC frame, and return the frame
 * length.  Returns 0 if it is not a frame.
 */
static size_t
adts_check_frame(const unsigned char *data)
{
	/* check syncword */
	if (!((data[0] == 0xFF) && ((data[1] & 0xF6) == 0xF0)))
		return 0;

	return ((uint32_t(data[3]) & 0x3) << 11) |
		((uint32_t(data[4])) << 3) |
		(data[5] >> 5);
}

/**
 * Find the next AAC frame in the buffer.  Returns 0 if no frame is
 * found or if not enough data is available.
 */
static size_t
adts_find_frame(DecoderBuffer &buffer)
{
	while (true) {
		auto data = ConstBuffer<uint8_t>::FromVoid(buffer.Need(8));
		if (data.IsNull())
			/* failed */
			return 0;

		/* find the 0xff marker */
		auto p = static_cast<const uint8_t *>(std::memchr(data.data, 0xff, data.size));
		if (p == nullptr) {
			/* no marker - discard the buffer */
			buffer.Clear();
			continue;
		}

		if (p > data.data) {
			/* discard data before 0xff */
			buffer.Consume(p - data.data);
			continue;
		}

		/* is it a frame? */
		const size_t frame_length = adts_check_frame(data.data);
		if (frame_length == 0) {
			/* it's just some random 0xff byte; discard it
			   and continue searching */
			buffer.Consume(1);
			continue;
		}

		if (buffer.Need(frame_length).IsNull()) {
			/* not enough data; discard this frame to
			   prevent a possible buffer overflow */
			buffer.Clear();
			continue;
		}

		/* found a full frame! */
		return frame_length;
	}
}

static SignedSongTime
adts_song_duration(DecoderBuffer &buffer)
{
	const InputStream &is = buffer.GetStream();
	const bool estimate = !is.CheapSeeking();
	if (estimate && !is.KnownSize())
		return SignedSongTime::Negative();

	unsigned sample_rate = 0;

	/* Read all frames to ensure correct time and bitrate */
	unsigned frames = 0;
	for (;; frames++) {
		const unsigned frame_length = adts_find_frame(buffer);
		if (frame_length == 0)
			break;

		if (frames == 0) {
			auto data = ConstBuffer<uint8_t>::FromVoid(buffer.Read());
			assert(!data.empty());
			assert(frame_length <= data.size);

			sample_rate = adts_sample_rates[(data.data[2] & 0x3c) >> 2];
			if (sample_rate == 0)
				break;
		}

		buffer.Consume(frame_length);

		if (estimate && frames == 128) {
			/* if this is a remote file, don't slurp the
			   whole file just for checking the song
			   duration; instead, stop after some time and
			   extrapolate the song duration from what we
			   have until now */

			const auto offset = is.GetOffset()
				- buffer.GetAvailable();
			if (offset <= 0)
				return SignedSongTime::Negative();

			const auto file_size = is.GetSize();
			frames = (frames * file_size) / offset;
			break;
		}
	}

	if (sample_rate == 0)
		return SignedSongTime::Negative();

	return SignedSongTime::FromScale<uint64_t>(frames * uint64_t(1024),
						   sample_rate);
}

static SignedSongTime
faad_song_duration(DecoderBuffer &buffer, InputStream &is)
{
	auto data = ConstBuffer<uint8_t>::FromVoid(buffer.Need(5));
	if (data.IsNull())
		return SignedSongTime::Negative();

	size_t tagsize = 0;
	if (data.size >= 10 && !memcmp(data.data, "ID3", 3)) {
		/* skip the ID3 tag */

		tagsize = (data.data[6] << 21) | (data.data[7] << 14) |
		    (data.data[8] << 7) | (data.data[9] << 0);

		tagsize += 10;

		if (!buffer.Skip(tagsize))
			return SignedSongTime::Negative();

		data = ConstBuffer<uint8_t>::FromVoid(buffer.Need(5));
		if (data.IsNull())
			return SignedSongTime::Negative();
	}

	if (data.size >= 8 && adts_check_frame(data.data) > 0) {
		/* obtain the duration from the ADTS header */

		if (!is.IsSeekable())
			return SignedSongTime::Negative();

		auto song_length = adts_song_duration(buffer);

		try {
			is.LockSeek(tagsize);
		} catch (...) {
		}

		buffer.Clear();

		return song_length;
	} else if (data.size >= 5 && memcmp(data.data, "ADIF", 4) == 0) {
		/* obtain the duration from the ADIF header */

		if (!is.KnownSize())
			return SignedSongTime::Negative();

		size_t skip_size = (data.data[4] & 0x80) ? 9 : 0;

		if (8 + skip_size > data.size)
			/* not enough data yet; skip parsing this
			   header */
			return SignedSongTime::Negative();

		unsigned bit_rate = ((data.data[4 + skip_size] & 0x0F) << 19) |
			(data.data[5 + skip_size] << 11) |
			(data.data[6 + skip_size] << 3) |
			(data.data[7 + skip_size] & 0xE0);

		const auto size = is.GetSize();
		if (bit_rate == 0)
			return SignedSongTime::Negative();

		return SongTime::FromScale(size, bit_rate / 8);
	} else
		return SignedSongTime::Negative();
}

static NeAACDecHandle
faad_decoder_new()
{
	auto decoder = NeAACDecOpen();

	NeAACDecConfigurationPtr config =
		NeAACDecGetCurrentConfiguration(decoder);
	config->outputFormat = FAAD_FMT_16BIT;
	config->downMatrix = 1;
	config->dontUpSampleImplicitSBR = 0;
	NeAACDecSetConfiguration(decoder, config);

	return decoder;
}

/**
 * Wrapper for NeAACDecInit() which works around some API
 * inconsistencies in libfaad.
 *
 * Throws #std::runtime_error on error.
 */
static void
faad_decoder_init(NeAACDecHandle decoder, DecoderBuffer &buffer,
		  AudioFormat &audio_format)
{
	auto data = ConstBuffer<uint8_t>::FromVoid(buffer.Read());
	if (data.empty())
		throw std::runtime_error("Empty file");

	uint8_t channels;
	unsigned long sample_rate;
	long nbytes = NeAACDecInit(decoder,
				   /* deconst hack, libfaad requires this */
				   const_cast<unsigned char *>(data.data),
				   data.size,
				   &sample_rate, &channels);
	if (nbytes < 0)
		throw std::runtime_error("Not an AAC stream");

	buffer.Consume(nbytes);

	audio_format = CheckAudioFormat(sample_rate, SampleFormat::S16,
					channels);
}

/**
 * Wrapper for NeAACDecDecode() which works around some API
 * inconsistencies in libfaad.
 */
static const void *
faad_decoder_decode(NeAACDecHandle decoder, DecoderBuffer &buffer,
		    NeAACDecFrameInfo *frame_info)
{
	auto data = ConstBuffer<uint8_t>::FromVoid(buffer.Read());
	if (data.empty())
		return nullptr;

	return NeAACDecDecode(decoder, frame_info,
			      /* deconst hack, libfaad requires this */
			      const_cast<uint8_t *>(data.data),
			      data.size);
}

/**
 * Determine a song file's total playing time.
 *
 * The first return value specifies whether the file was recognized.
 * The second return value is the duration.
 */
static std::pair<bool, SignedSongTime>
faad_get_file_time(InputStream &is)
{
	DecoderBuffer buffer(nullptr, is,
			     FAAD_MIN_STREAMSIZE * MAX_CHANNELS);
	auto duration = faad_song_duration(buffer, is);
	bool recognized = !duration.IsNegative();

	if (!recognized) {
		NeAACDecHandle decoder = faad_decoder_new();
		AtScopeExit(decoder) { NeAACDecClose(decoder); };

		buffer.Fill();

		AudioFormat audio_format;
		try {
			faad_decoder_init(decoder, buffer, audio_format);
			recognized = true;
		} catch (...) {
		}
	}

	return std::make_pair(recognized, duration);
}

static void
faad_stream_decode(DecoderClient &client, InputStream &is,
		   DecoderBuffer &buffer, NeAACDecHandle decoder)
{
	const auto total_time = faad_song_duration(buffer, is);

	if (adts_find_frame(buffer) == 0)
		return;

	/* initialize it */

	AudioFormat audio_format;
	faad_decoder_init(decoder, buffer, audio_format);

	/* initialize the MPD core */

	client.Ready(audio_format, false, total_time);

	/* the decoder loop */

	DecoderCommand cmd;
	unsigned bit_rate = 0;
	do {
		/* find the next frame */

		const size_t frame_size = adts_find_frame(buffer);
		if (frame_size == 0)
			/* end of file */
			break;

		/* decode it */

		NeAACDecFrameInfo frame_info;
		const void *const decoded =
			faad_decoder_decode(decoder, buffer, &frame_info);

		if (frame_info.error > 0) {
			FormatWarning(faad_decoder_domain,
				      "error decoding AAC stream: %s",
				      NeAACDecGetErrorMessage(frame_info.error));
			break;
		}

		if (frame_info.channels != audio_format.channels) {
			FormatNotice(faad_decoder_domain,
				     "channel count changed from %u to %u",
				     audio_format.channels, frame_info.channels);
			break;
		}

		if (frame_info.samplerate != audio_format.sample_rate) {
			FormatNotice(faad_decoder_domain,
				     "sample rate changed from %u to %lu",
				     audio_format.sample_rate,
				     frame_info.samplerate);
			break;
		}

		buffer.Consume(frame_info.bytesconsumed);

		/* update bit rate and position */

		if (frame_info.samples > 0) {
			bit_rate = lround(frame_info.bytesconsumed * 8.0 *
					  frame_info.channels * audio_format.sample_rate /
					  frame_info.samples / 1000);
		}

		/* send PCM samples to MPD */

		cmd = client.SubmitData(is, decoded,
					size_t(frame_info.samples) * 2,
					bit_rate);
	} while (cmd != DecoderCommand::STOP);
}

static void
faad_stream_decode(DecoderClient &client, InputStream &is)
{
	DecoderBuffer buffer(&client, is,
			     FAAD_MIN_STREAMSIZE * MAX_CHANNELS);

	/* create the libfaad decoder */

	auto decoder = faad_decoder_new();
	AtScopeExit(decoder) { NeAACDecClose(decoder); };

	faad_stream_decode(client, is, buffer, decoder);
}

static bool
faad_scan_stream(InputStream &is, TagHandler &handler)
{
	auto result = faad_get_file_time(is);
	if (!result.first)
		return false;

	if (!result.second.IsNegative())
		handler.OnDuration(SongTime(result.second));
	return true;
}

static const char *const faad_suffixes[] = { "aac", nullptr };
static const char *const faad_mime_types[] = {
	"audio/aac", "audio/aacp", nullptr
};

constexpr DecoderPlugin faad_decoder_plugin =
	DecoderPlugin("faad", faad_stream_decode, faad_scan_stream)
	.WithSuffixes(faad_suffixes)
	.WithMimeTypes(faad_mime_types);
