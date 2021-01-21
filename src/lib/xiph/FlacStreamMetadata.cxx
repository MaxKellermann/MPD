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

#include "FlacStreamMetadata.hxx"
#include "FlacAudioFormat.hxx"
#include "ScanVorbisComment.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "MixRampInfo.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "tag/Tag.hxx"
#include "tag/ReplayGain.hxx"
#include "tag/MixRamp.hxx"
#include "ReplayGainInfo.hxx"
#include "util/StringView.hxx"

#include <cassert>

static StringView
ToStringView(const FLAC__StreamMetadata_VorbisComment_Entry &entry) noexcept
{
	return {reinterpret_cast<const char *>(entry.entry), entry.length};
}

bool
flac_parse_replay_gain(ReplayGainInfo &rgi,
		       const FLAC__StreamMetadata_VorbisComment &vc)
{
	rgi.Clear();

	bool found = false;

	const auto *comments = vc.comments;
	for (FLAC__uint32 i = 0, n = vc.num_comments; i < n; ++i)
		if (ParseReplayGainVorbis(rgi, ToStringView(comments[i])))
			found = true;

	return found;
}

MixRampInfo
flac_parse_mixramp(const FLAC__StreamMetadata_VorbisComment &vc)
{
	MixRampInfo mix_ramp;

	const auto *comments = vc.comments;
	for (FLAC__uint32 i = 0, n = vc.num_comments; i < n; ++i)
		ParseMixRampVorbis(mix_ramp, ToStringView(comments[i]));

	return mix_ramp;
}

static void
flac_scan_comments(const FLAC__StreamMetadata_VorbisComment *comment,
		   TagHandler &handler) noexcept
{
	for (unsigned i = 0; i < comment->num_comments; ++i)
		ScanVorbisComment(ToStringView(comment->comments[i]), handler);
}

gcc_pure
static inline SongTime
flac_duration(const FLAC__StreamMetadata_StreamInfo *stream_info) noexcept
{
	assert(stream_info->sample_rate > 0);

	return SongTime::FromScale<uint64_t>(stream_info->total_samples,
					     stream_info->sample_rate);
}

static void
Scan(const FLAC__StreamMetadata_StreamInfo &stream_info,
     TagHandler &handler) noexcept
{
	if (stream_info.sample_rate > 0)
		handler.OnDuration(flac_duration(&stream_info));

	try {
		handler.OnAudioFormat(CheckAudioFormat(stream_info.sample_rate,
						       FlacSampleFormat(stream_info.bits_per_sample),
						       stream_info.channels));
	} catch (...) {
	}
}

static void
Scan(const FLAC__StreamMetadata_Picture &picture, TagHandler &handler) noexcept
{
	if (!handler.WantPicture())
		return;

	if (picture.mime_type != nullptr &&
	    StringIsEqual(picture.mime_type, "-->"))
		/* this is a URL, not image data */
		return;

	handler.OnPicture(picture.mime_type,
			  {picture.data, picture.data_length});
}

void
flac_scan_metadata(const FLAC__StreamMetadata *block,
		   TagHandler &handler) noexcept
{
	switch (block->type) {
	case FLAC__METADATA_TYPE_VORBIS_COMMENT:
		flac_scan_comments(&block->data.vorbis_comment,
				   handler);
		break;

	case FLAC__METADATA_TYPE_STREAMINFO:
		Scan(block->data.stream_info, handler);
		break;

	case FLAC__METADATA_TYPE_PICTURE:
		Scan(block->data.picture, handler);
		break;

	default:
		break;
	}
}

Tag
flac_vorbis_comments_to_tag(const FLAC__StreamMetadata_VorbisComment *comment)
{
	TagBuilder tag_builder;
	AddTagHandler h(tag_builder);
	flac_scan_comments(comment, h);
	return tag_builder.Commit();
}
