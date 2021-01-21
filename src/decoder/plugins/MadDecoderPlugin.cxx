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
#include "MadDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "tag/Id3Scan.hxx"
#include "tag/Id3ReplayGain.hxx"
#include "tag/Handler.hxx"
#include "tag/ReplayGain.hxx"
#include "tag/MixRamp.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "util/Clamp.hxx"
#include "util/StringCompare.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <mad.h>

#ifdef ENABLE_ID3TAG
#include "tag/Id3Unique.hxx"
#include <id3tag.h>
#endif

#include <cassert>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static constexpr unsigned long FRAMES_CUSHION = 2000;

enum class MadDecoderAction {
	SKIP,
	BREAK,
	CONT,
	OK
};

enum class MadDecoderMuteFrame {
	NONE,
	SKIP,
	SEEK
};

/* the number of samples of silence the decoder inserts at start */
static constexpr unsigned DECODERDELAY = 529;

static constexpr Domain mad_domain("mad");

gcc_const
static SongTime
ToSongTime(mad_timer_t t) noexcept
{
	return SongTime::FromMS(mad_timer_count(t, MAD_UNITS_MILLISECONDS));
}

static inline int32_t
mad_fixed_to_24_sample(mad_fixed_t sample) noexcept
{
	static constexpr unsigned bits = 24;
	static constexpr mad_fixed_t MIN = -MAD_F_ONE;
	static constexpr mad_fixed_t MAX = MAD_F_ONE - 1;

	/* round */
	sample = sample + (1L << (MAD_F_FRACBITS - bits));

	/* quantize */
	return Clamp(sample, MIN, MAX)
		>> (MAD_F_FRACBITS + 1 - bits);
}

static void
mad_fixed_to_24_buffer(int32_t *dest, const struct mad_pcm &src,
		       size_t start, size_t end,
		       unsigned int num_channels)
{
	for (size_t i = start; i < end; ++i)
		for (unsigned c = 0; c < num_channels; ++c)
			*dest++ = mad_fixed_to_24_sample(src.samples[c][i]);
}

class MadDecoder {
	static constexpr size_t READ_BUFFER_SIZE = 40960;

	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;
	mad_timer_t timer;
	unsigned char input_buffer[READ_BUFFER_SIZE];
	int32_t output_buffer[sizeof(mad_pcm::samples) / sizeof(mad_fixed_t)];
	SignedSongTime total_time;
	SongTime elapsed_time;
	SongTime seek_time;
	MadDecoderMuteFrame mute_frame = MadDecoderMuteFrame::NONE;
	long *frame_offsets = nullptr;
	mad_timer_t *times = nullptr;
	size_t highest_frame = 0;
	size_t max_frames = 0;
	size_t current_frame = 0;
	unsigned int drop_start_frames;
	unsigned int drop_end_frames;
	unsigned int drop_start_samples = 0;
	unsigned int drop_end_samples = 0;
	bool found_replay_gain = false;
	bool found_first_frame = false;
	bool decoded_first_frame = false;

	/**
	 * If this flag is true, then end-of-file was seen and a
	 * padding of 8 zero bytes were appended to #input_buffer, to
	 * allow libmad to decode the last frame.
	 */
	bool was_eof = false;

	DecoderClient *const client;
	InputStream &input_stream;
	enum mad_layer layer = mad_layer(0);

public:
	MadDecoder(DecoderClient *client, InputStream &input_stream) noexcept;
	~MadDecoder() noexcept;

	void RunDecoder() noexcept;
	bool RunScan(TagHandler &handler) noexcept;

private:
	bool Seek(long offset) noexcept;
	bool FillBuffer() noexcept;
	void ParseId3(size_t tagsize, Tag *tag) noexcept;
	MadDecoderAction DecodeNextFrame(bool skip, Tag *tag) noexcept;

	[[nodiscard]] gcc_pure
	offset_type ThisFrameOffset() const noexcept;

	[[nodiscard]] gcc_pure
	offset_type RestIncludingThisFrame() const noexcept;

	/**
	 * Attempt to calulcate the length of the song from filesize
	 */
	void FileSizeToSongLength() noexcept;

	bool DecodeFirstFrame(Tag *tag) noexcept;

	void AllocateBuffers() noexcept {
		assert(max_frames > 0);
		assert(frame_offsets == nullptr);
		assert(times == nullptr);

		frame_offsets = new long[max_frames];
		times = new mad_timer_t[max_frames];
	}

