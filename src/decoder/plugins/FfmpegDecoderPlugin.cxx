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

/* necessary because libavutil/common.h uses UINT64_C */
#define __STDC_CONSTANT_MACROS

#include "lib/ffmpeg/Time.hxx"
#include "FfmpegDecoderPlugin.hxx"
#include "lib/ffmpeg/Domain.hxx"
#include "lib/ffmpeg/Error.hxx"
#include "lib/ffmpeg/Init.hxx"
#include "lib/ffmpeg/Interleave.hxx"
#include "lib/ffmpeg/Buffer.hxx"
#include "lib/ffmpeg/Frame.hxx"
#include "lib/ffmpeg/Format.hxx"
#include "lib/ffmpeg/Codec.hxx"
#include "lib/ffmpeg/SampleFormat.hxx"
#include "../DecoderAPI.hxx"
#include "FfmpegMetaData.hxx"
#include "FfmpegIo.hxx"
#include "pcm/Interleave.hxx"
#include "tag/Builder.hxx"
#include "tag/Handler.hxx"
#include "tag/ReplayGain.hxx"
#include "tag/MixRamp.hxx"
#include "input/InputStream.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "util/ScopeExit.hxx"
#include "util/ConstBuffer.hxx"
#include "util/StringAPI.hxx"
#include "LogV.hxx"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

#include <cassert>

#include <string.h>

/**
 * Muxer options to be passed to avformat_open_input().
 */
static AVDictionary *avformat_options = nullptr;

static Ffmpeg::FormatContext
FfmpegOpenInput(AVIOContext *pb,
		const char *filename,
		AVInputFormat *fmt)
{
	Ffmpeg::FormatContext context(pb);

	AVDictionary *options = nullptr;
	AtScopeExit(&options) { av_dict_free(&options); };
	av_dict_copy(&options, avformat_options, 0);

	context.OpenInput(filename, fmt, &options);

	return context;
}

static bool
ffmpeg_init(const ConfigBlock &block)
{
	FfmpegInit();

	static constexpr const char *option_names[] = {
		"probesize",
		"analyzeduration",
	};

	for (const char *name : option_names) {
		const char *value = block.GetBlockValue(name);
		if (value != nullptr)
			av_dict_set(&avformat_options, name, value, 0);
	}

	return true;
}

static void
ffmpeg_finish() noexcept
{
	av_dict_free(&avformat_options);
}

gcc_pure
static bool
IsAudio(const AVStream &stream) noexcept
{
	return stream.codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
}

gcc_pure
static int
ffmpeg_find_audio_stream(const AVFormatContext &format_context) noexcept
{
	for (unsigned i = 0; i < format_context.nb_streams; ++i)
		if (IsAudio(*format_context.streams[i]))
			return i;

	return -1;
}

gcc_pure
static bool
IsPicture(const AVStream &stream) noexcept
{
	return stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
		(stream.disposition & AV_DISPOSITION_ATTACHED_PIC) != 0 &&
		stream.attached_pic.size > 0;
}

static const AVStream *
FindPictureStream(const AVFormatContext &format_context) noexcept
{
	for (unsigned i = 0; i < format_context.nb_streams; ++i)
		if (IsPicture(*format_context.streams[i]))
			return format_context.streams[i];

	return nullptr;
}

static const char *
GetMimeType(const AVCodecDescriptor &codec) noexcept
{
	return codec.mime_types != nullptr
		? *codec.mime_types
		: nullptr;
}

static const char *
GetMimeType(const AVStream &stream) noexcept
{
	const auto *codec = avcodec_descriptor_get(stream.codecpar->codec_id);
	if (codec != nullptr)
		return GetMimeType(*codec);

	return nullptr;
}

static ConstBuffer<void>
ToConstBuffer(const AVPacket &packet) noexcept
{
	return {packet.data, size_t(packet.size)};
}

/**
 * Accessor for AVStream::start_time that replaces AV_NOPTS_VALUE with
 * zero.  We can't use AV_NOPTS_VALUE in calculations, and we simply
 * assume that the stream's start time is zero, which appears to be
 * the best way out of that situation.
 */
