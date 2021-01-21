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

#include "SoxrResampler.hxx"
#include "AudioFormat.hxx"
#include "config/Block.hxx"
#include "util/RuntimeError.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <soxr.h>

#include <cassert>
#include <cmath>

#include <string.h>

static constexpr Domain soxr_domain("soxr");

static constexpr unsigned long SOXR_DEFAULT_RECIPE = SOXR_HQ;

/**
 * Special value for "invalid argument".
 */
static constexpr unsigned long SOXR_INVALID_RECIPE = -1;

/**
 * Special value for the recipe selection for custom recipe.
 */
static constexpr unsigned long SOXR_CUSTOM_RECIPE = -2;

static soxr_io_spec_t soxr_io_custom_recipe;
static soxr_quality_spec_t soxr_quality;
static soxr_runtime_spec_t soxr_runtime;
static bool soxr_use_custom_recipe;


static constexpr struct {
	unsigned long recipe;
	const char *name;
} soxr_quality_table[] = {
	{ SOXR_VHQ, "very high" },
	{ SOXR_HQ, "high" },
	{ SOXR_MQ, "medium" },
	{ SOXR_LQ, "low" },
	{ SOXR_QQ, "quick" },
	{ SOXR_CUSTOM_RECIPE, "custom" },
	{ SOXR_INVALID_RECIPE, nullptr }
};

gcc_const
static const char *
soxr_quality_name(unsigned long recipe) noexcept
{
	for (const auto *i = soxr_quality_table;; ++i) {
		assert(i->name != nullptr);

		if (i->recipe == recipe)
			return i->name;
	}
}

gcc_pure
static unsigned long
soxr_parse_quality(const char *quality) noexcept
{
	if (quality == nullptr)
		return SOXR_DEFAULT_RECIPE;

	for (const auto *i = soxr_quality_table; i->name != nullptr; ++i)
		if (strcmp(i->name, quality) == 0)
			return i->recipe;

	return SOXR_INVALID_RECIPE;
}

static unsigned
SoxrParsePrecision(unsigned value) {
	switch (value) {
	case 16:
	case 20:
	case 24:
	case 28:
	case 32:
		break;
	default:
		throw FormatInvalidArgument(
			"soxr converter invalid precision : %d [16|20|24|28|32]", value);
	}
	return value;
}

static double
SoxrParsePhaseResponse(unsigned value) {
	if (value > 100) {
		throw FormatInvalidArgument(
			"soxr converter invalid phase_respons : %d (0-100)", value);
	}

	return double(value);
}

static double
SoxrParsePassbandEnd(const char *svalue) {
	char *endptr;
	double value = strtod(svalue, &endptr);
	if (svalue == endptr || *endptr != 0) {
		throw FormatInvalidArgument(
			"soxr converter passband_end value not a number: %s", svalue);
	}

	if (value < 1 || value > 100) {
		throw FormatInvalidArgument(
			"soxr converter invalid passband_end : %s (1-100%%)", svalue);
	}

	return value / 100.0;
}

static double
SoxrParseStopbandBegin(const char *svalue) {
	char *endptr;
	double value = strtod(svalue, &endptr);
	if (svalue == endptr || *endptr != 0) {
		throw FormatInvalidArgument(
			"soxr converter stopband_begin value not a number: %s", svalue);
	}

	if (value < 100 || value > 199) {
		throw FormatInvalidArgument(
			"soxr converter invalid stopband_begin : %s (100-150%%)", svalue);
	}

	return value / 100.0;
}

static double
SoxrParseAttenuation(const char *svalue) {
	char *endptr;
	double value = strtod(svalue, &endptr);
	if (svalue == endptr || *endptr != 0) {
		throw FormatInvalidArgument(
			"soxr converter attenuation value not a number: %s", svalue);
	}

	if (value < 0 || value > 30) {
		throw FormatInvalidArgument(
			"soxr converter invalid attenuation : %s (0-30dB))", svalue);
	}

	return 1 / std::pow(10, value / 10.0);
}