	[[nodiscard]] gcc_pure
	size_t TimeToFrame(SongTime t) const noexcept;

	/**
	 * Record the current frame's offset in the "frame_offsets"
	 * buffer and go forward to the next frame, updating the
	 * attributes "current_frame" and "timer".
	 */
	void UpdateTimerNextFrame() noexcept;

	/**
	 * Sends the synthesized current frame via
	 * DecoderClient::SubmitData().
	 */
	DecoderCommand SubmitPCM(size_t start, size_t n) noexcept;

	/**
	 * Synthesize the current frame and send it via
	 * DecoderClient::SubmitData().
	 */
	DecoderCommand SynthAndSubmit() noexcept;

	/**
	 * @return false to stop decoding
	 */
	bool HandleCurrentFrame() noexcept;

	bool LoadNextFrame() noexcept;

	bool Read() noexcept;
};

MadDecoder::MadDecoder(DecoderClient *_client,
		       InputStream &_input_stream) noexcept
	:client(_client), input_stream(_input_stream)
{
	mad_stream_init(&stream);
	mad_stream_options(&stream, MAD_OPTION_IGNORECRC);
	mad_frame_init(&frame);
	mad_synth_init(&synth);
	mad_timer_reset(&timer);
}

inline bool
MadDecoder::Seek(long offset) noexcept
{
	try {
		input_stream.LockSeek(offset);
	} catch (...) {
		return false;
	}

	mad_stream_buffer(&stream, input_buffer, 0);
	stream.error = MAD_ERROR_NONE;

	return true;
}

inline bool
MadDecoder::FillBuffer() noexcept
{
	/* amount of rest data still residing in the buffer */
	size_t rest_size = 0;

	size_t max_read_size = sizeof(input_buffer);
	unsigned char *dest = input_buffer;

	if (stream.next_frame != nullptr) {
		rest_size = stream.bufend - stream.next_frame;
		memmove(input_buffer, stream.next_frame, rest_size);
		dest += rest_size;
		max_read_size -= rest_size;
	}

	/* we've exhausted the read buffer, so give up!, these potential
	 * mp3 frames are way too big, and thus unlikely to be mp3 frames */
	if (max_read_size == 0)
		return false;

	size_t nbytes = decoder_read(client, input_stream,
				     dest, max_read_size);
	if (nbytes == 0) {
		if (was_eof || max_read_size < MAD_BUFFER_GUARD)
			return false;

		was_eof = true;
		nbytes = MAD_BUFFER_GUARD;
		memset(dest, 0, nbytes);
	}

	mad_stream_buffer(&stream, input_buffer, rest_size + nbytes);
	stream.error = MAD_ERROR_NONE;

	return true;
}

#ifdef ENABLE_ID3TAG
gcc_pure
static MixRampInfo
parse_id3_mixramp(struct id3_tag *tag) noexcept
{
	MixRampInfo result;

	struct id3_frame *frame;
	for (unsigned i = 0; (frame = id3_tag_findframe(tag, "TXXX", i)); i++) {
		if (frame->nfields < 3)
			continue;

		char *const key = reinterpret_cast<char *>(id3_ucs4_latin1duplicate(id3_field_getstring
					     (&frame->fields[1])));
		char *const value = reinterpret_cast<char *>(id3_ucs4_latin1duplicate(id3_field_getstring
					     (&frame->fields[2])));

		ParseMixRampTag(result, key, value);

		free(key);
		free(value);
	}

	return result;
}
#endif

