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

/*
 * Saving and loading the playlist to/from the state file.
 *
 */

#include "PlaylistState.hxx"
#include "PlaylistError.hxx"
#include "Playlist.hxx"
#include "SingleMode.hxx"
#include "StateFileConfig.hxx"
#include "queue/QueueSave.hxx"
#include "fs/io/TextFile.hxx"
#include "fs/io/BufferedOutputStream.hxx"
#include "player/Control.hxx"
#include "util/CharUtil.hxx"
#include "util/StringAPI.hxx"
#include "util/StringCompare.hxx"
#include "util/NumberParser.hxx"
#include "Log.hxx"

#include <string.h>
#include <stdlib.h>

#define PLAYLIST_STATE_FILE_STATE		"state: "
#define PLAYLIST_STATE_FILE_RANDOM		"random: "
#define PLAYLIST_STATE_FILE_REPEAT		"repeat: "
#define PLAYLIST_STATE_FILE_SINGLE		"single: "
#define PLAYLIST_STATE_FILE_CONSUME		"consume: "
#define PLAYLIST_STATE_FILE_CURRENT		"current: "
#define PLAYLIST_STATE_FILE_TIME		"time: "
#define PLAYLIST_STATE_FILE_CROSSFADE		"crossfade: "
#define PLAYLIST_STATE_FILE_MIXRAMPDB		"mixrampdb: "
#define PLAYLIST_STATE_FILE_MIXRAMPDELAY	"mixrampdelay: "
#define PLAYLIST_STATE_FILE_PLAYLIST_BEGIN	"playlist_begin"
#define PLAYLIST_STATE_FILE_PLAYLIST_END	"playlist_end"

#define PLAYLIST_STATE_FILE_STATE_PLAY		"play"
#define PLAYLIST_STATE_FILE_STATE_PAUSE		"pause"
#define PLAYLIST_STATE_FILE_STATE_STOP		"stop"

void
playlist_state_save(BufferedOutputStream &os, const struct playlist &playlist,
		    PlayerControl &pc)
{
	const auto player_status = pc.LockGetStatus();

	os.Write(PLAYLIST_STATE_FILE_STATE);

	if (playlist.playing) {
		switch (player_status.state) {
		case PlayerState::PAUSE:
			os.Write(PLAYLIST_STATE_FILE_STATE_PAUSE "\n");
			break;
		default:
			os.Write(PLAYLIST_STATE_FILE_STATE_PLAY "\n");
		}
		os.Format(PLAYLIST_STATE_FILE_CURRENT "%i\n",
			  playlist.queue.OrderToPosition(playlist.current));
		os.Format(PLAYLIST_STATE_FILE_TIME "%f\n",
			  player_status.elapsed_time.ToDoubleS());
	} else {
		os.Write(PLAYLIST_STATE_FILE_STATE_STOP "\n");

		if (playlist.current >= 0)
			os.Format(PLAYLIST_STATE_FILE_CURRENT "%i\n",
				playlist.queue.OrderToPosition(playlist.current));
	}

	os.Format(PLAYLIST_STATE_FILE_RANDOM "%i\n", playlist.queue.random);
	os.Format(PLAYLIST_STATE_FILE_REPEAT "%i\n", playlist.queue.repeat);
	os.Format(PLAYLIST_STATE_FILE_SINGLE "%i\n",
			  int(playlist.queue.single));
	os.Format(PLAYLIST_STATE_FILE_CONSUME "%i\n", playlist.queue.consume);
	os.Format(PLAYLIST_STATE_FILE_CROSSFADE "%i\n",
		  int(pc.GetCrossFade().count()));
	os.Format(PLAYLIST_STATE_FILE_MIXRAMPDB "%f\n",
		  double(pc.GetMixRampDb()));
	os.Format(PLAYLIST_STATE_FILE_MIXRAMPDELAY "%f\n",
		  pc.GetMixRampDelay().count());
	os.Write(PLAYLIST_STATE_FILE_PLAYLIST_BEGIN "\n");
	queue_save(os, playlist.queue);
	os.Write(PLAYLIST_STATE_FILE_PLAYLIST_END "\n");
}

static void
playlist_state_load(TextFile &file, const SongLoader &song_loader,
		    struct playlist &playlist)
{
	const char *line = file.ReadLine();
	if (line == nullptr) {
		LogWarning(playlist_domain, "No playlist in state file");
		return;
	}

	while (!StringStartsWith(line, PLAYLIST_STATE_FILE_PLAYLIST_END)) {
		queue_load_song(file, song_loader, line, playlist.queue);

		line = file.ReadLine();
		if (line == nullptr) {
			LogWarning(playlist_domain,
				   "'" PLAYLIST_STATE_FILE_PLAYLIST_END
				   "' not found in state file");
			break;
		}
	}

