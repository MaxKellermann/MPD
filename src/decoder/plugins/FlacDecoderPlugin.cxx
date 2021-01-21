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

#include "FlacDecoderPlugin.h"
#include "FlacStreamDecoder.hxx"
#include "FlacDomain.hxx"
#include "FlacCommon.hxx"
#include "lib/xiph/FlacMetadataChain.hxx"
#include "OggCodec.hxx"
#include "input/InputStream.hxx"
#include "fs/Path.hxx"
#include "fs/NarrowPath.hxx"
#include "Log.hxx"

#if !defined(FLAC_API_VERSION_CURRENT) || FLAC_API_VERSION_CURRENT <= 7
#error libFLAC is too old
#endif

static void flacPrintErroredState(FLAC__StreamDecoderState state)
{
	switch (state) {
	case FLAC__STREAM_DECODER_SEARCH_FOR_METADATA:
	case FLAC__STREAM_DECODER_READ_METADATA:
	case FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC:
	case FLAC__STREAM_DECODER_READ_FRAME:
	case FLAC__STREAM_DECODER_END_OF_STREAM:
		return;

	case FLAC__STREAM_DECODER_OGG_ERROR:
	case FLAC__STREAM_DECODER_SEEK_ERROR:
	case FLAC__STREAM_DECODER_ABORTED:
	case FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR:
	case FLAC__STREAM_DECODER_UNINITIALIZED:
		break;
	}

	LogError(flac_domain, FLAC__StreamDecoderStateString[state]);
}

static void flacMetadata([[maybe_unused]] const FLAC__StreamDecoder * dec,
			 const FLAC__StreamMetadata * block, void *vdata)
{
	auto &fd = *static_cast<FlacDecoder *>(vdata);
	fd.OnMetadata(*block);
}

static FLAC__StreamDecoderWriteStatus
flac_write_cb(const FLAC__StreamDecoder *dec, const FLAC__Frame *frame,
	      const FLAC__int32 *const buf[], void *vdata)
{
	auto &fd = *static_cast<FlacDecoder *>(vdata);
	return fd.OnWrite(*frame, buf, fd.GetDeltaPosition(*dec));
}

static bool
flac_scan_file(Path path_fs, TagHandler &handler)
{
	FlacMetadataChain chain;
	if (!chain.Read(NarrowPath(path_fs))) {
		FormatDebug(flac_domain,
			    "Failed to read FLAC tags: %s",
			    chain.GetStatusString());
		return false;
	}

	chain.Scan(handler);
	return true;
}

static bool
flac_scan_stream(InputStream &is, TagHandler &handler)
{
	FlacMetadataChain chain;
	if (!chain.Read(is)) {
		FormatDebug(flac_domain,
			    "Failed to read FLAC tags: %s",
			    chain.GetStatusString());
		return false;
	}

	chain.Scan(handler);
	return true;
}

/**
 * Some glue code around FLAC__stream_decoder_new().
 */
static FlacStreamDecoder
flac_decoder_new()
{
	FlacStreamDecoder sd;
	if(!FLAC__stream_decoder_set_metadata_respond(sd.get(), FLAC__METADATA_TYPE_VORBIS_COMMENT))
		LogDebug(flac_domain,
			 "FLAC__stream_decoder_set_metadata_respond() has failed");

	return sd;
}

static bool
flac_decoder_initialize(FlacDecoder *data, FLAC__StreamDecoder *sd)
{
	if (!FLAC__stream_decoder_process_until_end_of_metadata(sd)) {
		if (FLAC__stream_decoder_get_state(sd) != FLAC__STREAM_DECODER_END_OF_STREAM)
			LogWarning(flac_domain, "problem reading metadata");
		return false;
	}

	if (data->initialized) {
		/* done */
		return true;
	}

	if (data->GetInputStream().IsSeekable())
		/* allow the workaround below only for nonseekable
		   streams*/
		return false;

	/* no stream_info packet found; try to initialize the decoder
	   from the first frame header */
	FLAC__stream_decoder_process_single(sd);
	return data->initialized;
}