inline void
MadDecoder::ParseId3(size_t tagsize, Tag *mpd_tag) noexcept
{
#ifdef ENABLE_ID3TAG
	std::unique_ptr<id3_byte_t[]> allocated;

	const id3_length_t count = stream.bufend - stream.this_frame;

	const id3_byte_t *id3_data;
	if (tagsize <= count) {
		id3_data = stream.this_frame;
		mad_stream_skip(&(stream), tagsize);
	} else {
		allocated.reset(new id3_byte_t[tagsize]);
		memcpy(allocated.get(), stream.this_frame, count);
		mad_stream_skip(&(stream), count);

		if (!decoder_read_full(client, input_stream,
				       allocated.get() + count, tagsize - count)) {
			LogDebug(mad_domain, "error parsing ID3 tag");
			return;
		}

		id3_data = allocated.get();
	}

	const UniqueId3Tag id3_tag(id3_tag_parse(id3_data, tagsize));
	if (id3_tag == nullptr)
		return;

	if (mpd_tag != nullptr)
		*mpd_tag = tag_id3_import(id3_tag.get());

	if (client != nullptr) {
		ReplayGainInfo rgi;

		if (Id3ToReplayGainInfo(rgi, id3_tag.get())) {
			client->SubmitReplayGain(&rgi);
			found_replay_gain = true;
		}

		client->SubmitMixRamp(parse_id3_mixramp(id3_tag.get()));
	}

#else /* !ENABLE_ID3TAG */
	(void)mpd_tag;

	/* This code is enabled when libid3tag is disabled.  Instead
	   of parsing the ID3 frame, it just skips it. */

	size_t count = stream.bufend - stream.this_frame;

	if (tagsize <= count) {
		mad_stream_skip(&stream, tagsize);
	} else {
		mad_stream_skip(&stream, count);
		decoder_skip(client, input_stream, tagsize - count);
	}
#endif
}

#ifndef ENABLE_ID3TAG
/**
 * This function emulates libid3tag when it is disabled.  Instead of
 * doing a real analyzation of the frame, it just checks whether the
 * frame begins with the string "ID3".  If so, it returns the length
 * of the ID3 frame.
 */
static signed long
id3_tag_query(const void *p0, size_t length) noexcept
{
	const char *p = (const char *)p0;

	return length >= 10 && memcmp(p, "ID3", 3) == 0
		? (p[8] << 7) + p[9] + 10
		: 0;
}
#endif /* !ENABLE_ID3TAG */

static MadDecoderAction
RecoverFrameError(const struct mad_stream &stream) noexcept
{
	if (MAD_RECOVERABLE(stream.error))
		return MadDecoderAction::SKIP;

	FormatWarning(mad_domain,
		      "unrecoverable frame level error: %s",
		      mad_stream_errorstr(&stream));
	return MadDecoderAction::BREAK;
}

MadDecoderAction
MadDecoder::DecodeNextFrame(bool skip, Tag *tag) noexcept
{
	if ((stream.buffer == nullptr || stream.error == MAD_ERROR_BUFLEN) &&
	    !FillBuffer())
		return MadDecoderAction::BREAK;

	if (mad_header_decode(&frame.header, &stream)) {
		if (stream.error == MAD_ERROR_BUFLEN)
			return MadDecoderAction::CONT;

		if (stream.error == MAD_ERROR_LOSTSYNC && stream.this_frame) {
			signed long tagsize = id3_tag_query(stream.this_frame,
							    stream.bufend -
							    stream.this_frame);

			if (tagsize > 0) {
				ParseId3(size_t(tagsize), tag);
				return MadDecoderAction::CONT;
			}
		}

		return RecoverFrameError(stream);
	}

	enum mad_layer new_layer = frame.header.layer;
	if (layer == mad_layer(0)) {
		if (new_layer != MAD_LAYER_II && new_layer != MAD_LAYER_III) {
			/* Only layer 2 and 3 have been tested to work */
			return MadDecoderAction::SKIP;
		}

		layer = new_layer;
	} else if (new_layer != layer) {
		/* Don't decode frames with a different layer than the first */
		return MadDecoderAction::SKIP;
	}

	if (!skip && mad_frame_decode(&frame, &stream))
		return RecoverFrameError(stream);

	return MadDecoderAction::OK;
}

/* xing stuff stolen from alsaplayer, and heavily modified by jat */
static constexpr unsigned XI_MAGIC = (('X' << 8) | 'i');
static constexpr unsigned NG_MAGIC = (('n' << 8) | 'g');
static constexpr unsigned IN_MAGIC = (('I' << 8) | 'n');
static constexpr unsigned FO_MAGIC = (('f' << 8) | 'o');

struct xing {
	long flags;             /* valid fields (see below) */
	unsigned long frames;   /* total number of frames */
	unsigned long bytes;    /* total number of bytes */
	unsigned char toc[100]; /* 100-point seek table */
	long scale;             /* VBR quality */
};

static constexpr unsigned XING_FRAMES = 1;
static constexpr unsigned XING_BYTES = 2;
static constexpr unsigned XING_TOC = 4;
static constexpr unsigned XING_SCALE = 8;