static constexpr int64_t
start_time_fallback(const AVStream &stream)
{
	return FfmpegTimestampFallback(stream.start_time, 0);
}

/**
 * Convert AVPacket::pts to a stream-relative time stamp (still in
 * AVStream::time_base units).  Returns a negative value on error.
 */
gcc_pure
static int64_t
StreamRelativePts(const AVPacket &packet, const AVStream &stream) noexcept
{
	auto pts = packet.pts;
	if (pts < 0 || pts == int64_t(AV_NOPTS_VALUE))
		return -1;

	auto start = start_time_fallback(stream);
	return pts - start;
}

/**
 * Convert a non-negative stream-relative time stamp in
 * AVStream::time_base units to a PCM frame number.
 */
gcc_pure
static uint64_t
PtsToPcmFrame(uint64_t pts, const AVStream &stream,
	      const AVCodecContext &codec_context) noexcept
{
	return av_rescale_q(pts, stream.time_base, codec_context.time_base);
}

/**
 * Invoke DecoderClient::SubmitData() with the contents of an
 * #AVFrame.
 */
static DecoderCommand
FfmpegSendFrame(DecoderClient &client, InputStream *is,
		AVCodecContext &codec_context,
		const AVFrame &frame,
		size_t &skip_bytes,
		FfmpegBuffer &buffer)
{
	ConstBuffer<void> output_buffer =
		Ffmpeg::InterleaveFrame(frame, buffer);

	if (skip_bytes > 0) {
		if (skip_bytes >= output_buffer.size) {
			skip_bytes -= output_buffer.size;
			return DecoderCommand::NONE;
		}

		output_buffer.data =
			static_cast<const uint8_t *>(output_buffer.data) + skip_bytes;
		output_buffer.size -= skip_bytes;
		skip_bytes = 0;
	}

	return client.SubmitData(is,
				 output_buffer.data, output_buffer.size,
				 codec_context.bit_rate / 1000);
}

static DecoderCommand
FfmpegReceiveFrames(DecoderClient &client, InputStream *is,
		    AVCodecContext &codec_context,
		    AVFrame &frame,
		    size_t &skip_bytes,
		    FfmpegBuffer &buffer,
		    bool &eof)
{
	while (true) {
		DecoderCommand cmd;

		int err = avcodec_receive_frame(&codec_context, &frame);
		switch (err) {
		case 0:
			cmd = FfmpegSendFrame(client, is, codec_context,
					      frame, skip_bytes,
					      buffer);
			if (cmd != DecoderCommand::NONE)
				return cmd;

			break;

		case AVERROR_EOF:
			eof = true;
			return DecoderCommand::NONE;

		case AVERROR(EAGAIN):
			/* need to call avcodec_send_packet() */
			return DecoderCommand::NONE;

		default:
			{
				char msg[256];
				av_strerror(err, msg, sizeof(msg));
				FormatWarning(ffmpeg_domain,
					      "avcodec_send_packet() failed: %s",
					      msg);
			}

			return DecoderCommand::STOP;
		}
	}
}

/**
 * Decode an #AVPacket and send the resulting PCM data to the decoder
 * API.
 *
 * @param min_frame skip all data before this PCM frame number; this
 * is used after seeking to skip data in an AVPacket until the exact
 * desired time stamp has been reached
 */
static DecoderCommand
ffmpeg_send_packet(DecoderClient &client, InputStream *is,
		   const AVPacket &packet,
		   AVCodecContext &codec_context,
		   const AVStream &stream,
		   AVFrame &frame,
		   uint64_t min_frame, size_t pcm_frame_size,
		   FfmpegBuffer &buffer)
{
	size_t skip_bytes = 0;

	const auto pts = StreamRelativePts(packet, stream);
	if (pts >= 0) {
		if (min_frame > 0) {
			auto cur_frame = PtsToPcmFrame(pts, stream,
						       codec_context);
			if (cur_frame < min_frame)
				skip_bytes = pcm_frame_size * (min_frame - cur_frame);
		} else
			client.SubmitTimestamp(FfmpegTimeToDouble(pts,
								  stream.time_base));
	}

	bool eof = false;

	int err = avcodec_send_packet(&codec_context, &packet);
	switch (err) {
	case 0:
		break;

	case AVERROR_EOF:
		eof = true;
		break;

	default:
		{
			char msg[256];
			av_strerror(err, msg, sizeof(msg));
			FormatWarning(ffmpeg_domain,
				      "avcodec_send_packet() failed: %s", msg);
		}

		return DecoderCommand::NONE;
	}

	auto cmd = FfmpegReceiveFrames(client, is, codec_context,
				       frame,
				       skip_bytes, buffer, eof);

	if (eof)
		cmd = DecoderCommand::STOP;

	return cmd;
}

