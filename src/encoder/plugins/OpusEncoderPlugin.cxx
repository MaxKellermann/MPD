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

#include "OpusEncoderPlugin.hxx"
#include "OggEncoder.hxx"
#include "pcm/AudioFormat.hxx"
#include "util/ByteOrder.hxx"
#include "util/StringUtil.hxx"

#include <opus.h>
#include <ogg/ogg.h>

#include <cassert>
#include <stdexcept>

#include <stdlib.h>

namespace {

class OpusEncoder final : public OggEncoder {
	const AudioFormat audio_format;

	const size_t frame_size;

	const size_t buffer_frames, buffer_size;
	size_t buffer_position = 0;
	uint8_t *const buffer;

	::OpusEncoder *const enc;

	unsigned char buffer2[1275 * 3 + 7];

	int lookahead;

	ogg_int64_t packetno = 0;

	ogg_int64_t granulepos = 0;

public:
	OpusEncoder(AudioFormat &_audio_format, ::OpusEncoder *_enc, bool _chaining);
	~OpusEncoder() noexcept override;

	/* virtual methods from class Encoder */
	void End() override;
	void Write(const void *data, size_t length) override;

	void PreTag() override;
	void SendTag(const Tag &tag) override;

private:
	void DoEncode(bool eos);
	void WriteSilence(unsigned fill_frames);

	void GenerateHeaders(const Tag *tag) noexcept;
	void GenerateHead() noexcept;
	void GenerateTags(const Tag *tag) noexcept;
};

class PreparedOpusEncoder final : public PreparedEncoder {
	opus_int32 bitrate;
	int complexity;
	int signal;
	int packet_loss;
	int vbr;
	int vbr_constraint;
	const bool chaining;

public:
	explicit PreparedOpusEncoder(const ConfigBlock &block);

	/* virtual methods from class PreparedEncoder */
	Encoder *Open(AudioFormat &audio_format) override;

	[[nodiscard]] const char *GetMimeType() const noexcept override {
		return "audio/ogg";
	}
};

PreparedOpusEncoder::PreparedOpusEncoder(const ConfigBlock &block)
	:chaining(block.GetBlockValue("opustags", false))
{
	const char *value = block.GetBlockValue("bitrate", "auto");
	if (strcmp(value, "auto") == 0)
		bitrate = OPUS_AUTO;
	else if (strcmp(value, "max") == 0)
		bitrate = OPUS_BITRATE_MAX;
	else {
		char *endptr;
		bitrate = strtoul(value, &endptr, 10);
		if (endptr == value || *endptr != 0 ||
		    bitrate < 500 || bitrate > 512000)
			throw std::runtime_error("Invalid bit rate");
	}

	complexity = block.GetBlockValue("complexity", 10U);
	if (complexity > 10)
		throw std::runtime_error("Invalid complexity");

	value = block.GetBlockValue("signal", "auto");
	if (strcmp(value, "auto") == 0)
		signal = OPUS_AUTO;
	else if (strcmp(value, "voice") == 0)
		signal = OPUS_SIGNAL_VOICE;
	else if (strcmp(value, "music") == 0)
		signal = OPUS_SIGNAL_MUSIC;
	else
		throw std::runtime_error("Invalid signal");

	value = block.GetBlockValue("vbr", "yes");
	if (strcmp(value, "yes") == 0) {
		vbr = 1U;
		vbr_constraint = 0U;
	} else if (strcmp(value, "no") == 0) {
		vbr = 0U;
		vbr_constraint = 0U;
	} else if (strcmp(value, "constrained") == 0) {
		vbr = 1U;
		vbr_constraint = 1U;
	} else
		throw std::runtime_error("Invalid vbr");

	packet_loss = block.GetBlockValue("packet_loss", 0U);
	if (packet_loss > 100)
		throw std::runtime_error("Invalid packet loss");
}

PreparedEncoder *
opus_encoder_init(const ConfigBlock &block)
{
	return new PreparedOpusEncoder(block);
}

OpusEncoder::OpusEncoder(AudioFormat &_audio_format, ::OpusEncoder *_enc, bool _chaining)
	:OggEncoder(_chaining),
	 audio_format(_audio_format),
	 frame_size(_audio_format.GetFrameSize()),
	 buffer_frames(_audio_format.sample_rate / 50),
	 buffer_size(frame_size * buffer_frames),
	 buffer(new uint8_t[buffer_size]),
	 enc(_enc)
{
	opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
	GenerateHeaders(nullptr);
}

Encoder *
PreparedOpusEncoder::Open(AudioFormat &audio_format)
{
	/* libopus supports only 48 kHz */
	audio_format.sample_rate = 48000;

	if (audio_format.channels > 2)
		audio_format.channels = 1;

	switch (audio_format.format) {
	case SampleFormat::S16:
	case SampleFormat::FLOAT:
		break;

	case SampleFormat::S8:
		audio_format.format = SampleFormat::S16;
		break;

	default:
		audio_format.format = SampleFormat::FLOAT;
		break;
	}

	int error_code;
	auto *enc = opus_encoder_create(audio_format.sample_rate,
					audio_format.channels,
					OPUS_APPLICATION_AUDIO,
					&error_code);
	if (enc == nullptr)
		throw std::runtime_error(opus_strerror(error_code));

	opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
	opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
	opus_encoder_ctl(enc, OPUS_SET_SIGNAL(signal));
	opus_encoder_ctl(enc, OPUS_SET_VBR(vbr));
	opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(vbr_constraint));
	opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(packet_loss));

	return new OpusEncoder(audio_format, enc, chaining);
}

OpusEncoder::~OpusEncoder() noexcept
{
	delete[] buffer;
	opus_encoder_destroy(enc);
}

