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

#include "FlacInput.hxx"
#include "FlacDomain.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "Log.hxx"
#include "util/Compiler.h"

#include <exception>

FLAC__StreamDecoderReadStatus
FlacInput::Read(FLAC__byte buffer[], size_t *bytes)
{
	size_t r = decoder_read(client, input_stream, (void *)buffer, *bytes);
	*bytes = r;

	if (r == 0) {
		if (input_stream.LockIsEOF() ||
		    (client != nullptr &&
		     client->GetCommand() != DecoderCommand::NONE))
			return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
		else
			return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	}

	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderSeekStatus
FlacInput::Seek(FLAC__uint64 absolute_byte_offset)
{
	if (!input_stream.IsSeekable())
		return FLAC__STREAM_DECODER_SEEK_STATUS_UNSUPPORTED;

	try {
		input_stream.LockSeek(absolute_byte_offset);
		return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
	} catch (...) {
		LogError(std::current_exception());
		return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
	}
}

FLAC__StreamDecoderTellStatus
FlacInput::Tell(FLAC__uint64 *absolute_byte_offset)
{
	if (!input_stream.IsSeekable())
		return FLAC__STREAM_DECODER_TELL_STATUS_UNSUPPORTED;

	*absolute_byte_offset = FLAC__uint64(input_stream.GetOffset());
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus
FlacInput::Length(FLAC__uint64 *stream_length)
{
	if (!input_stream.KnownSize())
		return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;

	*stream_length = FLAC__uint64(input_stream.GetSize());
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

FLAC__bool
FlacInput::Eof()
{
	return (client != nullptr &&
		client->GetCommand() != DecoderCommand::NONE &&
		client->GetCommand() != DecoderCommand::SEEK) ||
		input_stream.LockIsEOF();
}

void
FlacInput::Error(FLAC__StreamDecoderErrorStatus status)
{
	if (client == nullptr ||
	    client->GetCommand() != DecoderCommand::STOP)
		LogWarning(flac_domain,
			   FLAC__StreamDecoderErrorStatusString[status]);
}

FLAC__StreamDecoderReadStatus
FlacInput::Read([[maybe_unused]] const FLAC__StreamDecoder *flac_decoder,
		FLAC__byte buffer[], size_t *bytes,
		void *client_data)
{
	auto *i = static_cast<FlacInput *>(client_data);

	return i->Read(buffer, bytes);
}

FLAC__StreamDecoderSeekStatus
FlacInput::Seek([[maybe_unused]] const FLAC__StreamDecoder *flac_decoder,
		FLAC__uint64 absolute_byte_offset, void *client_data)
{
	auto *i = static_cast<FlacInput *>(client_data);

	return i->Seek(absolute_byte_offset);
}

FLAC__StreamDecoderTellStatus
FlacInput::Tell([[maybe_unused]] const FLAC__StreamDecoder *flac_decoder,
		FLAC__uint64 *absolute_byte_offset, void *client_data)
{
	auto *i = static_cast<FlacInput *>(client_data);

	return i->Tell(absolute_byte_offset);
}

FLAC__StreamDecoderLengthStatus
FlacInput::Length([[maybe_unused]] const FLAC__StreamDecoder *flac_decoder,
		  FLAC__uint64 *stream_length, void *client_data)
{
	auto *i = static_cast<FlacInput *>(client_data);

	return i->Length(stream_length);
}

FLAC__bool
FlacInput::Eof([[maybe_unused]] const FLAC__StreamDecoder *flac_decoder,
	       void *client_data)
{
	auto *i = static_cast<FlacInput *>(client_data);

	return i->Eof();
}

void
FlacInput::Error([[maybe_unused]] const FLAC__StreamDecoder *decoder,
		 FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	auto *i = static_cast<FlacInput *>(client_data);

	i->Error(status);
}

