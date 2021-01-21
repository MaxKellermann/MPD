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
#include "WavpackDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "fs/Path.hxx"
#include "util/Alloc.hxx"
#include "util/Math.hxx"
#include "util/ScopeExit.hxx"
#include "util/RuntimeError.hxx"

#include <wavpack/wavpack.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iterator>
#include <memory>

#define ERRORLEN 80

#ifdef OPEN_DSD_AS_PCM
/* libWavPack supports DSD since version 5 */
  #ifdef ENABLE_DSD
static constexpr int OPEN_DSD_FLAG = OPEN_DSD_NATIVE;
  #else
static constexpr int OPEN_DSD_FLAG = OPEN_DSD_AS_PCM;
  #endif
#else
/* no DSD support in this libWavPack version */
static constexpr int OPEN_DSD_FLAG = 0;
#endif

static WavpackContext *
WavpackOpenInput(Path path, int flags, int norm_offset)
{
	char error[ERRORLEN];
	auto *wpc = WavpackOpenFileInput(path.c_str(), error,
					 flags, norm_offset);
	if (wpc == nullptr)
		throw FormatRuntimeError("failed to open WavPack file \"%s\": %s",
					 path.c_str(), error);

	return wpc;
}

#ifdef OPEN_DSD_AS_PCM

static WavpackContext *
WavpackOpenInput(WavpackStreamReader64 *reader, void *wv_id, void *wvc_id,
		 int flags, int norm_offset)
{
	char error[ERRORLEN];
	auto *wpc = WavpackOpenFileInputEx64(reader, wv_id, wvc_id, error,
					   flags, norm_offset);
	if (wpc == nullptr)
		throw FormatRuntimeError("failed to open WavPack stream: %s",
					 error);

	return wpc;
}

#else

static WavpackContext *
WavpackOpenInput(WavpackStreamReader *reader, void *wv_id, void *wvc_id,
		 int flags, int norm_offset)
{
	char error[ERRORLEN];
	auto *wpc = WavpackOpenFileInputEx(reader, wv_id, wvc_id, error,
					   flags, norm_offset);
	if (wpc == nullptr)
		throw FormatRuntimeError("failed to open WavPack stream: %s",
					 error);

	return wpc;
}

#endif

gcc_pure
static SignedSongTime
GetDuration(WavpackContext *wpc) noexcept
{
#ifdef OPEN_DSD_AS_PCM
	/* libWavPack 5 */
	const auto n_samples = WavpackGetNumSamples64(wpc);
	if (n_samples == -1)
		/* unknown */
		return SignedSongTime::Negative();
#else
	const uint32_t n_samples = WavpackGetNumSamples(wpc);
	if (n_samples == uint32_t(-1))
		/* unknown */
		return SignedSongTime::Negative();
#endif

	return SongTime::FromScale<uint64_t>(n_samples,
					     WavpackGetSampleRate(wpc));
}

/*
 * Convert integer samples.
 */
template<typename T>
static void
format_samples_int(void *buffer, uint32_t count)
{
	auto *src = static_cast<int32_t *>(buffer);
	T *dst = (T *)buffer;
	/*
	 * The asserts like the following one are because we do the
	 * formatting of samples within a single buffer. The size of
	 * the output samples never can be greater than the size of
	 * the input ones. Otherwise we would have an overflow.
	 */
	static_assert(sizeof(*dst) <= sizeof(*src), "Wrong size");

	/* pass through and align samples */
	std::copy_n(src, count, dst);
}

/*
 * No conversion necessary.
 */
static void
format_samples_nop([[maybe_unused]] void *buffer, [[maybe_unused]] uint32_t count)
{
	/* do nothing */
}

/**
 * Choose a MPD sample format from libwavpacks' number of bits.
 */
