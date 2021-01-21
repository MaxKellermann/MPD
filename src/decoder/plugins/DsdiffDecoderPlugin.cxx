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

/* \file
 *
 * This plugin decodes DSDIFF data (SACD) embedded in DFF files.
 * The DFF code was modeled after the specification found here:
 * http://www.sonicstudio.com/pdf/dsd/DSDIFF_1.5_Spec.pdf
 *
 * All functions common to both DSD decoders have been moved to dsdlib
 */

#include "config.h"
#include "DsdiffDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "util/BitReverse.hxx"
#include "util/ByteOrder.hxx"
#include "util/StringView.hxx"
#include "tag/Handler.hxx"
#include "DsdLib.hxx"

struct DsdiffHeader {
	DsdId id;
	DffDsdUint64 size;
	DsdId format;
};

struct DsdiffChunkHeader {
	DsdId id;
	DffDsdUint64 size;

	/**
	 * Read the "size" attribute from the specified header, converting it
	 * to the host byte order if needed.
	 */
	[[nodiscard]] constexpr
	uint64_t GetSize() const {
		return size.Read();
	}

	/**
	 * Applies padding to GetSize(), according to the DSDIFF
	 * specification
	 * (http://www.sonicstudio.com/pdf/dsd/DSDIFF_1.5_Spec.pdf)
	 * section 2.3.
	 */
	[[nodiscard]] constexpr
	uint64_t GetPaddedSize() const noexcept {
		return (GetSize() + 1) & ~uint64_t(1);
	}
};

/** struct for DSDIFF native Artist and Title tags */
struct dsdiff_native_tag {
	uint32_t size;
};

struct DsdiffMetaData {
	unsigned sample_rate, channels;
	bool bitreverse;
	offset_type chunk_size;
};

static bool lsbitfirst;

static bool
dsdiff_init(const ConfigBlock &block)
{
	lsbitfirst = block.GetBlockValue("lsbitfirst", false);
	return true;
}

static bool
dsdiff_read_id(DecoderClient *client, InputStream &is,
	       DsdId *id)
{
	return decoder_read_full(client, is, id, sizeof(*id));
}

static bool
dsdiff_read_chunk_header(DecoderClient *client, InputStream &is,
			 DsdiffChunkHeader *header)
{
	return decoder_read_full(client, is, header, sizeof(*header));
}

static bool
dsdiff_read_payload(DecoderClient *client, InputStream &is,
		    const DsdiffChunkHeader *header,
		    void *data, size_t length)
{
	uint64_t size = header->GetSize();
	if (size != uint64_t(length))
		return false;

	return decoder_read_full(client, is, data, length);
}

/**
 * Read and parse a "SND" chunk inside "PROP".
 */
static bool
dsdiff_read_prop_snd(DecoderClient *client, InputStream &is,
		     DsdiffMetaData *metadata,
		     offset_type end_offset)
{
	DsdiffChunkHeader header;
	while (is.GetOffset() + sizeof(header) <= end_offset) {
		if (!dsdiff_read_chunk_header(client, is, &header))
			return false;

		offset_type chunk_end_offset = is.GetOffset()
			+ header.GetPaddedSize();
		if (chunk_end_offset > end_offset)
			return false;

		if (header.id.Equals("FS  ")) {
			uint32_t sample_rate;
			if (!dsdiff_read_payload(client, is, &header,
						 &sample_rate,
						 sizeof(sample_rate)))
				return false;

			metadata->sample_rate = FromBE32(sample_rate);
		} else if (header.id.Equals("CHNL")) {
			uint16_t channels;
			if (header.GetSize() < sizeof(channels) ||
			    !decoder_read_full(client, is,
					       &channels, sizeof(channels)) ||
			    !dsdlib_skip_to(client, is, chunk_end_offset))
				return false;

			metadata->channels = FromBE16(channels);
		} else if (header.id.Equals("CMPR")) {
			DsdId type;
			if (header.GetSize() < sizeof(type) ||
			    !decoder_read_full(client, is,
					       &type, sizeof(type)) ||
			    !dsdlib_skip_to(client, is, chunk_end_offset))
				return false;

			if (!type.Equals("DSD "))
				/* only uncompressed DSD audio data
				   is implemented */
				return false;
		} else {
			/* ignore unknown chunk */

			if (!dsdlib_skip_to(client, is, chunk_end_offset))
				return false;
		}
	}

	return is.GetOffset() == end_offset;
}

/**
 * Read and parse a "PROP" chunk.
 */
static bool
dsdiff_read_prop(DecoderClient *client, InputStream &is,
		 DsdiffMetaData *metadata,
		 const DsdiffChunkHeader *prop_header)
{
	uint64_t prop_size = prop_header->GetSize();
	const offset_type end_offset = is.GetOffset() + prop_size;

	DsdId prop_id;
	if (prop_size < sizeof(prop_id) ||
	    !dsdiff_read_id(client, is, &prop_id))
		return false;

	if (prop_id.Equals("SND "))
		return dsdiff_read_prop_snd(client, is, metadata, end_offset);
	else
		/* ignore unknown PROP chunk */
		return dsdlib_skip_to(client, is, end_offset);
}