void
pcm_resample_soxr_global_init(const ConfigBlock &block)
{
	const char *quality_string = block.GetBlockValue("quality");
	unsigned long recipe = soxr_parse_quality(quality_string);
	soxr_use_custom_recipe = recipe == SOXR_CUSTOM_RECIPE;

	if (recipe == SOXR_INVALID_RECIPE) {
		assert(quality_string != nullptr);
		throw FormatRuntimeError("unknown quality setting '%s' in line %d",
					 quality_string, block.line);
	} else if (recipe == SOXR_CUSTOM_RECIPE) {
		// used to preset possible internal flags, like SOXR_RESET_ON_CLEAR
		soxr_quality = soxr_quality_spec(SOXR_DEFAULT_RECIPE, 0);
		soxr_io_custom_recipe = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);

		soxr_quality.precision =
			SoxrParsePrecision(block.GetBlockValue("precision", SOXR_HQ));
		soxr_quality.phase_response =
			SoxrParsePhaseResponse(block.GetBlockValue("phase_response", 50));
		soxr_quality.passband_end =
			SoxrParsePassbandEnd(block.GetBlockValue("passband_end", "95.0"));
		soxr_quality.stopband_begin = SoxrParseStopbandBegin(
			block.GetBlockValue("stopband_begin", "100.0"));
		// see soxr.h soxr_quality_spec.flags
		soxr_quality.flags = (soxr_quality.flags & 0xFFFFFFC0) |
			(block.GetBlockValue("flags", 0) & 0x3F);
		soxr_io_custom_recipe.scale =
			SoxrParseAttenuation(block.GetBlockValue("attenuation", "0"));
	} else {
		soxr_quality = soxr_quality_spec(recipe, 0);
	}

	FormatDebug(soxr_domain,
		    "soxr converter '%s'",
		    soxr_quality_name(recipe));

	const unsigned n_threads = block.GetBlockValue("threads", 1);
	soxr_runtime = soxr_runtime_spec(n_threads);
}

AudioFormat
SoxrPcmResampler::Open(AudioFormat &af, unsigned new_sample_rate)
{
	assert(af.IsValid());
	assert(audio_valid_sample_rate(new_sample_rate));

	soxr_error_t e;
	soxr_io_spec_t* p_soxr_io = nullptr;
	if(soxr_use_custom_recipe) {
		p_soxr_io = & soxr_io_custom_recipe;
	}
	soxr = soxr_create(af.sample_rate, new_sample_rate,
			   af.channels, &e,
			   p_soxr_io, &soxr_quality, &soxr_runtime);
	if (soxr == nullptr)
		throw FormatRuntimeError("soxr initialization has failed: %s",
					 e);

	FormatDebug(soxr_domain, "soxr engine '%s'", soxr_engine(soxr));
	if (soxr_use_custom_recipe)
		FormatDebug(soxr_domain,
			    "soxr precision=%0.0f, phase_response=%0.2f, "
			    "passband_end=%0.2f, stopband_begin=%0.2f scale=%0.2f",
			    soxr_quality.precision, soxr_quality.phase_response,
			    soxr_quality.passband_end, soxr_quality.stopband_begin,
			    soxr_io_custom_recipe.scale);
	else
		FormatDebug(soxr_domain,
			    "soxr precision=%0.0f, phase_response=%0.2f, "
			    "passband_end=%0.2f, stopband_begin=%0.2f",
			    soxr_quality.precision, soxr_quality.phase_response,
			    soxr_quality.passband_end, soxr_quality.stopband_begin);

	channels = af.channels;

	ratio = float(new_sample_rate) / float(af.sample_rate);
	FormatDebug(soxr_domain,
		    "samplerate conversion ratio to %.2lf",
		    double(ratio));

	/* libsoxr works with floating point samples */
	af.format = SampleFormat::FLOAT;

	AudioFormat result = af;
	result.sample_rate = new_sample_rate;
	return result;
}

void
SoxrPcmResampler::Close() noexcept
{
	soxr_delete(soxr);
}

void
SoxrPcmResampler::Reset() noexcept
{
#if SOXR_THIS_VERSION >= SOXR_VERSION(0,1,2)
	soxr_clear(soxr);
#endif
}

ConstBuffer<void>
SoxrPcmResampler::Resample(ConstBuffer<void> src)
{
	const size_t frame_size = channels * sizeof(float);
	assert(src.size % frame_size == 0);

	const size_t n_frames = src.size / frame_size;

	/* always round up: worst case output buffer size */
	const size_t o_frames = size_t(n_frames * ratio) + 1;

	auto *output_buffer = static_cast<float *>(buffer.Get(o_frames * frame_size));

	size_t i_done, o_done;
	soxr_error_t e = soxr_process(soxr, src.data, n_frames, &i_done,
				      output_buffer, o_frames, &o_done);
	if (e != nullptr)
		throw FormatRuntimeError("soxr error: %s", e);

	return { output_buffer, o_done * frame_size };
}

ConstBuffer<void>
SoxrPcmResampler::Flush()
{
	const size_t frame_size = channels * sizeof(float);
	const size_t o_frames = 1024;

	auto *output_buffer = static_cast<float *>(buffer.Get(o_frames * frame_size));

	size_t o_done;
	soxr_error_t e = soxr_process(soxr, nullptr, 0, nullptr,
				      output_buffer, o_frames, &o_done);
	if (e != nullptr)
		throw FormatRuntimeError("soxr error: %s", e);

	if (o_done == 0)
		/* flush complete */
		output_buffer = nullptr;

	return { output_buffer, o_done * frame_size };
}