static SampleFormat
wavpack_bits_to_sample_format(bool is_float,
#if defined(OPEN_DSD_AS_PCM) && defined(ENABLE_DSD)
			      bool is_dsd,
#endif
			      int bytes_per_sample)
{
	if (is_float)
		return SampleFormat::FLOAT;

#if defined(OPEN_DSD_AS_PCM) && defined(ENABLE_DSD)
	if (is_dsd)
		return SampleFormat::DSD;
#endif

	switch (bytes_per_sample) {
	case 1:
		return SampleFormat::S8;

	case 2:
		return SampleFormat::S16;

	case 3:
		return SampleFormat::S24_P32;

	case 4:
		return SampleFormat::S32;

	default:
		return SampleFormat::UNDEFINED;
	}
}

static AudioFormat
CheckAudioFormat(WavpackContext *wpc)
{
	const bool is_float = (WavpackGetMode(wpc) & MODE_FLOAT) != 0;
#if defined(OPEN_DSD_AS_PCM) && defined(ENABLE_DSD)
	const bool is_dsd = (WavpackGetQualifyMode(wpc) & QMODE_DSD_AUDIO) != 0;
#endif
	SampleFormat sample_format =
		wavpack_bits_to_sample_format(is_float,
#if defined(OPEN_DSD_AS_PCM) && defined(ENABLE_DSD)
					      is_dsd,
#endif
					      WavpackGetBytesPerSample(wpc));

	return CheckAudioFormat(WavpackGetSampleRate(wpc),
				sample_format,
				WavpackGetReducedChannels(wpc));
}

/*
 * This does the main decoding thing.
 * Requires an already opened WavpackContext.
 */
static void
wavpack_decode(DecoderClient &client, WavpackContext *wpc, bool can_seek)
{
	const auto audio_format = CheckAudioFormat(wpc);

	auto *format_samples = format_samples_nop;
	if (audio_format.format == SampleFormat::DSD)
		format_samples = format_samples_int<uint8_t>;
	else if (audio_format.format != SampleFormat::FLOAT) {
		switch (WavpackGetBytesPerSample(wpc)) {
		case 1:
			format_samples = format_samples_int<int8_t>;
			break;

		case 2:
			format_samples = format_samples_int<int16_t>;
			break;
		}
	}

	client.Ready(audio_format, can_seek, GetDuration(wpc));

	const int output_sample_size = audio_format.GetFrameSize();

	/* wavpack gives us all kind of samples in a 32-bit space */
	int32_t chunk[1024];
	const uint32_t samples_requested = std::size(chunk) /
		audio_format.channels;

	DecoderCommand cmd = client.GetCommand();
	while (cmd != DecoderCommand::STOP) {
		if (cmd == DecoderCommand::SEEK) {
			if (can_seek) {
				auto where = client.GetSeekFrame();
#ifdef OPEN_DSD_AS_PCM
				bool success = WavpackSeekSample64(wpc, where);
#else
				bool success = WavpackSeekSample(wpc, where);
#endif
				if (!success) {
					/* seek errors are fatal */
					client.SeekError();
					break;
				}

				client.CommandFinished();
			} else {
				client.SeekError();
			}
		}

		uint32_t samples_got = WavpackUnpackSamples(wpc, chunk,
							    samples_requested);
		if (samples_got == 0)
			break;

		int bitrate = lround(WavpackGetInstantBitrate(wpc) / 1000);
		format_samples(chunk, samples_got * audio_format.channels);

		cmd = client.SubmitData(nullptr, chunk,
					samples_got * output_sample_size,
					bitrate);
	}
}

/*
 * #InputStream <=> WavpackStreamReader wrapper callbacks
 */

/* This struct is needed for per-stream last_byte storage. */
struct WavpackInput {
	DecoderClient *const client;
	InputStream &is;
	/* Needed for push_back_byte() */
	int last_byte;

	constexpr WavpackInput(DecoderClient *_client, InputStream &_is)
		:client(_client), is(_is), last_byte(EOF) {}

	int32_t ReadBytes(void *data, size_t bcount);

	[[nodiscard]] InputStream::offset_type GetPos() const {
		return is.GetOffset();
	}

	int SetPosAbs(InputStream::offset_type pos) {
		try {
			is.LockSeek(pos);
			return 0;
		} catch (...) {
			return -1;
		}
	}

