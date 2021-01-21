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

#include "OpusDecoderPlugin.h"
#include "OggDecoder.hxx"
#include "OpusDomain.hxx"
#include "OpusHead.hxx"
#include "OpusTags.hxx"
#include "lib/xiph/OggPacket.hxx"
#include "lib/xiph/OggFind.hxx"
#include "../DecoderAPI.hxx"
#include "decoder/Reader.hxx"
#include "input/Reader.hxx"
#include "OggCodec.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "input/InputStream.hxx"
#include "util/RuntimeError.hxx"
#include "Log.hxx"

#include <opus.h>
#include <ogg/ogg.h>

#include <string.h>

namespace {

constexpr opus_int32 opus_sample_rate = 48000;

/**
 * Allocate an output buffer for 16 bit PCM samples big enough to hold
 * a quarter second, larger than 120ms required by libopus.
 */
constexpr unsigned opus_output_buffer_frames = opus_sample_rate / 4;

gcc_pure
bool
IsOpusHead(const ogg_packet &packet) noexcept
{
	return packet.bytes >= 8 && memcmp(packet.packet, "OpusHead", 8) == 0;
}

gcc_pure
bool
IsOpusTags(const ogg_packet &packet) noexcept
{
	return packet.bytes >= 8 && memcmp(packet.packet, "OpusTags", 8) == 0;
}

/**
 * Convert an EBU R128 value to ReplayGain.
 */
constexpr float
EbuR128ToReplayGain(float ebu_r128) noexcept
{
	/* add 5dB to compensate for the different reference levels
	   between ReplayGain (89dB) and EBU R128 (-23 LUFS) */
	return ebu_r128 + 5;
}

bool
mpd_opus_init([[maybe_unused]] const ConfigBlock &block)
{
	LogDebug(opus_domain, opus_get_version_string());

	return true;
}

class MPDOpusDecoder final : public OggDecoder {
	OpusDecoder *opus_decoder = nullptr;
	opus_int16 *output_buffer = nullptr;

	/**
	 * The output gain from the Opus header in dB that should be
	 * applied unconditionally, but is often used specifically for
	 * ReplayGain.  Initialized by OnOggBeginning().
	 */
	float output_gain;

	/**
	 * The pre-skip value from the Opus header.  Initialized by
	 * OnOggBeginning().
	 */
	unsigned pre_skip;

	/**
	 * The number of decoded samples which shall be skipped.  At
	 * the beginning of the file, this gets set to #pre_skip (by
	 * OnOggBeginning()), and may also be set while seeking.
	 */
	unsigned skip;

	/**
	 * If non-zero, then a previous Opus stream has been found
	 * already with this number of channels.  If opus_decoder is
	 * nullptr, then its end-of-stream packet has been found
	 * already.
	 */
	unsigned previous_channels = 0;

	size_t frame_size;

	/**
	 * The granulepos of the next sample to be submitted to
	 * DecoderClient::SubmitData().  Negative if unkown.
	 * Initialized by OnOggBeginning().
	 */
	ogg_int64_t granulepos;

	/**
	 * Was DecoderClient::SubmitReplayGain() called?  We need to
	 * keep track of this, because it will usually be called by
	 * HandleTags(), but if there is no OpusTags packet, we need
	 * to submit our #output_gain value from the OpusHead.
	 */
	bool submitted_replay_gain = false;

public:
	explicit MPDOpusDecoder(DecoderReader &reader)
		:OggDecoder(reader) {}

	~MPDOpusDecoder();

	/**
	 * Has DecoderClient::Ready() been called yet?
	 */
	[[nodiscard]] bool IsInitialized() const {
		return previous_channels != 0;
	}

	bool Seek(uint64_t where_frame);

private:
	void AddGranulepos(ogg_int64_t n) noexcept {
		assert(n >= 0);

		if (granulepos >= 0)
			granulepos += n;
	}

