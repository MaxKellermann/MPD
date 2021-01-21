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

#include "SndfileDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "util/Domain.hxx"
#include "util/ScopeExit.hxx"
#include "util/StringView.hxx"
#include "Log.hxx"

#include <exception>

#include <sndfile.h>

static constexpr Domain sndfile_domain("sndfile");

static bool
sndfile_init([[maybe_unused]] const ConfigBlock &block)
{
       LogDebug(sndfile_domain, sf_version_string());
       return true;
}

struct SndfileInputStream {
	DecoderClient *const client;
	InputStream &is;

	size_t Read(void *buffer, size_t size) {
		/* libsndfile chokes on partial reads; therefore
		   always force full reads */
		return decoder_read_much(client, is, buffer, size);
	}
};

static sf_count_t
sndfile_vio_get_filelen(void *user_data)
{
	const auto &sis = *static_cast<SndfileInputStream *>(user_data);
	const auto &is = sis.is;

	if (!is.KnownSize())
		return -1;

	return is.GetSize();
}

static sf_count_t
sndfile_vio_seek(sf_count_t _offset, int whence, void *user_data)
{
	SndfileInputStream &sis = *static_cast<SndfileInputStream *>(user_data);
	InputStream &is = sis.is;

	offset_type offset = _offset;
	switch (whence) {
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

	try {
		is.LockSeek(offset);
		return is.GetOffset();
	} catch (...) {
		LogError(std::current_exception(), "Seek failed");
		return -1;
	}
}

static sf_count_t
sndfile_vio_read(void *ptr, sf_count_t count, void *user_data)
{
	SndfileInputStream &sis = *static_cast<SndfileInputStream *>(user_data);

	return sis.Read(ptr, count);
}

static sf_count_t
sndfile_vio_write([[maybe_unused]] const void *ptr,
		  [[maybe_unused]] sf_count_t count,
		  [[maybe_unused]] void *user_data)
{
	/* no writing! */
	return -1;
}

static sf_count_t
sndfile_vio_tell(void *user_data)
{
	const auto &sis = *static_cast<SndfileInputStream *>(user_data);
	const auto &is = sis.is;

	return is.GetOffset();
}

/**
 * This SF_VIRTUAL_IO implementation wraps MPD's #InputStream to a
 * libsndfile stream.
 */
static constexpr SF_VIRTUAL_IO vio = {
	sndfile_vio_get_filelen,
	sndfile_vio_seek,
	sndfile_vio_read,
	sndfile_vio_write,
	sndfile_vio_tell,
};

/**
 * Converts a frame number to a timestamp (in seconds).
 */
static constexpr SongTime
sndfile_duration(const SF_INFO &info)
{
	return SongTime::FromScale<uint64_t>(info.frames, info.samplerate);
}

gcc_pure
static SampleFormat
sndfile_sample_format(const SF_INFO &info) noexcept
{
	switch (info.format & SF_FORMAT_SUBMASK) {
	case SF_FORMAT_PCM_S8:
	case SF_FORMAT_PCM_U8:
	case SF_FORMAT_PCM_16:
		return SampleFormat::S16;

	case SF_FORMAT_FLOAT:
	case SF_FORMAT_DOUBLE:
		return SampleFormat::FLOAT;

	default:
		return SampleFormat::S32;
	}
}

static AudioFormat
CheckAudioFormat(const SF_INFO &info)
{
	return CheckAudioFormat(info.samplerate,
				sndfile_sample_format(info),
				info.channels);
}

static sf_count_t
sndfile_read_frames(SNDFILE *sf, SampleFormat format,
		    void *buffer, sf_count_t n_frames)
{
	switch (format) {
	case SampleFormat::S16:
		return sf_readf_short(sf, static_cast<short *>(buffer), n_frames);

	case SampleFormat::S32:
		return sf_readf_int(sf, static_cast<int *>(buffer), n_frames);

	case SampleFormat::FLOAT:
		return sf_readf_float(sf, static_cast<float *>(buffer), n_frames);

	default:
		assert(false);
		gcc_unreachable();
	}
}

static void
sndfile_stream_decode(DecoderClient &client, InputStream &is)
{
	SF_INFO info;

	info.format = 0;

	SndfileInputStream sis{&client, is};
	SNDFILE *const sf = sf_open_virtual(const_cast<SF_VIRTUAL_IO *>(&vio),
					    SFM_READ, &info, &sis);
	if (sf == nullptr) {
		FormatWarning(sndfile_domain, "sf_open_virtual() failed: %s",
			      sf_strerror(nullptr));
		return;
	}

	AtScopeExit(sf) { sf_close(sf); };

	const auto audio_format = CheckAudioFormat(info);

	client.Ready(audio_format, info.seekable, sndfile_duration(info));

	char buffer[16384];

	const size_t frame_size = audio_format.GetFrameSize();
	const sf_count_t read_frames = sizeof(buffer) / frame_size;

	DecoderCommand cmd;
	do {
		sf_count_t num_frames =
			sndfile_read_frames(sf,
					    audio_format.format,
					    buffer, read_frames);
		if (num_frames <= 0)
			break;

		cmd = client.SubmitData(is,
					buffer, num_frames * frame_size,
					0);
		if (cmd == DecoderCommand::SEEK) {
			sf_count_t c = client.GetSeekFrame();
			c = sf_seek(sf, c, SEEK_SET);
			if (c < 0)
				client.SeekError();
			else
				client.CommandFinished();
			cmd = DecoderCommand::NONE;
		}
	} while (cmd == DecoderCommand::NONE);
}

static void
sndfile_handle_tag(SNDFILE *sf, int str, TagType tag,
		   TagHandler &handler) noexcept
{
	const char *value = sf_get_string(sf, str);
	if (value != nullptr)
		handler.OnTag(tag, value);
}

static constexpr struct {
	int8_t str;
	TagType tag;
} sndfile_tags[] = {
	{ SF_STR_TITLE, TAG_TITLE },
	{ SF_STR_ARTIST, TAG_ARTIST },
	{ SF_STR_COMMENT, TAG_COMMENT },
	{ SF_STR_DATE, TAG_DATE },
	{ SF_STR_ALBUM, TAG_ALBUM },
	{ SF_STR_TRACKNUMBER, TAG_TRACK },
	{ SF_STR_GENRE, TAG_GENRE },
};

static bool
sndfile_scan_stream(InputStream &is, TagHandler &handler)
{
	SF_INFO info;

	info.format = 0;

	SndfileInputStream sis{nullptr, is};
	SNDFILE *const sf = sf_open_virtual(const_cast<SF_VIRTUAL_IO *>(&vio),
					    SFM_READ, &info, &sis);
	if (sf == nullptr)
		return false;

	AtScopeExit(sf) { sf_close(sf); };

	if (!audio_valid_sample_rate(info.samplerate)) {
		FormatWarning(sndfile_domain,
			      "Invalid sample rate in %s", is.GetURI());
		return false;
	}

	try {
		handler.OnAudioFormat(CheckAudioFormat(info));
	} catch (...) {
	}

	handler.OnDuration(sndfile_duration(info));

	for (auto i : sndfile_tags)
		sndfile_handle_tag(sf, i.str, i.tag, handler);

	return true;
}

static const char *const sndfile_suffixes[] = {
	"wav", "aiff", "aif", /* Microsoft / SGI / Apple */
	"au", "snd", /* Sun / DEC / NeXT */
	"paf", /* Paris Audio File */
	"iff", "svx", /* Commodore Amiga IFF / SVX */
	"sf", /* IRCAM */
	"voc", /* Creative */
	"w64", /* Soundforge */
	"pvf", /* Portable Voice Format */
	"xi", /* Fasttracker */
	"htk", /* HMM Tool Kit */
	"caf", /* Apple */
	"sd2", /* Sound Designer II */

	/* libsndfile also supports FLAC and Ogg Vorbis, but only by
	   linking with libFLAC and libvorbis - we can do better, we
	   have native plugins for these libraries */

	nullptr
};

static const char *const sndfile_mime_types[] = {
	"audio/wav",
	"audio/aiff",
	"audio/x-wav",
	"audio/x-aiff",

	/* what are the MIME types of the other supported formats? */

	nullptr
};

constexpr DecoderPlugin sndfile_decoder_plugin =
	DecoderPlugin("sndfile", sndfile_stream_decode, sndfile_scan_stream)
	.WithInit(sndfile_init)
	.WithSuffixes(sndfile_suffixes)
	.WithMimeTypes(sndfile_mime_types);