struct lame_version {
	unsigned major;
	unsigned minor;
};

struct lame {
	char encoder[10];       /* 9 byte encoder name/version ("LAME3.97b") */
	struct lame_version version; /* struct containing just the version */
	float peak;             /* replaygain peak */
	float track_gain;       /* replaygain track gain */
	float album_gain;       /* replaygain album gain */
	int encoder_delay;      /* # of added samples at start of mp3 */
	int encoder_padding;    /* # of added samples at end of mp3 */
	int crc;                /* CRC of the first 190 bytes of this frame */
};

static bool
parse_xing(struct xing *xing, struct mad_bitptr *ptr, int *oldbitlen) noexcept
{
	int bitlen = *oldbitlen;

	if (bitlen < 16)
		return false;

	const unsigned long bits = mad_bit_read(ptr, 16);
	bitlen -= 16;

	if (bits == XI_MAGIC) {
		if (bitlen < 16)
			return false;

		if (mad_bit_read(ptr, 16) != NG_MAGIC)
			return false;

		bitlen -= 16;
	} else if (bits == IN_MAGIC) {
		if (bitlen < 16)
			return false;

		if (mad_bit_read(ptr, 16) != FO_MAGIC)
			return false;

		bitlen -= 16;
	}
	else if (bits != NG_MAGIC && bits != FO_MAGIC)
		return false;

	if (bitlen < 32)
		return false;
	xing->flags = mad_bit_read(ptr, 32);
	bitlen -= 32;

	if (xing->flags & XING_FRAMES) {
		if (bitlen < 32)
			return false;
		xing->frames = mad_bit_read(ptr, 32);
		bitlen -= 32;
	}

	if (xing->flags & XING_BYTES) {
		if (bitlen < 32)
			return false;
		xing->bytes = mad_bit_read(ptr, 32);
		bitlen -= 32;
	}

	if (xing->flags & XING_TOC) {
		if (bitlen < 800)
			return false;
		for (unsigned char & i : xing->toc)
			i = mad_bit_read(ptr, 8);
		bitlen -= 800;
	}

	if (xing->flags & XING_SCALE) {
		if (bitlen < 32)
			return false;
		xing->scale = mad_bit_read(ptr, 32);
		bitlen -= 32;
	}

	/* Make sure we consume no less than 120 bytes (960 bits) in hopes that
	 * the LAME tag is found there, and not right after the Xing header */
	const int bitsleft = 960 - (*oldbitlen - bitlen);
	if (bitsleft < 0)
		return false;
	else if (bitsleft > 0) {
		mad_bit_skip(ptr, bitsleft);
		bitlen -= bitsleft;
	}

	*oldbitlen = bitlen;

	return true;
}