static DecoderCommand
FlacSubmitToClient(DecoderClient &client, FlacDecoder &d) noexcept
{
	if (d.tag.IsEmpty() && d.chunk.empty())
		return client.GetCommand();

	if (!d.tag.IsEmpty()) {
		auto cmd = client.SubmitTag(d.GetInputStream(),
					    std::move(d.tag));
		d.tag.Clear();
		if (cmd != DecoderCommand::NONE)
			return cmd;
	}

	if (!d.chunk.empty()) {
		auto cmd = client.SubmitData(d.GetInputStream(),
					     d.chunk.data,
					     d.chunk.size,
					     d.kbit_rate);
		d.chunk = nullptr;
		if (cmd != DecoderCommand::NONE)
			return cmd;
	}

	return DecoderCommand::NONE;
}

static void
flac_decoder_loop(FlacDecoder *data, FLAC__StreamDecoder *flac_dec)
{
	DecoderClient &client = *data->GetClient();

	while (true) {
		DecoderCommand cmd = FlacSubmitToClient(client, *data);

		if (cmd == DecoderCommand::SEEK) {
			FLAC__uint64 seek_sample = client.GetSeekFrame();
			if (FLAC__stream_decoder_seek_absolute(flac_dec, seek_sample)) {
				data->position = 0;
				client.CommandFinished();
			} else
				client.SeekError();

			/* FLAC__stream_decoder_seek_absolute()
			   decodes one frame and may have provided
			   data to be submitted to the client */
			continue;
		} else if (cmd == DecoderCommand::STOP)
			break;

		switch (FLAC__stream_decoder_get_state(flac_dec)) {
		case FLAC__STREAM_DECODER_SEARCH_FOR_METADATA:
		case FLAC__STREAM_DECODER_READ_METADATA:
		case FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC:
		case FLAC__STREAM_DECODER_READ_FRAME:
			/* continue decoding */
			break;

		case FLAC__STREAM_DECODER_END_OF_STREAM:
			/* regular end of stream */
			return;

		case FLAC__STREAM_DECODER_SEEK_ERROR:
			/* try to recover from seek error */
			if (!FLAC__stream_decoder_flush(flac_dec)) {
				LogError(flac_domain, "FLAC__stream_decoder_flush() failed");
				return;
			}

			break;

		case FLAC__STREAM_DECODER_OGG_ERROR:
		case FLAC__STREAM_DECODER_ABORTED:
		case FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR:
			/* an error, fatal enough for us to abort the
			   decoder */
			return;

		case FLAC__STREAM_DECODER_UNINITIALIZED:
			/* we shouldn't see this, ever - bail out */
			return;
		}

		if (!FLAC__stream_decoder_process_single(flac_dec) &&
		    client.GetCommand() == DecoderCommand::NONE) {
			/* a failure that was not triggered by a
			   decoder command */
			flacPrintErroredState(FLAC__stream_decoder_get_state(flac_dec));
			break;
		}
	}
}

static FLAC__StreamDecoderInitStatus
stream_init_oggflac(FLAC__StreamDecoder *flac_dec, FlacDecoder *data)
{
	return FLAC__stream_decoder_init_ogg_stream(flac_dec,
						    FlacInput::Read,
						    FlacInput::Seek,
						    FlacInput::Tell,
						    FlacInput::Length,
						    FlacInput::Eof,
						    flac_write_cb,
						    flacMetadata,
						    FlacInput::Error,
						    data);
}

static FLAC__StreamDecoderInitStatus
stream_init_flac(FLAC__StreamDecoder *flac_dec, FlacDecoder *data)
{
	return FLAC__stream_decoder_init_stream(flac_dec,
						FlacInput::Read,
						FlacInput::Seek,
						FlacInput::Tell,
						FlacInput::Length,
						FlacInput::Eof,
						flac_write_cb,
						flacMetadata,
						FlacInput::Error,
						data);
}