static void
dsdiff_handle_native_tag(DecoderClient *client, InputStream &is,
			 TagHandler &handler,
			 offset_type tagoffset,
			 TagType type)
{
	if (!dsdlib_skip_to(client, is, tagoffset))
		return;

	struct dsdiff_native_tag metatag;

	if (!decoder_read_full(client, is, &metatag, sizeof(metatag)))
		return;

	uint32_t length = FromBE32(metatag.size);

	/* Check and limit size of the tag to prevent a stack overflow */
	constexpr size_t MAX_LENGTH = 1024;
	if (length == 0 || length > MAX_LENGTH)
		return;

	char string[MAX_LENGTH];
	char *label;
	label = string;

	if (!decoder_read_full(client, is, label, size_t(length)))
		return;

	handler.OnTag(type, {label, length});
}

/**
 * Read and parse additional metadata chunks for tagging purposes. By default
 * dsdiff files only support equivalents for artist and title but some of the
 * extract tools add an id3 tag to provide more tags. If such id3 is found
 * this will be used for tagging otherwise the native tags (if any) will be
 * used
 */

static bool
dsdiff_read_metadata_extra(DecoderClient *client, InputStream &is,
			   DsdiffMetaData *metadata,
			   DsdiffChunkHeader *chunk_header,
			   TagHandler &handler)
{

	/* skip from DSD data to next chunk header */
	if (!dsdlib_skip(client, is, metadata->chunk_size))
		return false;
	if (!dsdiff_read_chunk_header(client, is, chunk_header))
		return false;

	/** offset for artist tag */
	offset_type artist_offset = 0;
	/** offset for title tag */
	offset_type title_offset = 0;

#ifdef ENABLE_ID3TAG
	offset_type id3_offset = 0;
#endif

	/* Now process all the remaining chunk headers in the stream
	   and record their position and size */

	do {
		offset_type chunk_size = chunk_header->GetSize();

		/* DIIN chunk, is directly followed by other chunks  */
		if (chunk_header->id.Equals("DIIN"))
			chunk_size = 0;

		/* DIAR chunk - DSDIFF native tag for Artist */
		if (chunk_header->id.Equals("DIAR")) {
			chunk_size = chunk_header->GetSize();
			artist_offset = is.GetOffset();
		}

		/* DITI chunk - DSDIFF native tag for Title */
		if (chunk_header->id.Equals("DITI")) {
			chunk_size = chunk_header->GetSize();
			title_offset = is.GetOffset();
		}
#ifdef ENABLE_ID3TAG
		/* 'ID3 ' chunk, offspec. Used by sacdextract */
		if (chunk_header->id.Equals("ID3 ")) {
			chunk_size = chunk_header->GetSize();
			id3_offset = is.GetOffset();
		}
#endif

		if (!dsdlib_skip(client, is, chunk_size))
			break;
	} while (dsdiff_read_chunk_header(client, is, chunk_header));

	/* done processing chunk headers, process tags if any */

#ifdef ENABLE_ID3TAG
	if (id3_offset != 0) {
		/* a ID3 tag has preference over the other tags, do not process
		   other tags if we have one */
		dsdlib_tag_id3(is, handler, id3_offset);
		return true;
	}
#endif

	if (artist_offset != 0)
		dsdiff_handle_native_tag(client, is, handler,
					 artist_offset, TAG_ARTIST);

	if (title_offset != 0)
		dsdiff_handle_native_tag(client, is, handler,
					 title_offset, TAG_TITLE);
	return true;
}

/**
 * Read and parse all metadata chunks at the beginning.  Stop when the
 * first "DSD" chunk is seen, and return its header in the
 * "chunk_header" parameter.
 */
static bool
dsdiff_read_metadata(DecoderClient *client, InputStream &is,
		     DsdiffMetaData *metadata,
		     DsdiffChunkHeader *chunk_header)
{
	DsdiffHeader header;
	if (!decoder_read_full(client, is, &header, sizeof(header)) ||
	    !header.id.Equals("FRM8") ||
	    !header.format.Equals("DSD "))
		return false;

	while (true) {
		if (!dsdiff_read_chunk_header(client, is,
					      chunk_header))
			return false;

		if (chunk_header->id.Equals("PROP")) {
			if (!dsdiff_read_prop(client, is, metadata,
					      chunk_header))
					return false;
		} else if (chunk_header->id.Equals("DSD ")) {
			const offset_type chunk_size = chunk_header->GetSize();
			metadata->chunk_size = chunk_size;
			return true;
		} else {
			/* ignore unknown chunk */
			const offset_type chunk_size = chunk_header->GetSize();
			const offset_type chunk_end_offset =
				is.GetOffset() + chunk_size;

			if (!dsdlib_skip_to(client, is, chunk_end_offset))
				return false;
		}
	}
}

