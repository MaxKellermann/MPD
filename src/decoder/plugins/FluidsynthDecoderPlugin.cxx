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

#include "FluidsynthDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "fs/NarrowPath.hxx"
#include "fs/Path.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <fluidsynth.h>

static constexpr Domain fluidsynth_domain("fluidsynth");

static unsigned sample_rate;
static const char *soundfont_path;

/**
 * Convert a fluidsynth log level to a MPD log level.
 */
static LogLevel
fluidsynth_level_to_mpd(enum fluid_log_level level)
{
	switch (level) {
	case FLUID_PANIC:
	case FLUID_ERR:
		return LogLevel::ERROR;

	case FLUID_WARN:
		return LogLevel::WARNING;

	case FLUID_INFO:
		return LogLevel::INFO;

	case FLUID_DBG:
	case LAST_LOG_LEVEL:
		return LogLevel::DEBUG;
	}

	/* invalid fluidsynth log level */
	return LogLevel::INFO;
}

/**
 * The fluidsynth logging callback.  It forwards messages to the MPD
 * logging library.
 */
static void
fluidsynth_mpd_log_function(int level,
#if FLUIDSYNTH_VERSION_MAJOR >= 2
			    const
#endif
			    char *message,
			    void *)
{
	Log(fluidsynth_level_to_mpd(fluid_log_level(level)),
	    fluidsynth_domain,
	    message);
}

static bool
fluidsynth_init(const ConfigBlock &block)
{
	sample_rate = block.GetPositiveValue("sample_rate", 48000U);
	CheckSampleRate(sample_rate);

	soundfont_path = block.GetBlockValue("soundfont",
					     "/usr/share/sounds/sf2/FluidR3_GM.sf2");

	fluid_set_log_function(LAST_LOG_LEVEL,
			       fluidsynth_mpd_log_function, nullptr);

	return true;
}

static void
fluidsynth_file_decode(DecoderClient &client, Path path_fs)
{
	char setting_sample_rate[] = "synth.sample-rate";
	/*
	char setting_verbose[] = "synth.verbose";
	char setting_yes[] = "yes";
	*/
	fluid_settings_t *settings;
	fluid_synth_t *synth;
	fluid_player_t *player;
	int ret;

	/* set up fluid settings */

	settings = new_fluid_settings();
	if (settings == nullptr)
		return;

	fluid_settings_setnum(settings, setting_sample_rate, sample_rate);

	/*
	fluid_settings_setstr(settings, setting_verbose, setting_yes);
	*/

	/* create the fluid synth */

	synth = new_fluid_synth(settings);
	if (synth == nullptr) {
		delete_fluid_settings(settings);
		return;
	}

	ret = fluid_synth_sfload(synth, soundfont_path, true);
	if (ret < 0) {
		LogWarning(fluidsynth_domain, "fluid_synth_sfload() failed");
		delete_fluid_synth(synth);
		delete_fluid_settings(settings);
		return;
	}

	/* create the fluid player */

	player = new_fluid_player(synth);
	if (player == nullptr) {
		delete_fluid_synth(synth);
		delete_fluid_settings(settings);
		return;
	}

	ret = fluid_player_add(player, NarrowPath(path_fs));
	if (ret != 0) {
		LogWarning(fluidsynth_domain, "fluid_player_add() failed");
		delete_fluid_player(player);
		delete_fluid_synth(synth);
		delete_fluid_settings(settings);
		return;
	}

	/* start the player */

	ret = fluid_player_play(player);
	if (ret != 0) {
		LogWarning(fluidsynth_domain, "fluid_player_play() failed");
		delete_fluid_player(player);
		delete_fluid_synth(synth);
		delete_fluid_settings(settings);
		return;
	}

	/* initialization complete - announce the audio format to the
	   MPD core */

	const AudioFormat audio_format(sample_rate, SampleFormat::S16, 2);
	client.Ready(audio_format, false, SignedSongTime::Negative());

	DecoderCommand cmd;
	while (fluid_player_get_status(player) == FLUID_PLAYER_PLAYING) {
		int16_t buffer[2048];
		const unsigned max_frames = std::size(buffer) / 2;

		/* read samples from fluidsynth and send them to the
		   MPD core */

		ret = fluid_synth_write_s16(synth, max_frames,
					    buffer, 0, 2,
					    buffer, 1, 2);
		if (ret != 0)
			break;

		cmd = client.SubmitData(nullptr, buffer, sizeof(buffer), 0);
		if (cmd != DecoderCommand::NONE)
			break;
	}

	/* clean up */

	fluid_player_stop(player);
	fluid_player_join(player);

	delete_fluid_player(player);
	delete_fluid_synth(synth);
	delete_fluid_settings(settings);
}

static bool
fluidsynth_scan_file(Path path_fs,
		     [[maybe_unused]] TagHandler &handler) noexcept
{
	return fluid_is_midifile(NarrowPath(path_fs));
}

static const char *const fluidsynth_suffixes[] = {
	"mid",
	nullptr
};

constexpr DecoderPlugin fluidsynth_decoder_plugin =
	DecoderPlugin("fluidsynth",
		      fluidsynth_file_decode, fluidsynth_scan_file)
	.WithInit(fluidsynth_init)
	.WithSuffixes(fluidsynth_suffixes);
