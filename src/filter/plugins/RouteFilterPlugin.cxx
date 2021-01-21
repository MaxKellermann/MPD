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

/** \file
 *
 * This filter copies audio data between channels. Useful for
 * upmixing mono/stereo audio to surround speaker configurations.
 *
 * Its configuration consists of a "filter" section with a single
 * "routes" entry, formatted as: \\
 * routes "0>1, 1>0, 2>2, 3>3, 3>4" \\
 * where each pair of numbers signifies a set of channels.
 * Each source>dest pair leads to the data from channel #source
 * being copied to channel #dest in the output.
 *
 * Example: \\
 * routes "0>0, 1>1, 0>2, 1>3"\\
 * upmixes stereo audio to a 4-speaker system, copying the front-left
 * (0) to front left (0) and rear left (2), copying front-right (1) to
 * front-right (1) and rear-right (3).
 *
 * If multiple sources are copied to the same destination channel, only
 * one of them takes effect.
 */

#include "RouteFilterPlugin.hxx"
#include "config/Block.hxx"
#include "filter/FilterPlugin.hxx"
#include "filter/Filter.hxx"
#include "filter/Prepared.hxx"
#include "pcm/AudioFormat.hxx"
#include "pcm/Buffer.hxx"
#include "pcm/Silence.hxx"
#include "util/StringStrip.hxx"
#include "util/RuntimeError.hxx"
#include "util/ConstBuffer.hxx"
#include "util/WritableBuffer.hxx"

#include <array>
#include <cstdint>
#include <stdexcept>

#include <string.h>
#include <stdlib.h>

class RouteFilter final : public Filter {
	/**
	 * The set of copy operations to perform on each sample
	 * The index is an output channel to use, the value is
	 * a corresponding input channel from which to take the
	 * data. A -1 means "no source"
	 */
	const std::array<int8_t, MAX_CHANNELS> sources;

	/**
	 * The actual input format of our signal, once opened
	 */
	const AudioFormat input_format;

	/**
	 * The size, in bytes, of each multichannel frame in the
	 * input buffer
	 */
	const size_t input_frame_size;

	/**
	 * The size, in bytes, of each multichannel frame in the
	 * output buffer
	 */
	size_t output_frame_size;

	/**
	 * The output buffer used last time around, can be reused if the size doesn't differ.
	 */
	PcmBuffer output_buffer;

public:
	RouteFilter(const AudioFormat &audio_format, unsigned out_channels,
		    const std::array<int8_t, MAX_CHANNELS> &_sources);

	/* virtual methods from class Filter */
	ConstBuffer<void> FilterPCM(ConstBuffer<void> src) override;
};

class PreparedRouteFilter final : public PreparedFilter {
	/**
	 * The minimum number of channels we need for output
	 * to be able to perform all the copies the user has specified
	 */
	unsigned min_output_channels;

	/**
	 * The minimum number of input channels we need to
	 * copy all the data the user has requested. If fewer
	 * than this many are supplied by the input, undefined
	 * copy operations are given zeroed sources in stead.
	 */
	unsigned min_input_channels;

	/**
	 * The set of copy operations to perform on each sample
	 * The index is an output channel to use, the value is
	 * a corresponding input channel from which to take the
	 * data. A -1 means "no source"
	 */
	std::array<int8_t, MAX_CHANNELS> sources;

public:
	/**
	 * Parse the "routes" section, a string on the form
	 *  a>b, c>d, e>f, ...
	 * where a... are non-unique, non-negative integers
	 * and input channel a gets copied to output channel b, etc.
	 * @param block the configuration block to read
	 * @param filter a route_filter whose min_channels and sources[] to set
	 */
	explicit PreparedRouteFilter(const ConfigBlock &block);

	/* virtual methods from class PreparedFilter */
	std::unique_ptr<Filter> Open(AudioFormat &af) override;
};