static bool
parse_lame(struct lame *lame, struct mad_bitptr *ptr, int *bitlen) noexcept
{
	/* Unlike the xing header, the lame tag has a fixed length.  Fail if
	 * not all 36 bytes (288 bits) are there. */
	if (*bitlen < 288)
		return false;

	for (unsigned i = 0; i < 9; i++)
		lame->encoder[i] = char(mad_bit_read(ptr, 8));
	lame->encoder[9] = '\0';

	*bitlen -= 72;

	/* This is technically incorrect, since the encoder might not be lame.
	 * But there's no other way to determine if this is a lame tag, and we
	 * wouldn't want to go reading a tag that's not there. */
	if (!StringStartsWith(lame->encoder, "LAME"))
		return false;

	if (sscanf(lame->encoder+4, "%u.%u",
	           &lame->version.major, &lame->version.minor) != 2)
		return false;

	FormatDebug(mad_domain, "detected LAME version %i.%i (\"%s\")",
		    lame->version.major, lame->version.minor, lame->encoder);

	/* The reference volume was changed from the 83dB used in the
	 * ReplayGain spec to 89dB in lame 3.95.1.  Bump the gain for older
	 * versions, since everyone else uses 89dB instead of 83dB.
	 * Unfortunately, lame didn't differentiate between 3.95 and 3.95.1, so
	 * it's impossible to make the proper adjustment for 3.95.
	 * Fortunately, 3.95 was only out for about a day before 3.95.1 was
	 * released. -- tmz */
	int adj = 0;
	if (lame->version.major < 3 ||
	    (lame->version.major == 3 && lame->version.minor < 95))
		adj = 6;

	mad_bit_skip(ptr, 16);

	lame->peak = MAD_F(mad_bit_read(ptr, 32) << 5); /* peak */
	FormatDebug(mad_domain, "LAME peak found: %f", double(lame->peak));

	lame->track_gain = 0;
	unsigned name = mad_bit_read(ptr, 3); /* gain name */
	unsigned orig = mad_bit_read(ptr, 3); /* gain originator */
	unsigned sign = mad_bit_read(ptr, 1); /* sign bit */
	int gain = mad_bit_read(ptr, 9); /* gain*10 */
	if (gain && name == 1 && orig != 0) {
		lame->track_gain = ((sign ? -gain : gain) / 10.0f) + adj;
		FormatDebug(mad_domain, "LAME track gain found: %f",
			    double(lame->track_gain));
	}

	/* tmz reports that this isn't currently written by any version of lame
	 * (as of 3.97).  Since we have no way of testing it, don't use it.
	 * Wouldn't want to go blowing someone's ears just because we read it
	 * wrong. :P -- jat */
	lame->album_gain = 0;
#if 0
	name = mad_bit_read(ptr, 3); /* gain name */
	orig = mad_bit_read(ptr, 3); /* gain originator */
	sign = mad_bit_read(ptr, 1); /* sign bit */
	gain = mad_bit_read(ptr, 9); /* gain*10 */
	if (gain && name == 2 && orig != 0) {
		lame->album_gain = ((sign ? -gain : gain) / 10.0) + adj;
		FormatDebug(mad_domain, "LAME album gain found: %f",
			    double(lame->track_gain));
	}
#else
	mad_bit_skip(ptr, 16);
#endif

	mad_bit_skip(ptr, 16);

	lame->encoder_delay = mad_bit_read(ptr, 12);
	lame->encoder_padding = mad_bit_read(ptr, 12);

	FormatDebug(mad_domain, "encoder delay is %i, encoder padding is %i",
		    lame->encoder_delay, lame->encoder_padding);

	mad_bit_skip(ptr, 80);

	lame->crc = mad_bit_read(ptr, 16);

	*bitlen -= 216;

	return true;
}

static inline SongTime
mad_frame_duration(const struct mad_frame *frame) noexcept
{
	return ToSongTime(frame->header.duration);
}

inline offset_type
MadDecoder::ThisFrameOffset() const noexcept
{
	auto offset = input_stream.GetOffset();

	if (stream.this_frame != nullptr)
		offset -= stream.bufend - stream.this_frame;
	else
		offset -= stream.bufend - stream.buffer;

	return offset;
}

inline offset_type
MadDecoder::RestIncludingThisFrame() const noexcept
{
	return input_stream.GetSize() - ThisFrameOffset();
}

inline void
MadDecoder::FileSizeToSongLength() noexcept
{
	if (input_stream.KnownSize()) {
		offset_type rest = RestIncludingThisFrame();

		const SongTime frame_duration = mad_frame_duration(&frame);
		const SongTime duration =
			SongTime::FromScale<uint64_t>(rest,
						      frame.header.bitrate / 8);
		total_time = duration;

		max_frames = (frame_duration.IsPositive()
			      ? duration.count() / frame_duration.count()
			      : 0)
			+ FRAMES_CUSHION;
	} else {
		max_frames = FRAMES_CUSHION;
		total_time = SignedSongTime::Negative();
	}
}

