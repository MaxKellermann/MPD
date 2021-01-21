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

#include "HwSetup.hxx"
#include "Format.hxx"
#include "util/ByteOrder.hxx"
#include "util/Domain.hxx"
#include "util/RuntimeError.hxx"
#include "pcm/AudioFormat.hxx"
#include "Log.hxx"
#include "config.h"

static constexpr Domain alsa_output_domain("alsa_output");

namespace Alsa {

/**
 * Attempts to configure the specified sample format.  On failure,
 * fall back to the packed version.
 */
static int
TryFormatOrPacked(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
		  snd_pcm_format_t fmt, PcmExport::Params &params)
{
	int err = snd_pcm_hw_params_set_format(pcm, hwparams, fmt);
	if (err == 0)
		params.pack24 = false;

	if (err != -EINVAL)
		return err;

	fmt = PackAlsaPcmFormat(fmt);
	if (fmt == SND_PCM_FORMAT_UNKNOWN)
		return -EINVAL;

	err = snd_pcm_hw_params_set_format(pcm, hwparams, fmt);
	if (err == 0)
		params.pack24 = true;

	return err;
}

/**
 * Attempts to configure the specified sample format, and tries the
 * reversed host byte order if was not supported.
 */
static int
TryFormatOrByteSwap(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
		    snd_pcm_format_t fmt,
		    PcmExport::Params &params)
{
	int err = TryFormatOrPacked(pcm, hwparams, fmt, params);
	if (err == 0)
		params.reverse_endian = false;

	if (err != -EINVAL)
		return err;

	fmt = ByteSwapAlsaPcmFormat(fmt);
	if (fmt == SND_PCM_FORMAT_UNKNOWN)
		return -EINVAL;

	err = TryFormatOrPacked(pcm, hwparams, fmt, params);
	if (err == 0)
		params.reverse_endian = true;

	return err;
}

/**
 * Attempts to configure the specified sample format.  On DSD_U8
 * failure, attempt to switch to DSD_U32 or DSD_U16.
 */
static int
TryFormatDsd(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
	     snd_pcm_format_t fmt, PcmExport::Params &params)
{
	int err = TryFormatOrByteSwap(pcm, hwparams, fmt, params);

#if defined(ENABLE_DSD) && defined(HAVE_ALSA_DSD_U32)
	if (err == -EINVAL && fmt == SND_PCM_FORMAT_DSD_U8) {
		/* attempt to switch to DSD_U32 */
		fmt = IsLittleEndian()
			? SND_PCM_FORMAT_DSD_U32_LE
			: SND_PCM_FORMAT_DSD_U32_BE;
		err = TryFormatOrByteSwap(pcm, hwparams, fmt, params);
		if (err == 0)
			params.dsd_mode = PcmExport::DsdMode::U32;
		else
			fmt = SND_PCM_FORMAT_DSD_U8;
	}

	if (err == -EINVAL && fmt == SND_PCM_FORMAT_DSD_U8) {
		/* attempt to switch to DSD_U16 */
		fmt = IsLittleEndian()
			? SND_PCM_FORMAT_DSD_U16_LE
			: SND_PCM_FORMAT_DSD_U16_BE;
		err = TryFormatOrByteSwap(pcm, hwparams, fmt, params);
		if (err == 0)
			params.dsd_mode = PcmExport::DsdMode::U16;
		else
			fmt = SND_PCM_FORMAT_DSD_U8;
	}
#endif

	return err;
}

static int
TryFormat(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
	  SampleFormat sample_format,
	  PcmExport::Params &params)
{
	snd_pcm_format_t alsa_format = ToAlsaPcmFormat(sample_format);
	if (alsa_format == SND_PCM_FORMAT_UNKNOWN)
		return -EINVAL;

	return TryFormatDsd(pcm, hwparams, alsa_format, params);
}

/**
 * Configure a sample format, and probe other formats if that fails.
 */
static int
SetupSampleFormat(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwparams,
		  SampleFormat &sample_format,
		  PcmExport::Params &params)
{
	/* try the input format first */

	int err = TryFormat(pcm, hwparams, sample_format, params);

	/* if unsupported by the hardware, try other formats */

	static constexpr SampleFormat probe_formats[] = {
		SampleFormat::S24_P32,
		SampleFormat::S32,
		SampleFormat::S16,
		SampleFormat::S8,
		SampleFormat::UNDEFINED,
	};

	for (unsigned i = 0;
	     err == -EINVAL && probe_formats[i] != SampleFormat::UNDEFINED;
	     ++i) {
		const SampleFormat mpd_format = probe_formats[i];
		if (mpd_format == sample_format)
			continue;

		err = TryFormat(pcm, hwparams, mpd_format, params);
		if (err == 0)
			sample_format = mpd_format;
	}

	return err;
}

HwResult
SetupHw(snd_pcm_t *pcm,
	unsigned buffer_time, unsigned period_time,
	AudioFormat &audio_format, PcmExport::Params &params)
{
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_hw_params_alloca(&hwparams);

	int err;
	unsigned int period_time_ro = period_time;

	/* configure HW params */
	err = snd_pcm_hw_params_any(pcm, hwparams);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params_any() failed: %s",
					 snd_strerror(-err));