static FLAC__StreamDecoderInitStatus
stream_init(FLAC__StreamDecoder *flac_dec, FlacDecoder *data, bool is_ogg)
{
	return is_ogg
		? stream_init_oggflac(flac_dec, data)
		: stream_init_flac(flac_dec, data);
}

static bool
FlacInitAndDecode(FlacDecoder &data, FLAC__StreamDecoder *sd, bool is_ogg)
{
	auto init_status = stream_init(sd, &data, is_ogg);
	if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		LogWarning(flac_domain,
			   FLAC__StreamDecoderInitStatusString[init_status]);
		return false;
	}

	bool result = flac_decoder_initialize(&data, sd);
	if (result)
		flac_decoder_loop(&data, sd);

	FLAC__stream_decoder_finish(sd);
	return result;
}

static void
flac_decode_internal(DecoderClient &client,
		     InputStream &input_stream,
		     bool is_ogg)
{
	auto flac_dec = flac_decoder_new();
	if (!flac_dec)
		return;

	FlacDecoder data(client, input_stream);

	FlacInitAndDecode(data, flac_dec.get(), is_ogg);
}

static void
flac_decode(DecoderClient &client, InputStream &input_stream)
{
	flac_decode_internal(client, input_stream, false);
}

static bool
oggflac_init([[maybe_unused]] const ConfigBlock &block)
{
	return !!FLAC_API_SUPPORTS_OGG_FLAC;
}

static bool
oggflac_scan_file(Path path_fs, TagHandler &handler)
{
	FlacMetadataChain chain;
	if (!chain.ReadOgg(NarrowPath(path_fs))) {
		FormatDebug(flac_domain,
			    "Failed to read OggFLAC tags: %s",
			    chain.GetStatusString());
		return false;
	}

	chain.Scan(handler);
	return true;
}

static bool
oggflac_scan_stream(InputStream &is, TagHandler &handler)
{
	FlacMetadataChain chain;
	if (!chain.ReadOgg(is)) {
		FormatDebug(flac_domain,
			    "Failed to read OggFLAC tags: %s",
			    chain.GetStatusString());
		return false;
	}

	chain.Scan(handler);
	return true;
}

static void
oggflac_decode(DecoderClient &client, InputStream &input_stream)
{
	if (ogg_codec_detect(&client, input_stream) != OGG_CODEC_FLAC)
		return;

	/* rewind the stream, because ogg_codec_detect() has
	   moved it */
	try {
		input_stream.LockRewind();
	} catch (...) {
	}

	flac_decode_internal(client, input_stream, true);
}

static const char *const oggflac_suffixes[] = { "ogg", "oga", nullptr };
static const char *const oggflac_mime_types[] = {
	"application/ogg",
	"application/x-ogg",
	"audio/ogg",
	"audio/x-flac+ogg",
	"audio/x-ogg",
	nullptr
};

constexpr DecoderPlugin oggflac_decoder_plugin =
	DecoderPlugin("oggflac", oggflac_decode, oggflac_scan_stream,
		      nullptr, oggflac_scan_file)
	.WithInit(oggflac_init)
	.WithSuffixes(oggflac_suffixes)
	.WithMimeTypes(oggflac_mime_types);

static const char *const flac_suffixes[] = { "flac", nullptr };
static const char *const flac_mime_types[] = {
	"application/flac",
	"application/x-flac",
	"audio/flac",
	"audio/x-flac",
	nullptr
};

constexpr DecoderPlugin flac_decoder_plugin =
	DecoderPlugin("flac", flac_decode, flac_scan_stream,
		      nullptr, flac_scan_file)
	.WithSuffixes(flac_suffixes)
	.WithMimeTypes(flac_mime_types);