void
OpusEncoder::DoEncode(bool eos)
{
	assert(buffer_position == buffer_size || eos);

	opus_int32 result =
		audio_format.format == SampleFormat::S16
		? opus_encode(enc,
		              reinterpret_cast<const opus_int16 *>(buffer),
		              buffer_frames,
		              buffer2,
		              sizeof(buffer2))
		: opus_encode_float(enc,
		                    reinterpret_cast<const float *>(buffer),
		                    buffer_frames,
		                    buffer2,
		                    sizeof(buffer2));
	if (result < 0)
		throw std::runtime_error("Opus encoder error");

	granulepos += buffer_position / frame_size;

	ogg_packet packet;
	packet.packet = buffer2;
	packet.bytes = result;
	packet.b_o_s = false;
	packet.e_o_s = eos;
	packet.granulepos = granulepos;
	packet.packetno = packetno++;
	stream.PacketIn(packet);

	buffer_position = 0;
}

void
OpusEncoder::End()
{
	memset(buffer + buffer_position, 0,
	       buffer_size - buffer_position);
	DoEncode(true);
	Flush();
}

void
OpusEncoder::WriteSilence(unsigned fill_frames)
{
	size_t fill_bytes = fill_frames * frame_size;

	while (fill_bytes > 0) {
		size_t nbytes = buffer_size - buffer_position;
		if (nbytes > fill_bytes)
			nbytes = fill_bytes;

		memset(buffer + buffer_position, 0, nbytes);
		buffer_position += nbytes;
		fill_bytes -= nbytes;

		if (buffer_position == buffer_size)
			DoEncode(false);
	}
}

void
OpusEncoder::Write(const void *_data, size_t length)
{
	const auto *data = static_cast<const uint8_t *>(_data);

	if (lookahead > 0) {
		/* generate some silence at the beginning of the
		   stream */

		assert(buffer_position == 0);

		WriteSilence(lookahead);
		lookahead = 0;
	}

	while (length > 0) {
		size_t nbytes = buffer_size - buffer_position;
		if (nbytes > length)
			nbytes = length;

		memcpy(buffer + buffer_position, data, nbytes);
		data += nbytes;
		length -= nbytes;
		buffer_position += nbytes;

		if (buffer_position == buffer_size)
			DoEncode(false);
	}
}

void
OpusEncoder::GenerateHeaders(const Tag *tag) noexcept
{
	GenerateHead();
	GenerateTags(tag);
}

void
OpusEncoder::GenerateHead() noexcept
{
	unsigned char header[19];
	memcpy(header, "OpusHead", 8);
	header[8] = 1;
	header[9] = audio_format.channels;
	*reinterpret_cast<uint16_t *>(header + 10) = ToLE16(lookahead);
	*reinterpret_cast<uint32_t *>(header + 12) = ToLE32(audio_format.sample_rate);
	header[16] = 0;
	header[17] = 0;
	header[18] = 0;

	ogg_packet packet;
	packet.packet = header;
	packet.bytes = sizeof(header);
	packet.b_o_s = true;
	packet.e_o_s = false;
	packet.granulepos = 0;
	packet.packetno = packetno++;
	stream.PacketIn(packet);
	// flush not needed because libogg autoflushes on b_o_s flag
}

void
OpusEncoder::GenerateTags(const Tag *tag) noexcept
{
	const char *version = opus_get_version_string();
	size_t version_length = strlen(version);

	// len("OpusTags") + 4 byte version length + len(version) + 4 byte tag count
	size_t comments_size = 8 + 4 + version_length + 4;
	uint32_t tag_count = 0;
	if (tag) {
		for (const auto &item: *tag) {
			++tag_count;
			// 4 byte length + len(tagname) + len('=') + len(value)
			comments_size += 4 + strlen(tag_item_names[item.type]) + 1 + strlen(item.value);
		}
	}

	auto *comments = new unsigned char[comments_size];
	unsigned char *p = comments;

	memcpy(comments, "OpusTags", 8);
	*reinterpret_cast<uint32_t *>(comments + 8) = ToLE32(version_length);
	p += 12;

	memcpy(p, version, version_length);
	p += version_length;

	tag_count = ToLE32(tag_count);
	memcpy(p, &tag_count, 4);
	p += 4;

	if (tag) {
		for (const auto &item: *tag) {
			size_t tag_name_len = strlen(tag_item_names[item.type]);
			size_t tag_val_len = strlen(item.value);
			uint32_t tag_len_le = ToLE32(tag_name_len + 1 + tag_val_len);

			memcpy(p, &tag_len_le, 4);
			p += 4;

			ToUpperASCII(reinterpret_cast<char *>(p), tag_item_names[item.type], tag_name_len + 1);
			p += tag_name_len;

			*p++ = '=';

			memcpy(p, item.value, tag_val_len);
			p += tag_val_len;
		}
	}
	assert(comments + comments_size == p);

	ogg_packet packet;
	packet.packet = comments;
	packet.bytes = comments_size;
	packet.b_o_s = false;
	packet.e_o_s = false;
	packet.granulepos = 0;
	packet.packetno = packetno++;
	stream.PacketIn(packet);
	Flush();

	delete[] comments;
}

void
OpusEncoder::PreTag()
{
	End();
	packetno = 0;
	granulepos = 0; // not really required, but useful to prevent wraparound
	opus_encoder_ctl(enc, OPUS_RESET_STATE);
}

void
OpusEncoder::SendTag(const Tag &tag)
{
	stream.Reinitialize(GenerateOggSerial());
	opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
	GenerateHeaders(&tag);
}

} // namespace

const EncoderPlugin opus_encoder_plugin = {
	"opus",
	opus_encoder_init,
};