inline bool
MadDecoder::DecodeFirstFrame(Tag *tag) noexcept
{
	struct xing xing;

#if GCC_CHECK_VERSION(10,0)
	/* work around bogus -Wuninitialized in GCC 10 */
	xing.frames = 0;
#endif

	while (true) {
		const auto action = DecodeNextFrame(false, tag);
		switch (action) {
		case MadDecoderAction::SKIP:
		case MadDecoderAction::CONT:
			continue;

		case MadDecoderAction::BREAK:
			return false;

		case MadDecoderAction::OK:
			break;
		}

		break;
	}

	struct mad_bitptr ptr = stream.anc_ptr;
	int bitlen = stream.anc_bitlen;

	FileSizeToSongLength();

	/*
	 * if an xing tag exists, use that!
	 */
	if (parse_xing(&xing, &ptr, &bitlen)) {
		mute_frame = MadDecoderMuteFrame::SKIP;

		if ((xing.flags & XING_FRAMES) && xing.frames) {
			mad_timer_t duration = frame.header.duration;
			mad_timer_multiply(&duration, xing.frames);
			total_time = ToSongTime(duration);
			max_frames = xing.frames;
		}

		struct lame lame;
		if (parse_lame(&lame, &ptr, &bitlen)) {
			if (input_stream.IsSeekable()) {
				/* libmad inserts 529 samples of
				   silence at the beginning and
				   removes those 529 samples at the
				   end */
				drop_start_samples = lame.encoder_delay +
				                           DECODERDELAY;
				drop_end_samples = lame.encoder_padding;
				if (drop_end_samples > DECODERDELAY)
					drop_end_samples -= DECODERDELAY;
				else
					drop_end_samples = 0;
			}

			/* Album gain isn't currently used.  See comment in
			 * parse_lame() for details. -- jat */
			if (client != nullptr && !found_replay_gain &&
			    lame.track_gain > 0.0f) {
				ReplayGainInfo rgi;
				rgi.Clear();
				rgi.track.gain = lame.track_gain;
				rgi.track.peak = lame.peak;
				client->SubmitReplayGain(&rgi);
			}
		}
	}

	if (!max_frames)
		return false;

	if (max_frames > 8 * 1024 * 1024) {
		FormatWarning(mad_domain,
			      "mp3 file header indicates too many frames: %zu",
			      max_frames);
		return false;
	}

	return true;
}

MadDecoder::~MadDecoder() noexcept
{
	mad_synth_finish(&synth);
	mad_frame_finish(&frame);
	mad_stream_finish(&stream);

	delete[] frame_offsets;
	delete[] times;
}

size_t
MadDecoder::TimeToFrame(SongTime t) const noexcept
{
	size_t i;

	for (i = 0; i < highest_frame; ++i) {
		auto frame_time = ToSongTime(times[i]);
		if (frame_time >= t)
			break;
	}

	return i;
}

void
MadDecoder::UpdateTimerNextFrame() noexcept
{
	if (current_frame >= highest_frame) {
		/* record this frame's properties in frame_offsets
		   (for seeking) and times */

		if (current_frame >= max_frames)
			/* cap current_frame */
			current_frame = max_frames - 1;
		else
			highest_frame++;

		frame_offsets[current_frame] = ThisFrameOffset();

		mad_timer_add(&timer, frame.header.duration);
		times[current_frame] = timer;
	} else
		/* get the new timer value from "times" */
		timer = times[current_frame];

	current_frame++;
	elapsed_time = ToSongTime(timer);
}

DecoderCommand
MadDecoder::SubmitPCM(size_t i, size_t pcm_length) noexcept
{
	size_t num_samples = pcm_length - i;

	mad_fixed_to_24_buffer(output_buffer, synth.pcm,
			       i, i + num_samples,
			       MAD_NCHANNELS(&frame.header));
	num_samples *= MAD_NCHANNELS(&frame.header);

	return client->SubmitData(input_stream, output_buffer,
				  sizeof(output_buffer[0]) * num_samples,
				  frame.header.bitrate / 1000);
}

inline DecoderCommand
MadDecoder::SynthAndSubmit() noexcept
{
	mad_synth_frame(&synth, &frame);

	if (!found_first_frame) {
		unsigned int samples_per_frame = synth.pcm.length;
		drop_start_frames = drop_start_samples / samples_per_frame;
		drop_end_frames = drop_end_samples / samples_per_frame;
		drop_start_samples = drop_start_samples % samples_per_frame;
		drop_end_samples = drop_end_samples % samples_per_frame;
		found_first_frame = true;
	}

	if (drop_start_frames > 0) {
		drop_start_frames--;
		return DecoderCommand::NONE;
	} else if ((drop_end_frames > 0) &&
		   current_frame == max_frames - drop_end_frames) {
		/* stop decoding, effectively dropping all remaining
		   frames */
		return DecoderCommand::STOP;
	}

	size_t i = 0;
	if (!decoded_first_frame) {
		i = drop_start_samples;
		decoded_first_frame = true;
	}

	size_t pcm_length = synth.pcm.length;
	if (drop_end_samples &&
	    current_frame == max_frames - drop_end_frames - 1) {
		if (drop_end_samples >= pcm_length)
			return DecoderCommand::STOP;

		pcm_length -= drop_end_samples;
	}

	auto cmd = SubmitPCM(i, pcm_length);
	if (cmd != DecoderCommand::NONE)
		return cmd;

	if (drop_end_samples &&
	    current_frame == max_frames - drop_end_frames - 1)
		/* stop decoding, effectively dropping
		 * all remaining samples */
		return DecoderCommand::STOP;

	return DecoderCommand::NONE;
}