gcc_const
static SampleFormat
ffmpeg_sample_format(enum AVSampleFormat sample_fmt) noexcept
{
	const auto result = Ffmpeg::FromFfmpegSampleFormat(sample_fmt);
	if (result != SampleFormat::UNDEFINED)
		return result;

	char buffer[64];
	const char *name = av_get_sample_fmt_string(buffer, sizeof(buffer),
						    sample_fmt);
	if (name != nullptr)
		FormatError(ffmpeg_domain,
			    "Unsupported libavcodec SampleFormat value: %s (%d)",
			    name, sample_fmt);
	else
		FormatError(ffmpeg_domain,
			    "Unsupported libavcodec SampleFormat value: %d",
			    sample_fmt);
	return SampleFormat::UNDEFINED;
}

static void
FfmpegParseMetaData(AVDictionary &dict, ReplayGainInfo &rg, MixRampInfo &mr)
{
	AVDictionaryEntry *i = nullptr;

	while ((i = av_dict_get(&dict, "", i,
				AV_DICT_IGNORE_SUFFIX)) != nullptr) {
		const char *name = i->key;
		const char *value = i->value;

		if (!ParseReplayGainTag(rg, name, value))
			ParseMixRampTag(mr, name, value);
	}
}

static void
FfmpegParseMetaData(const AVStream &stream,
		    ReplayGainInfo &rg, MixRampInfo &mr)
{
	FfmpegParseMetaData(*stream.metadata, rg, mr);
}

static void
FfmpegParseMetaData(const AVFormatContext &format_context, int audio_stream,
		    ReplayGainInfo &rg, MixRampInfo &mr)
{
	assert(audio_stream >= 0);

	FfmpegParseMetaData(*format_context.metadata, rg, mr);
	FfmpegParseMetaData(*format_context.streams[audio_stream],
				    rg, mr);
}

static void
FfmpegParseMetaData(DecoderClient &client,
		    const AVFormatContext &format_context, int audio_stream)
{
	ReplayGainInfo rg;
	rg.Clear();

	MixRampInfo mr;
	mr.Clear();

	FfmpegParseMetaData(format_context, audio_stream, rg, mr);

	if (rg.IsDefined())
		client.SubmitReplayGain(&rg);

	if (mr.IsDefined())
		client.SubmitMixRamp(std::move(mr));
}

static void
FfmpegScanMetadata(const AVStream &stream, TagHandler &handler) noexcept
{
	FfmpegScanDictionary(stream.metadata, handler);
}

static void
FfmpegScanMetadata(const AVFormatContext &format_context, int audio_stream,
		   TagHandler &handler) noexcept
{
	assert(audio_stream >= 0);

	FfmpegScanDictionary(format_context.metadata, handler);
	FfmpegScanMetadata(*format_context.streams[audio_stream],
			   handler);
}

static void
FfmpegScanTag(const AVFormatContext &format_context, int audio_stream,
	      TagBuilder &tag)
{
	FullTagHandler h(tag);
	FfmpegScanMetadata(format_context, audio_stream, h);
}

/**
 * Check if a new stream tag was received and pass it to
 * DecoderClient::SubmitTag().
 */