	int SetPosRel(InputStream::offset_type delta, int mode) {
		offset_type offset = delta;
		switch (mode) {
		case SEEK_SET:
			break;

		case SEEK_CUR:
			offset += is.GetOffset();
			break;

		case SEEK_END:
			if (!is.KnownSize())
				return -1;

			offset += is.GetSize();
			break;

		default:
			return -1;
		}

		return SetPosAbs(offset);
	}

	int PushBackByte(int c) {
		if (last_byte == EOF) {
			last_byte = c;
			return c;
		} else {
			return EOF;
		}
	}

	[[nodiscard]] InputStream::offset_type GetLength() const {
		if (!is.KnownSize())
			return 0;

		return is.GetSize();
	}

	[[nodiscard]] bool CanSeek() const {
		return is.IsSeekable();
	}
};

/**
 * Little wrapper for struct WavpackInput to cast from void *.
 */
static WavpackInput *
wpin(void *id)
{
	assert(id);
	return static_cast<WavpackInput *>(id);
}

static int32_t
wavpack_input_read_bytes(void *id, void *data, int32_t bcount)
{
	return wpin(id)->ReadBytes(data, bcount);
}

int32_t
WavpackInput::ReadBytes(void *data, size_t bcount)
{
	auto *buf = static_cast<uint8_t *>(data);
	int32_t i = 0;

	if (last_byte != EOF) {
		*buf++ = last_byte;
		last_byte = EOF;
		--bcount;
		++i;
	}

	/* wavpack fails if we return a partial read, so we just wait
	   until the buffer is full */
	while (bcount > 0) {
		size_t nbytes = decoder_read(client, is, buf, bcount);
		if (nbytes == 0) {
			/* EOF, error or a decoder command */
			break;
		}

		i += nbytes;
		bcount -= nbytes;
		buf += nbytes;
	}

	return i;
}

#ifdef OPEN_DSD_AS_PCM

static int64_t
wavpack_input_get_pos(void *id)
{
	const auto &wpi = *wpin(id);
	return wpi.GetPos();
}

static int
wavpack_input_set_pos_abs(void *id, int64_t pos)
{
	auto &wpi = *wpin(id);
	return wpi.SetPosAbs(pos);
}

static int
wavpack_input_set_pos_rel(void *id, int64_t delta, int mode)
{
	auto &wpi = *wpin(id);
	return wpi.SetPosRel(delta, mode);
}

#else

static uint32_t
wavpack_input_get_pos(void *id)
{
	const auto &wpi = *wpin(id);
	return wpi.GetPos();
}

static int
wavpack_input_set_pos_abs(void *id, uint32_t pos)
{
	auto &wpi = *wpin(id);
	return wpi.SetPosAbs(pos);
}

static int
wavpack_input_set_pos_rel(void *id, int32_t delta, int mode)
{
	auto &wpi = *wpin(id);
	return wpi.SetPosRel(delta, mode);
}

#endif

static int
wavpack_input_push_back_byte(void *id, int c)
{
	auto &wpi = *wpin(id);
	return wpi.PushBackByte(c);
}

#ifdef OPEN_DSD_AS_PCM

static int64_t
wavpack_input_get_length(void *id)
{
	const auto &wpi = *wpin(id);
	return wpi.GetLength();
}

#else

static uint32_t
wavpack_input_get_length(void *id)
{
	const auto &wpi = *wpin(id);
	return wpi.GetLength();
}

#endif

static int
wavpack_input_can_seek(void *id)
{
	const auto &wpi = *wpin(id);
	return wpi.CanSeek();
}

#ifdef OPEN_DSD_AS_PCM

static WavpackStreamReader64 mpd_is_reader = {
	wavpack_input_read_bytes,
	nullptr, /* write_bytes */
	wavpack_input_get_pos,
	wavpack_input_set_pos_abs,
	wavpack_input_set_pos_rel,
	wavpack_input_push_back_byte,
	wavpack_input_get_length,
	wavpack_input_can_seek,
	nullptr, /* truncate_here */
	nullptr, /* close */
};

#else