inline bool
MadDecoder::HandleCurrentFrame() noexcept
{
	switch (mute_frame) {
		DecoderCommand cmd;

	case MadDecoderMuteFrame::SKIP:
		mute_frame = MadDecoderMuteFrame::NONE;
		break;
	case MadDecoderMuteFrame::SEEK:
		if (elapsed_time >= seek_time)
			mute_frame = MadDecoderMuteFrame::NONE;
		UpdateTimerNextFrame();
		break;
	case MadDecoderMuteFrame::NONE:
		cmd = SynthAndSubmit();
		UpdateTimerNextFrame();
		if (cmd == DecoderCommand::SEEK) {
			assert(input_stream.IsSeekable());

			const auto t = client->GetSeekTime();
			size_t j = TimeToFrame(t);
			if (j < highest_frame) {
				if (Seek(frame_offsets[j])) {
					current_frame = j;
					was_eof = false;
					client->CommandFinished();
				} else
					client->SeekError();
			} else {
				seek_time = t;
				mute_frame = MadDecoderMuteFrame::SEEK;
				client->CommandFinished();
			}
		} else if (cmd != DecoderCommand::NONE)
			return false;
	}

	return true;
}

inline bool
MadDecoder::LoadNextFrame() noexcept
{
	while (true) {
		Tag tag;

		const auto action =
			DecodeNextFrame(mute_frame != MadDecoderMuteFrame::NONE,
					&tag);
		if (!tag.IsEmpty())
			client->SubmitTag(input_stream, std::move(tag));

		switch (action) {
		case MadDecoderAction::SKIP:
		case MadDecoderAction::CONT:
			continue;

		case MadDecoderAction::BREAK:
			return false;

		case MadDecoderAction::OK:
			return true;
		}
	}
}

inline bool
MadDecoder::Read() noexcept
{
	return HandleCurrentFrame() &&
		LoadNextFrame();
}

inline void
MadDecoder::RunDecoder() noexcept
{
	assert(client != nullptr);

	Tag tag;
	if (!DecodeFirstFrame(&tag)) {
		if (client->GetCommand() == DecoderCommand::NONE)
			LogError(mad_domain,
				 "input does not appear to be a mp3 bit stream");
		return;
	}

	AllocateBuffers();

	client->Ready(CheckAudioFormat(frame.header.samplerate,
				       SampleFormat::S24_P32,
				       MAD_NCHANNELS(&frame.header)),
		      input_stream.IsSeekable(),
		      total_time);

	if (!tag.IsEmpty())
		client->SubmitTag(input_stream, std::move(tag));

	while (Read()) {}
}

static void
mad_decode(DecoderClient &client, InputStream &input_stream)
{
	MadDecoder data(&client, input_stream);
	data.RunDecoder();
}

inline bool
MadDecoder::RunScan(TagHandler &handler) noexcept
{
	if (!DecodeFirstFrame(nullptr))
		return false;

	if (!total_time.IsNegative())
		handler.OnDuration(SongTime(total_time));

	try {
		handler.OnAudioFormat(CheckAudioFormat(frame.header.samplerate,
						       SampleFormat::S24_P32,
						       MAD_NCHANNELS(&frame.header)));
	} catch (...) {
	}

	return true;
}

static bool
mad_decoder_scan_stream(InputStream &is, TagHandler &handler)
{
	MadDecoder data(nullptr, is);
	return data.RunScan(handler);
}

static const char *const mad_suffixes[] = { "mp3", "mp2", nullptr };
static const char *const mad_mime_types[] = { "audio/mpeg", nullptr };

constexpr DecoderPlugin mad_decoder_plugin =
	DecoderPlugin("mad", mad_decode, mad_decoder_scan_stream)
	.WithSuffixes(mad_suffixes)
	.WithMimeTypes(mad_mime_types);
