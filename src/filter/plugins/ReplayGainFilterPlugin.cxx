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

#include "ReplayGainFilterPlugin.hxx"
#include "filter/Filter.hxx"
#include "filter/Prepared.hxx"
#include "ReplayGainInfo.hxx"
#include "ReplayGainConfig.hxx"
#include "mixer/MixerControl.hxx"
#include "pcm/AudioFormat.hxx"
#include "pcm/Volume.hxx"
#include "util/ConstBuffer.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <cassert>
#include <exception>

static constexpr Domain replay_gain_domain("replay_gain");

class ReplayGainFilter final : public Filter {
	const ReplayGainConfig config;

	/**
	 * If set, then this hardware mixer is used for applying
	 * replay gain, instead of the software volume library.
	 */
	Mixer *const mixer;

	/**
	 * The base volume level for scale=1.0, between 1 and 100
	 * (including).
	 */
	const unsigned base;

	ReplayGainMode mode = ReplayGainMode::OFF;

	ReplayGainInfo info;

	/**
	 * About the current volume: it is between 0 and a value that
	 * may or may not exceed #PCM_VOLUME_1.
	 *
	 * If the default value of true is used for replaygain_limit, the
	 * application of the volume to the signal will never cause clipping.
	 *
	 * On the other hand, if the user has set replaygain_limit to false,
	 * the chance of clipping is explicitly preferred if that's required to
	 * maintain a consistent audio level. Whether clipping will actually
	 * occur depends on what value the user is using for replaygain_preamp.
	 */
	PcmVolume pv;

public:
	ReplayGainFilter(const ReplayGainConfig &_config, bool allow_convert,
			 const AudioFormat &audio_format,
			 Mixer *_mixer, unsigned _base)
		:Filter(audio_format),
		 config(_config),
		 mixer(_mixer), base(_base) {
		info.Clear();

		out_audio_format.format = pv.Open(out_audio_format.format,
						  allow_convert);
	}

	void SetInfo(const ReplayGainInfo *_info) {
		if (_info != nullptr)
			info = *_info;
		else
			info.Clear();

		Update();
	}

	void SetMode(ReplayGainMode _mode) {
		if (_mode == mode)
			/* no change */
			return;

		FormatDebug(replay_gain_domain,
			    "replay gain mode has changed %s->%s\n",
			    ToString(mode), ToString(_mode));

		mode = _mode;
		Update();
	}

	/**
	 * Recalculates the new volume after a property was changed.
	 */
	void Update();

	/* virtual methods from class Filter */
	ConstBuffer<void> FilterPCM(ConstBuffer<void> src) override;
};

class PreparedReplayGainFilter final : public PreparedFilter {
	const ReplayGainConfig config;

	/**
	 * If set, then this hardware mixer is used for applying
	 * replay gain, instead of the software volume library.
	 */
	Mixer *mixer = nullptr;

	/**
	 * Allow the class to convert to a different #SampleFormat to
	 * preserve quality?
	 */
	const bool allow_convert;

	/**
	 * The base volume level for scale=1.0, between 1 and 100
	 * (including).
	 */
	unsigned base;

public:
	explicit PreparedReplayGainFilter(const ReplayGainConfig _config,
					  bool _allow_convert)
		:config(_config), allow_convert(_allow_convert) {}

	void SetMixer(Mixer *_mixer, unsigned _base) {
		assert(_mixer == nullptr || (_base > 0 && _base <= 100));

		mixer = _mixer;
		base = _base;
	}

	/* virtual methods from class Filter */
	std::unique_ptr<Filter> Open(AudioFormat &af) override;
};

void
ReplayGainFilter::Update()
{
	unsigned volume = PCM_VOLUME_1;
	if (mode != ReplayGainMode::OFF) {
		const auto &tuple = info.Get(mode);
		float scale = tuple.CalculateScale(config);
		FormatDebug(replay_gain_domain,
			    "scale=%f\n", double(scale));

		volume = pcm_float_to_volume(scale);
	}

	if (mixer != nullptr) {
		/* update the hardware mixer volume */

		unsigned _volume = (volume * base) / PCM_VOLUME_1;
		if (_volume > 100)
			_volume = 100;

		try {
			mixer_set_volume(mixer, _volume);
		} catch (...) {
			LogError(std::current_exception(),
				 "Failed to update hardware mixer");
		}
	} else
		pv.SetVolume(volume);
}

std::unique_ptr<PreparedFilter>
NewReplayGainFilter(const ReplayGainConfig &config,
		    bool allow_convert) noexcept
{
	return std::make_unique<PreparedReplayGainFilter>(config,
							  allow_convert);
}

std::unique_ptr<Filter>
PreparedReplayGainFilter::Open(AudioFormat &af)
{
	return std::make_unique<ReplayGainFilter>(config, allow_convert,
						  af, mixer, base);
}

ConstBuffer<void>
ReplayGainFilter::FilterPCM(ConstBuffer<void> src)
{
	return mixer != nullptr
		? src
		: pv.Apply(src);
}

void
replay_gain_filter_set_mixer(PreparedFilter &_filter, Mixer *mixer,
			     unsigned base)
{
	auto &filter = (PreparedReplayGainFilter &)_filter;

	filter.SetMixer(mixer, base);
}

void
replay_gain_filter_set_info(Filter &_filter, const ReplayGainInfo *info)
{
	auto &filter = (ReplayGainFilter &)_filter;

	filter.SetInfo(info);
}

void
replay_gain_filter_set_mode(Filter &_filter, ReplayGainMode mode)
{
	auto &filter = (ReplayGainFilter &)_filter;

	filter.SetMode(mode);
}