static void
FfmpegCheckTag(DecoderClient &client, InputStream *is,
	       AVFormatContext &format_context, int audio_stream)
{
	AVStream &stream = *format_context.streams[audio_stream];
	if ((stream.event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED) == 0)
		/* no new metadata */
		return;

	/* clear the flag */
	stream.event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;

	TagBuilder tag;
	FfmpegScanTag(format_context, audio_stream, tag);
	if (!tag.empty())
		client.SubmitTag(is, tag.Commit());
}

static void
FfmpegDecode(DecoderClient &client, InputStream *input,
	     AVFormatContext &format_context)
{
	const int find_result =
		avformat_find_stream_info(&format_context, nullptr);
	if (find_result < 0) {
		LogError(ffmpeg_domain, "Couldn't find stream info");
		return;
	}

	int audio_stream = ffmpeg_find_audio_stream(format_context);
	if (audio_stream == -1) {
		LogError(ffmpeg_domain, "No audio stream inside");
		return;
	}

	AVStream &av_stream = *format_context.streams[audio_stream];

	const auto &codec_params = *av_stream.codecpar;

	const AVCodecDescriptor *codec_descriptor =
		avcodec_descriptor_get(codec_params.codec_id);
	if (codec_descriptor != nullptr)
		FormatDebug(ffmpeg_domain, "codec '%s'",
			    codec_descriptor->name);

	AVCodec *codec = avcodec_find_decoder(codec_params.codec_id);

	if (!codec) {
		LogError(ffmpeg_domain, "Unsupported audio codec");
		return;
	}

	Ffmpeg::CodecContext codec_context(*codec);
	codec_context.FillFromParameters(*av_stream.codecpar);
	codec_context.Open(*codec, nullptr);

	const SampleFormat sample_format =
		ffmpeg_sample_format(codec_context->sample_fmt);
	if (sample_format == SampleFormat::UNDEFINED) {
		// (error message already done by ffmpeg_sample_format())
		return;
	}

	const auto audio_format = CheckAudioFormat(codec_context->sample_rate,
						   sample_format,
						   codec_context->channels);

	const SignedSongTime total_time =
		av_stream.duration != int64_t(AV_NOPTS_VALUE)
		? FromFfmpegTimeChecked(av_stream.duration, av_stream.time_base)
		: FromFfmpegTimeChecked(format_context.duration, AV_TIME_BASE_Q);

	client.Ready(audio_format,
		     input
		     ? input->IsSeekable()
		     : (format_context.ctx_flags & AVFMTCTX_UNSEEKABLE) != 0,
		     total_time);

	FfmpegParseMetaData(client, format_context, audio_stream);

	Ffmpeg::Frame frame;

	FfmpegBuffer interleaved_buffer;

	uint64_t min_frame = 0;

	DecoderCommand cmd = client.GetCommand();
	while (cmd != DecoderCommand::STOP) {
		if (cmd == DecoderCommand::SEEK) {
			int64_t where =
				ToFfmpegTime(client.GetSeekTime(),
					     av_stream.time_base) +
				start_time_fallback(av_stream);

			/* AVSEEK_FLAG_BACKWARD asks FFmpeg to seek to
			   the packet boundary before the seek time
			   stamp, not after */
			if (av_seek_frame(&format_context, audio_stream, where,
					  AVSEEK_FLAG_ANY|AVSEEK_FLAG_BACKWARD) < 0)
				client.SeekError();
			else {
				codec_context.FlushBuffers();
				min_frame = client.GetSeekFrame();
				client.CommandFinished();
			}
		}

		AVPacket packet;
		if (av_read_frame(&format_context, &packet) < 0)
			/* end of file */
			break;

		AtScopeExit(&packet) {
			av_packet_unref(&packet);
		};

		FfmpegCheckTag(client, input, format_context, audio_stream);

		if (packet.size > 0 && packet.stream_index == audio_stream) {
			cmd = ffmpeg_send_packet(client, input,
						 packet,
						 *codec_context,
						 av_stream,
						 *frame,
						 min_frame, audio_format.GetFrameSize(),
						 interleaved_buffer);
			min_frame = 0;
		} else
			cmd = client.GetCommand();
	}
}