	void HandleTags(const ogg_packet &packet);
	void HandleAudio(const ogg_packet &packet);

protected:
	/* virtual methods from class OggVisitor */
	void OnOggBeginning(const ogg_packet &packet) override;
	void OnOggPacket(const ogg_packet &packet) override;
	void OnOggEnd() override;
};

MPDOpusDecoder::~MPDOpusDecoder()
{
	delete[] output_buffer;

	if (opus_decoder != nullptr)
		opus_decoder_destroy(opus_decoder);
}

void
MPDOpusDecoder::OnOggPacket(const ogg_packet &packet)
{
	if (IsOpusTags(packet))
		HandleTags(packet);
	else
		HandleAudio(packet);
}

void
MPDOpusDecoder::OnOggBeginning(const ogg_packet &packet)
{
	assert(packet.b_o_s);

	if (opus_decoder != nullptr || !IsOpusHead(packet))
		throw std::runtime_error("BOS packet must be OpusHead");

	unsigned channels;
	signed output_gain_i;
	if (!ScanOpusHeader(packet.packet, packet.bytes, channels, output_gain_i, pre_skip) ||
	    !audio_valid_channel_count(channels))
		throw std::runtime_error("Malformed BOS packet");

	/* convert Q7.8 fixed-point to float */
	output_gain = float(output_gain_i) / 256.0f;

	granulepos = 0;
	skip = pre_skip;

	assert(opus_decoder == nullptr);
	assert(IsInitialized() == (output_buffer != nullptr));

	if (IsInitialized() && channels != previous_channels)
		throw FormatRuntimeError("Next stream has different channels (%u -> %u)",
					 previous_channels, channels);

	/* TODO: parse attributes from the OpusHead (sample rate,
	   channels, ...) */

	int opus_error;
	opus_decoder = opus_decoder_create(opus_sample_rate, channels,
					   &opus_error);
	if (opus_decoder == nullptr)
		throw FormatRuntimeError("libopus error: %s",
					 opus_strerror(opus_error));

	if (IsInitialized()) {
		/* decoder was already initialized by the previous
		   stream; skip the rest of this method */
		LogDebug(opus_domain, "Found another stream");
		return;
	}

	const auto eos_granulepos = UpdateEndGranulePos();
	const auto duration = eos_granulepos >= 0
		? SignedSongTime::FromScale<uint64_t>(eos_granulepos,
						      opus_sample_rate)
		: SignedSongTime::Negative();

	previous_channels = channels;
	const AudioFormat audio_format(opus_sample_rate,
				       SampleFormat::S16, channels);
	client.Ready(audio_format, eos_granulepos > 0, duration);
	frame_size = audio_format.GetFrameSize();

	if (output_buffer == nullptr)
		/* note: if we ever support changing the channel count
		   in chained streams, we need to reallocate this
		   buffer instead of keeping it */
		output_buffer = new opus_int16[opus_output_buffer_frames
					       * audio_format.channels];

	auto cmd = client.GetCommand();
	if (cmd != DecoderCommand::NONE)
		throw cmd;
}

void
MPDOpusDecoder::OnOggEnd()
{
	if (!IsSeekable() && IsInitialized()) {
		/* allow chaining of (unseekable) streams */
		assert(opus_decoder != nullptr);
		assert(output_buffer != nullptr);

		opus_decoder_destroy(opus_decoder);
		opus_decoder = nullptr;
	} else
		throw StopDecoder();
}

inline void
MPDOpusDecoder::HandleTags(const ogg_packet &packet)
{
	ReplayGainInfo rgi;
	rgi.Clear();

	TagBuilder tag_builder;
	AddTagHandler h(tag_builder);

	if (!ScanOpusTags(packet.packet, packet.bytes, &rgi, h))
		return;

	if (rgi.IsDefined()) {
		/* submit all valid EBU R128 values with output_gain
		   applied */
		if (rgi.track.IsDefined())
			rgi.track.gain += EbuR128ToReplayGain(output_gain);
		if (rgi.album.IsDefined())
			rgi.album.gain += EbuR128ToReplayGain(output_gain);
		client.SubmitReplayGain(&rgi);
		submitted_replay_gain = true;
	}

	if (!tag_builder.empty()) {
		Tag tag = tag_builder.Commit();
		auto cmd = client.SubmitTag(input_stream, std::move(tag));
		if (cmd != DecoderCommand::NONE)
			throw cmd;
	}
}

inline void
MPDOpusDecoder::HandleAudio(const ogg_packet &packet)
{
	assert(opus_decoder != nullptr);

	if (!submitted_replay_gain) {
		/* if we didn't see an OpusTags packet with EBU R128
		   values, we still need to apply the output gain
		   value from the OpusHead packet; submit it as "track
		   gain" value */
		ReplayGainInfo rgi;
		rgi.Clear();
		rgi.track.gain = EbuR128ToReplayGain(output_gain);
		client.SubmitReplayGain(&rgi);
		submitted_replay_gain = true;
	}

	int nframes = opus_decode(opus_decoder,
				  (const unsigned char*)packet.packet,
				  packet.bytes,
				  output_buffer, opus_output_buffer_frames,
				  0);
	if (gcc_unlikely(nframes <= 0)) {
		if (nframes < 0)
			throw FormatRuntimeError("libopus error: %s",
						 opus_strerror(nframes));
		else
			return;
	}

	/* apply the "skip" value */
	if (skip >= unsigned(nframes)) {
		skip -= nframes;
		AddGranulepos(nframes);
		return;
	}

	const opus_int16 *data = output_buffer;
	data += skip * previous_channels;
	nframes -= skip;
	AddGranulepos(skip);
	skip = 0;

	if (packet.e_o_s && packet.granulepos > 0 && granulepos >= 0) {
		/* End Trimming (RFC7845 4.4): "The page with the 'end
		   of stream' flag set MAY have a granule position
		   that indicates the page contains less audio data
		   than would normally be returned by decoding up
		   through the final packet.  This is used to end the
		   stream somewhere other than an even frame
		   boundary. [...] The remaining samples are
		   discarded. */
		ogg_int64_t remaining = packet.granulepos - granulepos;
		if (remaining <= 0)
			return;

		if (remaining < nframes)
			nframes = remaining;
	}

	/* submit decoded samples to the DecoderClient */
	const size_t nbytes = nframes * frame_size;
	auto cmd = client.SubmitData(input_stream,
				     data, nbytes,
				     0);
	if (cmd != DecoderCommand::NONE)
		throw cmd;

	if (packet.granulepos > 0) {
		granulepos = packet.granulepos;
		client.SubmitTimestamp(FloatDuration(granulepos - pre_skip)
				       / opus_sample_rate);
	} else
		AddGranulepos(nframes);
}

bool
MPDOpusDecoder::Seek(uint64_t where_frame)
{
	assert(IsSeekable());
	assert(input_stream.IsSeekable());
	assert(input_stream.KnownSize());

	const ogg_int64_t where_granulepos(where_frame);

	/* we don't know the exact granulepos after seeking, so let's
	   set it to -1 - it will be set after the next packet which
	   declares its granulepos */
	granulepos = -1;

	try {
		SeekGranulePos(where_granulepos);

		/* since all frame numbers are offset by the file's
		   pre-skip value, we need to apply it here as well;
		   we could just seek to "where_frame+pre_skip" as
		   well, but I think by decoding those samples and
		   discard them, we're safer */
		skip = pre_skip;
		return true;
	} catch (...) {
		return false;
	}
}

void
mpd_opus_stream_decode(DecoderClient &client,
		       InputStream &input_stream)
{
	if (ogg_codec_detect(&client, input_stream) != OGG_CODEC_OPUS)
		return;

	/* rewind the stream, because ogg_codec_detect() has
	   moved it */
	try {
		input_stream.LockRewind();
	} catch (...) {
	}

	DecoderReader reader(client, input_stream);

	MPDOpusDecoder d(reader);

	while (true) {
		try {
			d.Visit();
			break;
		} catch (DecoderCommand cmd) {
			if (cmd == DecoderCommand::SEEK) {
				if (d.Seek(client.GetSeekFrame()))
					client.CommandFinished();
				else
					client.SeekError();
			} else if (cmd != DecoderCommand::NONE)
				break;
		}
	}
}

bool
ReadAndParseOpusHead(OggSyncState &sync, OggStreamState &stream,
		     unsigned &channels, signed &output_gain, unsigned &pre_skip)
{
	ogg_packet packet;

	return OggReadPacket(sync, stream, packet) && packet.b_o_s &&
		IsOpusHead(packet) &&
		ScanOpusHeader(packet.packet, packet.bytes, channels,
			       output_gain, pre_skip) &&
		audio_valid_channel_count(channels);
}

bool
ReadAndVisitOpusTags(OggSyncState &sync, OggStreamState &stream,
		     TagHandler &handler)
{
	ogg_packet packet;

	return OggReadPacket(sync, stream, packet) &&
		IsOpusTags(packet) &&
		ScanOpusTags(packet.packet, packet.bytes,
			     nullptr,
			     handler);
}

void
VisitOpusDuration(InputStream &is, OggSyncState &sync, OggStreamState &stream,
		  ogg_int64_t pre_skip, TagHandler &handler)
{
	ogg_packet packet;

	if (OggSeekFindEOS(sync, stream, packet, is) &&
	    packet.granulepos >= pre_skip) {
		const auto duration =
			SongTime::FromScale<uint64_t>(packet.granulepos,
						      opus_sample_rate);
		handler.OnDuration(duration);
	}
}

static bool
mpd_opus_scan_stream(InputStream &is, TagHandler &handler)
{
	InputStreamReader reader(is);
	OggSyncState oy(reader);

	ogg_page first_page;
	if (!oy.ExpectPage(first_page))
		return false;

	OggStreamState os(first_page);

	unsigned channels, pre_skip;
	signed output_gain;
	if (!ReadAndParseOpusHead(oy, os, channels, output_gain, pre_skip) ||
	    !ReadAndVisitOpusTags(oy, os, handler))
		return false;

	handler.OnAudioFormat(AudioFormat(opus_sample_rate,
					  SampleFormat::S16, channels));

	VisitOpusDuration(is, oy, os, pre_skip, handler);
	return true;
}

const char *const opus_suffixes[] = {
	"opus",
	"ogg",
	"oga",
	nullptr
};

const char *const opus_mime_types[] = {
	/* the official MIME type (RFC 5334) */
	"audio/ogg",

	/* deprecated (RFC 5334) */
	"application/ogg",

	/* deprecated; from an early draft */
	"audio/opus",
	nullptr
};

} /* anonymous namespace */

constexpr DecoderPlugin opus_decoder_plugin =
	DecoderPlugin("opus", mpd_opus_stream_decode, mpd_opus_scan_stream)
	.WithInit(mpd_opus_init)
	.WithSuffixes(opus_suffixes)
	.WithMimeTypes(opus_mime_types);
