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

#include "AudiofileDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "util/ScopeExit.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <audiofile.h>
#include <af_vfs.h>

#include <cassert>
#include <exception>

#include <stdio.h>
#include <stdlib.h>

static constexpr Domain audiofile_domain("audiofile");

static void
audiofile_error_func(long, const char *msg) noexcept
{
	LogWarning(audiofile_domain, msg);
}

static bool
audiofile_init(const ConfigBlock &)
{
	afSetErrorHandler(audiofile_error_func);
	return true;
}

struct AudioFileInputStream {
	DecoderClient *const client;
	InputStream &is;

	size_t Read(void *buffer, size_t size) noexcept {
		/* libaudiofile does not like partial reads at all,
		   and will abort playback; therefore always force full
		   reads */
		return decoder_read_full(client, is, buffer, size)
			? size
			: 0;
	}
};

gcc_pure
static SongTime
audiofile_get_duration(AFfilehandle fh) noexcept
{
	return SongTime::FromScale<uint64_t>(afGetFrameCount(fh, AF_DEFAULT_TRACK),
					     afGetRate(fh, AF_DEFAULT_TRACK));
}

static ssize_t
audiofile_file_read(AFvirtualfile *vfile, void *data, size_t length) noexcept
{
	AudioFileInputStream &afis = *static_cast<AudioFileInputStream *>(vfile->closure);

	return afis.Read(data, length);
}

static AFfileoffset
audiofile_file_length(AFvirtualfile *vfile) noexcept
{
	AudioFileInputStream &afis = *static_cast<AudioFileInputStream *>(vfile->closure);
	InputStream &is = afis.is;

	return is.GetSize();
}

static AFfileoffset
audiofile_file_tell(AFvirtualfile *vfile) noexcept
{
	AudioFileInputStream &afis = *static_cast<AudioFileInputStream *>(vfile->closure);
	InputStream &is = afis.is;

	return is.GetOffset();
}

static void
audiofile_file_destroy(AFvirtualfile *vfile) noexcept
{
	assert(vfile->closure != nullptr);

	vfile->closure = nullptr;
}

static AFfileoffset
audiofile_file_seek(AFvirtualfile *vfile, AFfileoffset _offset,
		    int is_relative) noexcept
{
	AudioFileInputStream &afis = *static_cast<AudioFileInputStream *>(vfile->closure);
	InputStream &is = afis.is;

	offset_type offset = _offset;
	if (is_relative)
		offset += is.GetOffset();

	try {
		is.LockSeek(offset);
		return is.GetOffset();
	} catch (...) {
		LogError(std::current_exception(), "Seek failed");
		return -1;
	}
}

static AFvirtualfile *
setup_virtual_fops(AudioFileInputStream &afis) noexcept
{
	auto vf = static_cast<AFvirtualfile *>(malloc(sizeof(AFvirtualfile)));
	vf->closure = &afis;
	vf->write = nullptr;
	vf->read    = audiofile_file_read;
	vf->length  = audiofile_file_length;
	vf->destroy = audiofile_file_destroy;
	vf->seek    = audiofile_file_seek;
	vf->tell    = audiofile_file_tell;
	return vf;
}

gcc_const
static SampleFormat
audiofile_bits_to_sample_format(int bits) noexcept
{
	switch (bits) {
	case 8:
		return SampleFormat::S8;

	case 16:
		return SampleFormat::S16;

	case 24:
		return SampleFormat::S24_P32;

	case 32:
		return SampleFormat::S32;
	}

	return SampleFormat::UNDEFINED;
}

static SampleFormat
audiofile_setup_sample_format(AFfilehandle af_fp) noexcept
{
	int fs, bits;

	afGetSampleFormat(af_fp, AF_DEFAULT_TRACK, &fs, &bits);
	if (!audio_valid_sample_format(audiofile_bits_to_sample_format(bits))) {
		FormatDebug(audiofile_domain,
			    "input file has %d bit samples, converting to 16",
			    bits);
		bits = 16;
	}

	afSetVirtualSampleFormat(af_fp, AF_DEFAULT_TRACK,
				 AF_SAMPFMT_TWOSCOMP, bits);
	afGetVirtualSampleFormat(af_fp, AF_DEFAULT_TRACK, &fs, &bits);

	return audiofile_bits_to_sample_format(bits);
}

static AudioFormat
CheckAudioFormat(AFfilehandle fh)
{
	return CheckAudioFormat(afGetRate(fh, AF_DEFAULT_TRACK),
				audiofile_setup_sample_format(fh),
				afGetVirtualChannels(fh, AF_DEFAULT_TRACK));
}

static void
audiofile_stream_decode(DecoderClient &client, InputStream &is)
{
	if (!is.IsSeekable() || !is.KnownSize()) {
		LogWarning(audiofile_domain, "not seekable");
		return;
	}

	AudioFileInputStream afis{&client, is};
	AFvirtualfile *const vf = setup_virtual_fops(afis);

	auto fh = afOpenVirtualFile(vf, "r", nullptr);
	if (fh == AF_NULL_FILEHANDLE)
		return;

	AtScopeExit(fh) { afCloseFile(fh); };

	const auto audio_format = CheckAudioFormat(fh);
	const auto total_time = audiofile_get_duration(fh);

	const auto kbit_rate = uint16_t(is.GetSize() * uint64_t(8) / total_time.ToMS());

	const auto frame_size = unsigned(afGetVirtualFrameSize(fh, AF_DEFAULT_TRACK, true));

	client.Ready(audio_format, true, total_time);

	DecoderCommand cmd;
	do {
		uint8_t chunk[8192];
		const int nframes =
			afReadFrames(fh, AF_DEFAULT_TRACK, chunk,
				     sizeof(chunk) / frame_size);
		if (nframes <= 0)
			break;

		cmd = client.SubmitData(nullptr,
					chunk, nframes * frame_size,
					kbit_rate);

		if (cmd == DecoderCommand::SEEK) {
			AFframecount frame = client.GetSeekFrame();
			afSeekFrame(fh, AF_DEFAULT_TRACK, frame);

			client.CommandFinished();
			cmd = DecoderCommand::NONE;
		}
	} while (cmd == DecoderCommand::NONE);
}

static bool
audiofile_scan_stream(InputStream &is, TagHandler &handler)
{
	if (!is.IsSeekable() || !is.KnownSize())
		return false;

	AudioFileInputStream afis{nullptr, is};
	AFvirtualfile *vf = setup_virtual_fops(afis);
	AFfilehandle fh = afOpenVirtualFile(vf, "r", nullptr);
	if (fh == AF_NULL_FILEHANDLE)
		return false;

	AtScopeExit(fh) { afCloseFile(fh); };

	handler.OnDuration(audiofile_get_duration(fh));

	try {
		handler.OnAudioFormat(CheckAudioFormat(fh));
	} catch (...) {
	}

	return true;
}

static const char *const audiofile_suffixes[] = {
	"wav", "au", "aiff", "aif", nullptr
};

static const char *const audiofile_mime_types[] = {
	"audio/wav",
	"audio/aiff",
	"audio/x-wav",
	"audio/x-aiff",
	nullptr
};

constexpr DecoderPlugin audiofile_decoder_plugin =
	DecoderPlugin("audiofile",
		      audiofile_stream_decode, audiofile_scan_stream)
	.WithInit(audiofile_init)
	.WithSuffixes(audiofile_suffixes)
	.WithMimeTypes(audiofile_mime_types);