PreparedRouteFilter::PreparedRouteFilter(const ConfigBlock &block)
{
	/* TODO:
	 * With a more clever way of marking "don't copy to output N",
	 * This could easily be merged into a single loop with some
	 * dynamic realloc() instead of one count run and one malloc().
	 */

	sources.fill(-1);

	min_input_channels = 0;
	min_output_channels = 0;

	// A cowardly default, just passthrough stereo
	const char *routes = block.GetBlockValue("routes", "0>0, 1>1");
	while (true) {
		routes = StripLeft(routes);

		char *endptr;
		const unsigned source = strtoul(routes, &endptr, 10);
		endptr = StripLeft(endptr);
		if (endptr == routes || *endptr != '>')
			throw std::runtime_error("Malformed 'routes' specification");

		if (source >= MAX_CHANNELS)
			throw FormatRuntimeError("Invalid source channel number: %u",
						 source);

		if (source >= min_input_channels)
			min_input_channels = source + 1;

		routes = StripLeft(endptr + 1);

		unsigned dest = strtoul(routes, &endptr, 10);
		endptr = StripLeft(endptr);
		if (endptr == routes)
			throw std::runtime_error("Malformed 'routes' specification");

		if (dest >= MAX_CHANNELS)
			throw FormatRuntimeError("Invalid destination channel number: %u",
						 dest);

		if (dest >= min_output_channels)
			min_output_channels = dest + 1;

		sources[dest] = source;

		routes = endptr;

		if (*routes == 0)
			break;

		if (*routes != ',')
			throw std::runtime_error("Malformed 'routes' specification");

		++routes;
	}
}

static std::unique_ptr<PreparedFilter>
route_filter_init(const ConfigBlock &block)
{
	return std::make_unique<PreparedRouteFilter>(block);
}

RouteFilter::RouteFilter(const AudioFormat &audio_format,
			 unsigned out_channels,
			 const std::array<int8_t, MAX_CHANNELS> &_sources)
	:Filter(audio_format), sources(_sources), input_format(audio_format),
	 input_frame_size(input_format.GetFrameSize())
{
	// Decide on an output format which has enough channels,
	// and is otherwise identical
	out_audio_format.channels = out_channels;

	// Precalculate this simple value, to speed up allocation later
	output_frame_size = out_audio_format.GetFrameSize();
}

std::unique_ptr<Filter>
PreparedRouteFilter::Open(AudioFormat &audio_format)
{
	return std::make_unique<RouteFilter>(audio_format, min_output_channels,
					     sources);
}

ConstBuffer<void>
RouteFilter::FilterPCM(ConstBuffer<void> src)
{
	size_t number_of_frames = src.size / input_frame_size;

	const size_t bytes_per_frame_per_channel = input_format.GetSampleSize();

	// A moving pointer that always refers to channel 0 in the input, at the currently handled frame
	const auto *base_source = static_cast<const uint8_t *>(src.data);

	// Grow our reusable buffer, if needed, and set the moving pointer
	const size_t result_size = number_of_frames * output_frame_size;
	void *const result = output_buffer.Get(result_size);

	// A moving pointer that always refers to the currently filled channel of the currently handled frame, in the output
	auto *chan_destination = static_cast<uint8_t *>(result);

	// Perform our copy operations, with N input channels and M output channels
	for (unsigned int s=0; s<number_of_frames; ++s) {

		// Need to perform one copy per output channel
		for (unsigned c = 0; c < out_audio_format.channels; ++c) {
			if (sources[c] == -1 ||
			    unsigned(sources[c]) >= input_format.channels) {
				// No source for this destination output,
				// give it zeroes as input
				PcmSilence({chan_destination, bytes_per_frame_per_channel},
					   input_format.format);
			} else {
				// Get the data from channel sources[c]
				// and copy it to the output
				const uint8_t *data = base_source +
					(sources[c] * bytes_per_frame_per_channel);
				memcpy(chan_destination,
					  data,
					  bytes_per_frame_per_channel);
			}
			// Move on to the next output channel
			chan_destination += bytes_per_frame_per_channel;
		}


		// Go on to the next N input samples
		base_source += input_frame_size;
	}

	// Here it is, ladies and gentlemen! Rerouted data!
	return { result, result_size };
}

const FilterPlugin route_filter_plugin = {
	"route",
	route_filter_init,
};