	err = snd_pcm_hw_params_set_access(pcm, hwparams,
					   SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params_set_access() failed: %s",
					 snd_strerror(-err));

	err = SetupSampleFormat(pcm, hwparams,
				audio_format.format, params);
	if (err < 0)
		throw FormatRuntimeError("Failed to configure format %s: %s",
					 sample_format_to_string(audio_format.format),
					 snd_strerror(-err));

	unsigned int channels = audio_format.channels;
	err = snd_pcm_hw_params_set_channels_near(pcm, hwparams,
						  &channels);
	if (err < 0)
		throw FormatRuntimeError("Failed to configure %i channels: %s",
					 int(audio_format.channels),
					 snd_strerror(-err));

	audio_format.channels = int8_t(channels);

	const unsigned requested_sample_rate =
		params.CalcOutputSampleRate(audio_format.sample_rate);
	unsigned output_sample_rate = requested_sample_rate;

	err = snd_pcm_hw_params_set_rate_near(pcm, hwparams,
					      &output_sample_rate, nullptr);
	if (err < 0)
		throw FormatRuntimeError("Failed to configure sample rate %u Hz: %s",
					 requested_sample_rate,
					 snd_strerror(-err));

	if (output_sample_rate == 0)
		throw FormatRuntimeError("Failed to configure sample rate %u Hz",
					 audio_format.sample_rate);

	if (output_sample_rate != requested_sample_rate)
		audio_format.sample_rate = params.CalcInputSampleRate(output_sample_rate);

	snd_pcm_uframes_t buffer_size_min, buffer_size_max;
	snd_pcm_hw_params_get_buffer_size_min(hwparams, &buffer_size_min);
	snd_pcm_hw_params_get_buffer_size_max(hwparams, &buffer_size_max);
	unsigned buffer_time_min, buffer_time_max;
	snd_pcm_hw_params_get_buffer_time_min(hwparams, &buffer_time_min, nullptr);
	snd_pcm_hw_params_get_buffer_time_max(hwparams, &buffer_time_max, nullptr);
	FormatDebug(alsa_output_domain, "buffer: size=%u..%u time=%u..%u",
		    unsigned(buffer_size_min), unsigned(buffer_size_max),
		    buffer_time_min, buffer_time_max);

	snd_pcm_uframes_t period_size_min, period_size_max;
	snd_pcm_hw_params_get_period_size_min(hwparams, &period_size_min, nullptr);
	snd_pcm_hw_params_get_period_size_max(hwparams, &period_size_max, nullptr);
	unsigned period_time_min, period_time_max;
	snd_pcm_hw_params_get_period_time_min(hwparams, &period_time_min, nullptr);
	snd_pcm_hw_params_get_period_time_max(hwparams, &period_time_max, nullptr);
	FormatDebug(alsa_output_domain, "period: size=%u..%u time=%u..%u",
		    unsigned(period_size_min), unsigned(period_size_max),
		    period_time_min, period_time_max);

	if (buffer_time > 0) {
		err = snd_pcm_hw_params_set_buffer_time_near(pcm, hwparams,
							     &buffer_time, nullptr);
		if (err < 0)
			throw FormatRuntimeError("snd_pcm_hw_params_set_buffer_time_near() failed: %s",
						 snd_strerror(-err));
	} else {
		err = snd_pcm_hw_params_get_buffer_time(hwparams, &buffer_time,
							nullptr);
		if (err < 0)
			buffer_time = 0;
	}

	if (period_time_ro == 0 && buffer_time >= 10000) {
		period_time_ro = period_time = buffer_time / 4;

		FormatDebug(alsa_output_domain,
			    "default period_time = buffer_time/4 = %u/4 = %u",
			    buffer_time, period_time);
	}

	if (period_time_ro > 0) {
		period_time = period_time_ro;
		err = snd_pcm_hw_params_set_period_time_near(pcm, hwparams,
							     &period_time, nullptr);
		if (err < 0)
			throw FormatRuntimeError("snd_pcm_hw_params_set_period_time_near() failed: %s",
						 snd_strerror(-err));
	}

	err = snd_pcm_hw_params(pcm, hwparams);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params() failed: %s",
					 snd_strerror(-err));

	HwResult result;

	err = snd_pcm_hw_params_get_format(hwparams, &result.format);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params_get_format() failed: %s",
					 snd_strerror(-err));

	err = snd_pcm_hw_params_get_buffer_size(hwparams, &result.buffer_size);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params_get_buffer_size() failed: %s",
					 snd_strerror(-err));

	err = snd_pcm_hw_params_get_period_size(hwparams, &result.period_size,
						nullptr);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_hw_params_get_period_size() failed: %s",
					 snd_strerror(-err));

	return result;
}

} // namespace Alsa