static void
bit_reverse_buffer(uint8_t *p, uint8_t *end)
{
	for (; p < end; ++p)
		*p = bit_reverse(*p);
}

static offset_type
FrameToOffset(uint64_t frame, unsigned channels)
{
	return frame * channels;
}

/**
 * Decode one "DSD" chunk.
 */
static bool
dsdiff_decode_chunk(DecoderClient &client, InputStream &is,
		    unsigned channels, unsigned sample_rate,
		    const offset_type total_bytes)
{
	const unsigned kbit_rate = channels * sample_rate / 1000;
	const offset_type start_offset = is.GetOffset();

	uint8_t buffer[8192];

	const size_t sample_size = sizeof(buffer[0]);
	const size_t frame_size = channels * sample_size;
	const unsigned buffer_frames = sizeof(buffer) / frame_size;
	const size_t buffer_size = buffer_frames * frame_size;

	auto cmd = client.GetCommand();
	for (offset_type remaining_bytes = total_bytes;
	     remaining_bytes >= frame_size && cmd != DecoderCommand::STOP;) {
		if (cmd == DecoderCommand::SEEK) {
			uint64_t frame = client.GetSeekFrame();
			offset_type offset = FrameToOffset(frame, channels);
			if (offset >= total_bytes) {
				client.CommandFinished();
				break;
			}

			if (dsdlib_skip_to(&client, is,
					   start_offset + offset)) {
				client.CommandFinished();
				remaining_bytes = total_bytes - offset;
			} else
				client.SeekError();
		}

		/* see how much aligned data from the remaining chunk
		   fits into the local buffer */
		size_t now_size = buffer_size;
		if (remaining_bytes < offset_type(now_size)) {
			unsigned now_frames = remaining_bytes / frame_size;
			now_size = now_frames * frame_size;
		}

		if (!decoder_read_full(&client, is, buffer, now_size))
			return false;

		const size_t nbytes = now_size;
		remaining_bytes -= nbytes;

		if (lsbitfirst)
			bit_reverse_buffer(buffer, buffer + nbytes);

		cmd = client.SubmitData(is, buffer, nbytes,
					kbit_rate);
	}

	return true;
}

static void
dsdiff_stream_decode(DecoderClient &client, InputStream &is)
{
	DsdiffMetaData metadata;

	DsdiffChunkHeader chunk_header;
	/* check if it is is a proper DFF file */
	if (!dsdiff_read_metadata(&client, is, &metadata, &chunk_header))
		return;

	auto audio_format = CheckAudioFormat(metadata.sample_rate / 8,
					     SampleFormat::DSD,
					     metadata.channels);

	/* calculate song time from DSD chunk size and sample frequency */
	offset_type chunk_size = metadata.chunk_size;

	uint64_t n_frames = chunk_size / audio_format.channels;
	auto songtime = SongTime::FromScale<uint64_t>(n_frames,
						      audio_format.sample_rate);

	/* success: file was recognized */
	client.Ready(audio_format, is.IsSeekable(), songtime);

	/* every iteration of the following loop decodes one "DSD"
	   chunk from a DFF file */

	dsdiff_decode_chunk(client, is,
			    metadata.channels,
			    metadata.sample_rate,
			    chunk_size);
}

static bool
dsdiff_scan_stream(InputStream &is, TagHandler &handler)
{
	DsdiffMetaData metadata;
	DsdiffChunkHeader chunk_header;

	/* First check for DFF metadata */
	if (!dsdiff_read_metadata(nullptr, is, &metadata, &chunk_header))
		return false;

	const auto sample_rate = metadata.sample_rate / 8;
	if (!audio_valid_sample_rate(sample_rate) ||
	    !audio_valid_channel_count(metadata.channels))
		return false;

	/* calculate song time and add as tag */
	uint64_t n_frames = metadata.chunk_size / metadata.channels;
	auto songtime = SongTime::FromScale<uint64_t>(n_frames,
						      sample_rate);
	handler.OnDuration(songtime);

	/* Read additional metadata and created tags if available */
	dsdiff_read_metadata_extra(nullptr, is, &metadata, &chunk_header,
				   handler);

	return true;
}

static const char *const dsdiff_suffixes[] = {
	"dff",
	nullptr
};

static const char *const dsdiff_mime_types[] = {
	"application/x-dff",
	"audio/x-dff",
	"audio/x-dsd",
	nullptr
};

constexpr DecoderPlugin dsdiff_decoder_plugin =
	DecoderPlugin("dsdiff", dsdiff_stream_decode, dsdiff_scan_stream)
	.WithInit(dsdiff_init)
	.WithSuffixes(dsdiff_suffixes)
	.WithMimeTypes(dsdiff_mime_types);