static void
ffmpeg_decode(DecoderClient &client, InputStream &input)
{
	AvioStream stream(&client, input);
	if (!stream.Open()) {
		LogError(ffmpeg_domain, "Failed to open stream");
		return;
	}

	auto format_context =
		FfmpegOpenInput(stream.io, input.GetURI(), nullptr);

	const auto *input_format = format_context->iformat;
	FormatDebug(ffmpeg_domain, "detected input format '%s' (%s)",
		    input_format->name, input_format->long_name);

	FfmpegDecode(client, &input, *format_context);
}

static bool
FfmpegScanStream(AVFormatContext &format_context, TagHandler &handler)
{
	const int find_result =
		avformat_find_stream_info(&format_context, nullptr);
	if (find_result < 0)
		return false;

	const int audio_stream = ffmpeg_find_audio_stream(format_context);
	if (audio_stream < 0)
		return false;

	const AVStream &stream = *format_context.streams[audio_stream];
	if (stream.duration != int64_t(AV_NOPTS_VALUE))
		handler.OnDuration(FromFfmpegTime(stream.duration,
						  stream.time_base));
	else if (format_context.duration != int64_t(AV_NOPTS_VALUE))
		handler.OnDuration(FromFfmpegTime(format_context.duration,
						  AV_TIME_BASE_Q));

	const auto &codec_params = *stream.codecpar;
	try {
		handler.OnAudioFormat(CheckAudioFormat(codec_params.sample_rate,
						       ffmpeg_sample_format(AVSampleFormat(codec_params.format)),
						       codec_params.channels));
	} catch (...) {
	}

	FfmpegScanMetadata(format_context, audio_stream, handler);

	if (handler.WantPicture()) {
		const auto *picture_stream = FindPictureStream(format_context);
		if (picture_stream != nullptr)
			handler.OnPicture(GetMimeType(*picture_stream),
					  ToConstBuffer(picture_stream->attached_pic));
	}

	return true;
}

static bool
ffmpeg_scan_stream(InputStream &is, TagHandler &handler)
{
	AvioStream stream(nullptr, is);
	if (!stream.Open())
		return false;

	auto f = FfmpegOpenInput(stream.io, is.GetURI(), nullptr);
	return FfmpegScanStream(*f, handler);
}

static void
ffmpeg_uri_decode(DecoderClient &client, const char *uri)
{
	auto format_context =
		FfmpegOpenInput(nullptr, uri, nullptr);

	const auto *input_format = format_context->iformat;
	FormatDebug(ffmpeg_domain, "detected input format '%s' (%s)",
		    input_format->name, input_format->long_name);

	FfmpegDecode(client, nullptr, *format_context);
}

static std::set<std::string>
ffmpeg_protocols() noexcept
{
	std::set<std::string> protocols;

	const AVInputFormat *format = nullptr;
	void *opaque = nullptr;
	while ((format = av_demuxer_iterate(&opaque)) != nullptr) {
		if (StringIsEqual(format->name, "rtsp")) {
			protocols.emplace("rtsp://");
			protocols.emplace("rtsps://");
		} else if (StringIsEqual(format->name, "rtp"))
			protocols.emplace("rtp://");
	}

	return protocols;
}

/**
 * A list of extensions found for the formats supported by ffmpeg.
 * This list is current as of 02-23-09; To find out if there are more
 * supported formats, check the ffmpeg changelog since this date for
 * more formats.
 */
