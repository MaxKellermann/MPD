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

#include "NormalizeFilterPlugin.hxx"
#include "filter/FilterPlugin.hxx"
#include "filter/Filter.hxx"
#include "filter/Prepared.hxx"
#include "pcm/Buffer.hxx"
#include "pcm/AudioFormat.hxx"
#include "AudioCompress/compress.h"
#include "util/ConstBuffer.hxx"

#include <string.h>

class NormalizeFilter final : public Filter {
	Compressor *const compressor;

	PcmBuffer buffer;

public:
	explicit NormalizeFilter(const AudioFormat &audio_format)
		:Filter(audio_format), compressor(Compressor_new(0)) {
	}

	~NormalizeFilter() override {
		Compressor_delete(compressor);
	}

	/* virtual methods from class Filter */
	ConstBuffer<void> FilterPCM(ConstBuffer<void> src) override;
};

class PreparedNormalizeFilter final : public PreparedFilter {
public:
	/* virtual methods from class PreparedFilter */
	std::unique_ptr<Filter> Open(AudioFormat &af) override;
};

static std::unique_ptr<PreparedFilter>
normalize_filter_init([[maybe_unused]] const ConfigBlock &block)
{
	return std::make_unique<PreparedNormalizeFilter>();
}

std::unique_ptr<Filter>
PreparedNormalizeFilter::Open(AudioFormat &audio_format)
{
	audio_format.format = SampleFormat::S16;

	return std::make_unique<NormalizeFilter>(audio_format);
}

ConstBuffer<void>
NormalizeFilter::FilterPCM(ConstBuffer<void> src)
{
	auto *dest = static_cast<int16_t *>(buffer.Get(src.size));
	memcpy(dest, src.data, src.size);

	Compressor_Process_int16(compressor, dest, src.size / 2);
	return { (const void *)dest, src.size };
}

const FilterPlugin normalize_filter_plugin = {
	"normalize",
	normalize_filter_init,
};

std::unique_ptr<PreparedFilter>
normalize_filter_prepare() noexcept
{
	return std::make_unique<PreparedNormalizeFilter>();
}
