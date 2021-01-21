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

#include "config.h"

#include "PcmDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "pcm/Pack.hxx"
#include "input/InputStream.hxx"
#include "util/ByteOrder.hxx"
#include "util/Domain.hxx"
#include "util/ByteReverse.hxx"
#include "util/StaticFifoBuffer.hxx"
#include "util/NumberParser.hxx"
#include "util/MimeType.hxx"
#include "Log.hxx"

#ifdef ENABLE_ALSA
#include "pcm/AudioParser.hxx"
#endif

#include <exception>

#include <string.h>

static constexpr Domain pcm_decoder_domain("pcm_decoder");

template<typename B>
static bool
FillBuffer(DecoderClient &client, InputStream &is, B &buffer)
{
	buffer.Shift();
	auto w = buffer.Write();
	if (w.empty())
		return true;

	size_t nbytes = decoder_read(client, is, w.data, w.size);
	if (nbytes == 0 && is.LockIsEOF())
		return false;

	buffer.Append(nbytes);
	return true;
}

static void
pcm_stream_decode(DecoderClient &client, InputStream &is)
{
	AudioFormat audio_format = {
		44100,
		SampleFormat::S16,
		2,
	};

	const char *const mime = is.GetMimeType();

	const bool l16 = mime != nullptr &&
		GetMimeTypeBase(mime) == "audio/L16";
	const bool l24 = mime != nullptr &&
		GetMimeTypeBase(mime) == "audio/L24";
	const bool is_float = mime != nullptr &&
		GetMimeTypeBase(mime) == "audio/x-mpd-float";
	if (l16 || l24 || is_float) {
		audio_format.sample_rate = 0;
		audio_format.channels = 1;
	}

	if (l24)
		audio_format.format = SampleFormat::S24_P32;

	const bool reverse_endian = (l16 && IsLittleEndian()) ||
		(mime != nullptr &&
		 strcmp(mime, "audio/x-mpd-cdda-pcm-reverse") == 0);

	if (is_float)
		audio_format.format = SampleFormat::FLOAT;

	if (mime != nullptr) {
		const auto mime_parameters = ParseMimeTypeParameters(mime);

		/* MIME type parameters according to RFC 2586 */
		auto i = mime_parameters.find("rate");
		if (i != mime_parameters.end()) {
			const char *s = i->second.c_str();
			char *endptr;
			unsigned value = ParseUnsigned(s, &endptr);
			if (endptr == s || *endptr != 0) {
				FormatWarning(pcm_decoder_domain,
					      "Failed to parse sample rate: %s",
					      s);
				return;
			}

			try {
				CheckSampleRate(value);
			} catch (...) {
				LogError(std::current_exception());
				return;
			}

			audio_format.sample_rate = value;
		}

		i = mime_parameters.find("channels");
		if (i != mime_parameters.end()) {
			const char *s = i->second.c_str();
			char *endptr;
			unsigned value = ParseUnsigned(s, &endptr);
			if (endptr == s || *endptr != 0) {
				FormatWarning(pcm_decoder_domain,
					      "Failed to parse sample rate: %s",
					      s);
				return;
			}

			try {
				CheckChannelCount(value);
			} catch (...) {
				LogError(std::current_exception());
				return;
			}

			audio_format.channels = value;
		}

#ifdef ENABLE_ALSA
		if (GetMimeTypeBase(mime) == "audio/x-mpd-alsa-pcm") {
			i = mime_parameters.find("format");
			if (i != mime_parameters.end()) {
				const char *s = i->second.c_str();
				audio_format = ParseAudioFormat(s, false);
				if (!audio_format.IsFullyDefined()) {
					FormatWarning(pcm_decoder_domain,
							  "Invalid audio format specification: %s",
							  mime);
					return;
				}
			}
		}
#endif
	}

	if (audio_format.sample_rate == 0) {
		FormatWarning(pcm_decoder_domain,
			      "Missing 'rate' parameter: %s",
			      mime);
		return;
	}

	const auto out_frame_size = audio_format.GetFrameSize();
	const auto in_frame_size = out_frame_size;

	const auto total_time = is.KnownSize()
		? SignedSongTime::FromScale<uint64_t>(is.GetSize() / in_frame_size,
						      audio_format.sample_rate)
		: SignedSongTime::Negative();

	client.Ready(audio_format, is.IsSeekable(), total_time);

	StaticFifoBuffer<uint8_t, 4096> buffer;

	/* a buffer for pcm_unpack_24be() large enough to hold the
	   results for a full source buffer */
	int32_t unpack_buffer[buffer.GetCapacity() / 3];

	DecoderCommand cmd;
	do {
		if (!FillBuffer(client, is, buffer))
			break;

		auto r = buffer.Read();
		/* round down to the nearest frame size, because we
		   must not pass partial frames to
		   DecoderClient::SubmitData() */
		r.size -= r.size % in_frame_size;
		buffer.Consume(r.size);

		if (reverse_endian)
			/* make sure we deliver samples in host byte order */
			reverse_bytes_16(reinterpret_cast<uint16_t *>(r.data),
					 reinterpret_cast<uint16_t *>(r.data),
					 reinterpret_cast<uint16_t *>(r.data + r.size));
		else if (l24) {
			/* convert big-endian packed 24 bit
			   (audio/L24) to native-endian 24 bit (in 32
			   bit integers) */
			pcm_unpack_24be(unpack_buffer, r.begin(), r.end());
			r.data = reinterpret_cast<uint8_t *>(&unpack_buffer[0]);
			r.size = (r.size / 3) * 4;
		}

		cmd = !r.empty()
			? client.SubmitData(is, r.data, r.size, 0)
			: client.GetCommand();
		if (cmd == DecoderCommand::SEEK) {
			uint64_t frame = client.GetSeekFrame();
			offset_type offset = frame * in_frame_size;

			try {
				is.LockSeek(offset);
				buffer.Clear();
				client.CommandFinished();
			} catch (...) {
				LogError(std::current_exception());
				client.SeekError();
			}

			cmd = DecoderCommand::NONE;
		}
	} while (cmd == DecoderCommand::NONE);
}

static const char *const pcm_mime_types[] = {
	/* RFC 2586 */
	"audio/L16",

	/* RFC 3190 */
	"audio/L24",

	/* MPD-specific: float32 native-endian */
	"audio/x-mpd-float",

	/* for streams obtained by the cdio_paranoia input plugin */
	"audio/x-mpd-cdda-pcm",

	/* same as above, but with reverse byte order */
	"audio/x-mpd-cdda-pcm-reverse",

#ifdef ENABLE_ALSA
	/* for streams obtained by the alsa input plugin */
	"audio/x-mpd-alsa-pcm",
#endif

	nullptr
};

constexpr DecoderPlugin pcm_decoder_plugin =
	DecoderPlugin("pcm", pcm_stream_decode, nullptr)
	.WithMimeTypes(pcm_mime_types);