	playlist.queue.IncrementVersion();
}

bool
playlist_state_restore(const StateFileConfig &config,
		       const char *line, TextFile &file,
		       const SongLoader &song_loader,
		       struct playlist &playlist, PlayerControl &pc)
{
	int current = -1;
	SongTime seek_time = SongTime::zero();
	bool random_mode = false;

	line = StringAfterPrefix(line, PLAYLIST_STATE_FILE_STATE);
	if (line == nullptr)
		return false;

	PlayerState state;
	if (strcmp(line, PLAYLIST_STATE_FILE_STATE_PLAY) == 0)
		state = PlayerState::PLAY;
	else if (strcmp(line, PLAYLIST_STATE_FILE_STATE_PAUSE) == 0)
		state = PlayerState::PAUSE;
	else
		state = PlayerState::STOP;

	while ((line = file.ReadLine()) != nullptr) {
		const char *p;
		if ((p = StringAfterPrefix(line, PLAYLIST_STATE_FILE_TIME))) {
			seek_time = SongTime::FromS(ParseDouble(p));
		} else if ((p = StringAfterPrefix(line, PLAYLIST_STATE_FILE_REPEAT))) {
			playlist.SetRepeat(pc, StringIsEqual(p, "1"));
		} else if ((p = StringAfterPrefix(line, PLAYLIST_STATE_FILE_SINGLE))) {
			playlist.SetSingle(pc, SingleFromString(p));
		} else if ((p = StringAfterPrefix(line, PLAYLIST_STATE_FILE_CONSUME))) {
			playlist.SetConsume(StringIsEqual(p, "1"));
		} else if ((p = StringAfterPrefix(line, PLAYLIST_STATE_FILE_CROSSFADE))) {
			pc.SetCrossFade(FloatDuration(atoi(p)));
		} else if ((p = StringAfterPrefix(line, PLAYLIST_STATE_FILE_MIXRAMPDB))) {
			pc.SetMixRampDb(ParseFloat(p));
		} else if ((p = StringAfterPrefix(line, PLAYLIST_STATE_FILE_MIXRAMPDELAY))) {
			/* this check discards "nan" which was used
			   prior to MPD 0.18 */
			if (IsDigitASCII(*p))
				pc.SetMixRampDelay(FloatDuration(ParseFloat(p)));
		} else if ((p = StringAfterPrefix(line, PLAYLIST_STATE_FILE_RANDOM))) {
			random_mode = StringIsEqual(p, "1");
		} else if ((p = StringAfterPrefix(line, PLAYLIST_STATE_FILE_CURRENT))) {
			current = atoi(p);
		} else if (StringStartsWith(line,
					    PLAYLIST_STATE_FILE_PLAYLIST_BEGIN)) {
			playlist_state_load(file, song_loader, playlist);
		}
	}

	playlist.SetRandom(pc, random_mode);

	if (!playlist.queue.IsEmpty()) {
		if (!playlist.queue.IsValidPosition(current))
			current = 0;

		if (state == PlayerState::PLAY && config.restore_paused)
			/* the user doesn't want MPD to auto-start
			   playback after startup; fall back to
			   "pause" */
			state = PlayerState::PAUSE;

		/* enable all devices for the first time; this must be
		   called here, after the audio output states were
		   restored, before playback begins */
		if (state != PlayerState::STOP)
			pc.LockUpdateAudio();

		if (state == PlayerState::STOP /* && config_option */)
			playlist.current = current;
		else if (seek_time.count() == 0) {
			try {
				playlist.PlayPosition(pc, current);
			} catch (...) {
				/* TODO: log error? */
			}
		} else {
			try {
				playlist.SeekSongPosition(pc, current,
							  seek_time);
			} catch (...) {
				/* TODO: log error? */
			}
		}

		if (state == PlayerState::PAUSE)
			pc.LockPause();
	}

	return true;
}

unsigned
playlist_state_get_hash(const playlist &playlist,
			PlayerControl &pc)
{
	const auto player_status = pc.LockGetStatus();

	return playlist.queue.version ^
		(player_status.state != PlayerState::STOP
		 ? (player_status.elapsed_time.ToS() << 8)
		 : 0) ^
		(playlist.current >= 0
		 ? (playlist.queue.OrderToPosition(playlist.current) << 16)
		 : 0) ^
		(int(pc.GetCrossFade().count()) << 20) ^
		(unsigned(player_status.state) << 24) ^
		/* note that this takes 2 bits */
		(int(playlist.queue.single) << 25) ^
		(playlist.queue.random << 27) ^
		(playlist.queue.repeat << 28) ^
		(playlist.queue.consume << 30) ^
		(playlist.queue.random << 31);
}