static const char *const ffmpeg_suffixes[] = {
	"16sv", "3g2", "3gp", "4xm", "8svx",
	"aa3", "aac", "ac3", "adx", "afc", "aif",
	"aifc", "aiff", "al", "alaw", "amr", "anim", "apc", "ape", "asf",
	"atrac", "au", "aud", "avi", "avm2", "avs", "bap", "bfi", "c93", "cak",
	"cin", "cmv", "cpk", "daud", "dct", "divx", "dts", "dv", "dvd", "dxa",
	"eac3", "film", "flac", "flc", "fli", "fll", "flx", "flv", "g726",
	"gsm", "gxf", "iss", "m1v", "m2v", "m2t", "m2ts",
	"m4a", "m4b", "m4v",
	"mad",
	"mj2", "mjpeg", "mjpg", "mka", "mkv", "mlp", "mm", "mmf", "mov", "mp+",
	"mp1", "mp2", "mp3", "mp4", "mpc", "mpeg", "mpg", "mpga", "mpp", "mpu",
	"mve", "mvi", "mxf", "nc", "nsv", "nut", "nuv", "oga", "ogm", "ogv",
	"ogx", "oma", "ogg", "omg", "opus", "psp", "pva", "qcp", "qt", "r3d", "ra",
	"ram", "rl2", "rm", "rmvb", "roq", "rpl", "rvc", "shn", "smk", "snd",
	"sol", "son", "spx", "str", "swf", "tak", "tgi", "tgq", "tgv", "thp", "ts",
	"tsp", "tta", "xa", "xvid", "uv", "uv2", "vb", "vid", "vob", "voc",
	"vp6", "vmd", "wav", "webm", "wma", "wmv", "wsaud", "wsvga", "wv",
	"wve",
	nullptr
};

static const char *const ffmpeg_mime_types[] = {
	"application/flv",
	"application/m4a",
	"application/mp4",
	"application/octet-stream",
	"application/ogg",
	"application/x-ms-wmz",
	"application/x-ms-wmd",
	"application/x-ogg",
	"application/x-shockwave-flash",
	"application/x-shorten",
	"audio/8svx",
	"audio/16sv",
	"audio/aac",
	"audio/aacp",
	"audio/ac3",
	"audio/aiff",
	"audio/amr",
	"audio/basic",
	"audio/flac",
	"audio/m4a",
	"audio/mp4",
	"audio/mpeg",
	"audio/musepack",
	"audio/ogg",
	"audio/opus",
	"audio/qcelp",
	"audio/vorbis",
	"audio/vorbis+ogg",
	"audio/wav",
	"audio/x-8svx",
	"audio/x-16sv",
	"audio/x-aac",
	"audio/x-ac3",
	"audio/x-adx",
	"audio/x-aiff",
	"audio/x-alaw",
	"audio/x-au",
	"audio/x-dca",
	"audio/x-eac3",
	"audio/x-flac",
	"audio/x-gsm",
	"audio/x-mace",
	"audio/x-matroska",
	"audio/x-monkeys-audio",
	"audio/x-mpeg",
	"audio/x-ms-wma",
	"audio/x-ms-wax",
	"audio/x-musepack",
	"audio/x-ogg",
	"audio/x-vorbis",
	"audio/x-vorbis+ogg",
	"audio/x-pn-realaudio",
	"audio/x-pn-multirate-realaudio",
	"audio/x-speex",
	"audio/x-tta",
	"audio/x-voc",
	"audio/x-wav",
	"audio/x-wma",
	"audio/x-wv",
	"video/anim",
	"video/quicktime",
	"video/msvideo",
	"video/ogg",
	"video/theora",
	"video/webm",
	"video/x-dv",
	"video/x-flv",
	"video/x-matroska",
	"video/x-mjpeg",
	"video/x-mpeg",
	"video/x-ms-asf",
	"video/x-msvideo",
	"video/x-ms-wmv",
	"video/x-ms-wvx",
	"video/x-ms-wm",
	"video/x-ms-wmx",
	"video/x-nut",
	"video/x-pva",
	"video/x-theora",
	"video/x-vid",
	"video/x-wmv",
	"video/x-xvid",

	/* special value for the "ffmpeg" input plugin: all streams by
	   the "ffmpeg" input plugin shall be decoded by this
	   plugin */
	"audio/x-mpd-ffmpeg",

	nullptr
};

constexpr DecoderPlugin ffmpeg_decoder_plugin =
	DecoderPlugin("ffmpeg", ffmpeg_decode, ffmpeg_scan_stream)
	.WithInit(ffmpeg_init, ffmpeg_finish)
	.WithProtocols(ffmpeg_protocols, ffmpeg_uri_decode)
	.WithSuffixes(ffmpeg_suffixes)
	.WithMimeTypes(ffmpeg_mime_types);