static WavpackStreamReader mpd_is_reader = {
	wavpack_input_read_bytes,
	wavpack_input_get_pos,
	wavpack_input_set_pos_abs,
	wavpack_input_set_pos_rel,
	wavpack_input_push_back_byte,
	wavpack_input_get_length,
	wavpack_input_can_seek,
	nullptr /* no need to write edited tags */
};

#endif

static InputStreamPtr
wavpack_open_wvc(DecoderClient &client, const char *uri)
{
	/*
	 * As we use dc->utf8url, this function will be bad for
	 * single files. utf8url is not absolute file path :/
	 */
	if (uri == nullptr)
		return nullptr;

	char *wvc_url = xstrcatdup(uri, "c");
	AtScopeExit(wvc_url) {
		free(wvc_url);
	};

	try {
		return client.OpenUri(uri);
	} catch (...) {
		return nullptr;
	}
}

/*
 * Decodes a stream.
 */
static void
wavpack_streamdecode(DecoderClient &client, InputStream &is)
{
	int open_flags = OPEN_DSD_FLAG | OPEN_NORMALIZE;
	bool can_seek = is.IsSeekable();

	std::unique_ptr<WavpackInput> wvc;
	auto is_wvc = wavpack_open_wvc(client, is.GetURI());
	if (is_wvc) {
		open_flags |= OPEN_WVC;
		can_seek &= is_wvc->IsSeekable();

		wvc = std::make_unique<WavpackInput>(&client, *is_wvc);
	}

	if (!can_seek) {
		open_flags |= OPEN_STREAMING;
	}

	WavpackInput isp(&client, is);

	auto *wpc = WavpackOpenInput(&mpd_is_reader, &isp, wvc.get(),
				     open_flags, 0);
	AtScopeExit(wpc) {
		WavpackCloseFile(wpc);
	};

	wavpack_decode(client, wpc, can_seek);
}

/*
 * Decodes a file.
 */
static void
wavpack_filedecode(DecoderClient &client, Path path_fs)
{
	auto *wpc = WavpackOpenInput(path_fs,
				     OPEN_DSD_FLAG | OPEN_NORMALIZE | OPEN_WVC,
				     0);
	AtScopeExit(wpc) {
		WavpackCloseFile(wpc);
	};

	wavpack_decode(client, wpc, true);
}

static void
Scan(WavpackContext *wpc,TagHandler &handler) noexcept
{
	try {
		handler.OnAudioFormat(CheckAudioFormat(wpc));
	} catch (...) {
	}

	const auto duration = GetDuration(wpc);
	if (!duration.IsNegative())
		handler.OnDuration(SongTime(duration));
}

/*
 * Reads metainfo from the specified file.
 */
static bool
wavpack_scan_file(Path path_fs, TagHandler &handler) noexcept
{
	WavpackContext *wpc;

	try {
		wpc = WavpackOpenInput(path_fs, OPEN_DSD_FLAG, 0);
	} catch (...) {
		return false;
	}

	AtScopeExit(wpc) {
		WavpackCloseFile(wpc);
	};

	Scan(wpc, handler);
	return true;
}

static bool
wavpack_scan_stream(InputStream &is, TagHandler &handler)
{
	WavpackInput isp(nullptr, is);

	WavpackContext *wpc;
	try {
		wpc = WavpackOpenInput(&mpd_is_reader, &isp, nullptr,
					     OPEN_DSD_FLAG, 0);
	} catch (...) {
		return false;
	}

	AtScopeExit(wpc) {
		WavpackCloseFile(wpc);
	};

	Scan(wpc, handler);
	return true;
}

static char const *const wavpack_suffixes[] = {
	"wv",
	nullptr
};

static char const *const wavpack_mime_types[] = {
	"audio/x-wavpack",
	nullptr
};

constexpr DecoderPlugin wavpack_decoder_plugin =
	DecoderPlugin("wavpack", wavpack_streamdecode, wavpack_scan_stream,
		      wavpack_filedecode, wavpack_scan_file)
	.WithSuffixes(wavpack_suffixes)
	.WithMimeTypes(wavpack_mime_types);
