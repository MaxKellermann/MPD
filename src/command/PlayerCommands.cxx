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

#include "config.h"
#include "PlayerCommands.hxx"
#include "Request.hxx"
#include "queue/Playlist.hxx"
#include "PlaylistPrint.hxx"
#include "SingleMode.hxx"
#include "client/Client.hxx"
#include "client/Response.hxx"
#include "mixer/Volume.hxx"
#include "Partition.hxx"
#include "Instance.hxx"
#include "IdleFlags.hxx"
#include "util/StringBuffer.hxx"
#include "util/ScopeExit.hxx"
#include "util/Exception.hxx"
#include "util/Math.hxx"

#ifdef ENABLE_DATABASE
#include "db/update/Service.hxx"
#endif

#include <fmt/format.h>

#define COMMAND_STATUS_STATE            "state"
#define COMMAND_STATUS_REPEAT           "repeat"
#define COMMAND_STATUS_SINGLE           "single"
#define COMMAND_STATUS_CONSUME          "consume"
#define COMMAND_STATUS_RANDOM           "random"
#define COMMAND_STATUS_PLAYLIST         "playlist"
#define COMMAND_STATUS_PLAYLIST_LENGTH  "playlistlength"
#define COMMAND_STATUS_SONG             "song"
#define COMMAND_STATUS_SONGID           "songid"
#define COMMAND_STATUS_NEXTSONG         "nextsong"
#define COMMAND_STATUS_NEXTSONGID       "nextsongid"
#define COMMAND_STATUS_TIME             "time"
#define COMMAND_STATUS_BITRATE          "bitrate"
#define COMMAND_STATUS_ERROR            "error"
#define COMMAND_STATUS_CROSSFADE	"xfade"
#define COMMAND_STATUS_MIXRAMPDB	"mixrampdb"
#define COMMAND_STATUS_MIXRAMPDELAY	"mixrampdelay"
#define COMMAND_STATUS_AUDIO		"audio"
#define COMMAND_STATUS_UPDATING_DB	"updating_db"

CommandResult
handle_play(Client &client, Request args, [[maybe_unused]] Response &r)
{
	int song = args.ParseOptional(0, -1);

	client.GetPartition().PlayPosition(song);
	return CommandResult::OK;
}

CommandResult
handle_playid(Client &client, Request args, [[maybe_unused]] Response &r)
{
	int id = args.ParseOptional(0, -1);

	client.GetPartition().PlayId(id);
	return CommandResult::OK;
}

CommandResult
handle_stop(Client &client, [[maybe_unused]] Request args, [[maybe_unused]] Response &r)
{
	client.GetPartition().Stop();
	return CommandResult::OK;
}

CommandResult
handle_currentsong(Client &client, [[maybe_unused]] Request args, Response &r)
{
	playlist_print_current(r, client.GetPlaylist());
	return CommandResult::OK;
}

CommandResult
handle_pause(Client &client, Request args, [[maybe_unused]] Response &r)
{
	auto &pc = client.GetPlayerControl();

	if (!args.empty()) {
		bool pause_flag = args.ParseBool(0);
		pc.LockSetPause(pause_flag);
	} else
		pc.LockPause();

	return CommandResult::OK;
}

CommandResult
handle_status(Client &client, [[maybe_unused]] Request args, Response &r)
{
	auto &partition = client.GetPartition();
	auto &pc = partition.pc;

	const char *state = nullptr;
	int song;

	const auto player_status = pc.LockGetStatus();

	switch (player_status.state) {
	case PlayerState::STOP:
		state = "stop";
		break;
	case PlayerState::PAUSE:
		state = "pause";
		break;
	case PlayerState::PLAY:
		state = "play";
		break;
	}

	const auto &playlist = partition.playlist;

	const auto volume = volume_level_get(partition.outputs);
	if (volume >= 0)
		r.Fmt(FMT_STRING("volume: {}\n"), volume);

	r.Fmt(FMT_STRING(COMMAND_STATUS_REPEAT ": {}\n"
			 COMMAND_STATUS_RANDOM ": {}\n"
			 COMMAND_STATUS_SINGLE ": {}\n"
			 COMMAND_STATUS_CONSUME ": {}\n"
			 "partition: {}\n"
			 COMMAND_STATUS_PLAYLIST ": {}\n"
			 COMMAND_STATUS_PLAYLIST_LENGTH ": {}\n"
			 COMMAND_STATUS_MIXRAMPDB ": {}\n"
			 COMMAND_STATUS_STATE ": {}\n"),
	      (unsigned)playlist.GetRepeat(),
	      (unsigned)playlist.GetRandom(),
	      SingleToString(playlist.GetSingle()),
	      (unsigned)playlist.GetConsume(),
	      partition.name.c_str(),
	      playlist.GetVersion(),
	      playlist.GetLength(),
	      pc.GetMixRampDb(),
	      state);

	if (pc.GetCrossFade() > FloatDuration::zero())
		r.Fmt(FMT_STRING(COMMAND_STATUS_CROSSFADE ": {}\n"),
		      lround(pc.GetCrossFade().count()));

	if (pc.GetMixRampDelay() > FloatDuration::zero())
		r.Fmt(FMT_STRING(COMMAND_STATUS_MIXRAMPDELAY ": {}\n"),
		      pc.GetMixRampDelay().count());

	song = playlist.GetCurrentPosition();
	if (song >= 0) {
		r.Fmt(FMT_STRING(COMMAND_STATUS_SONG ": {}\n"
				 COMMAND_STATUS_SONGID ": {}\n"),
		      song, playlist.PositionToId(song));
	}

	if (player_status.state != PlayerState::STOP) {
		r.Fmt(FMT_STRING(COMMAND_STATUS_TIME ": {}:{}\n"
				 "elapsed: {:1.3}\n"
				 COMMAND_STATUS_BITRATE ": {}\n"),
		      player_status.elapsed_time.RoundS(),
		      player_status.total_time.IsNegative()
		      ? 0U
		      : unsigned(player_status.total_time.RoundS()),
		      player_status.elapsed_time.ToDoubleS(),
		      player_status.bit_rate);

		if (!player_status.total_time.IsNegative())
			r.Fmt(FMT_STRING("duration: {:1.3}\n"),
				 player_status.total_time.ToDoubleS());

		if (player_status.audio_format.IsDefined())
			r.Fmt(FMT_STRING(COMMAND_STATUS_AUDIO ": {}\n"),
			      ToString(player_status.audio_format));
	}

#ifdef ENABLE_DATABASE
	const UpdateService *update_service = partition.instance.update;
	unsigned updateJobId = update_service != nullptr
		? update_service->GetId()
		: 0;
	if (updateJobId != 0) {
		r.Fmt(FMT_STRING(COMMAND_STATUS_UPDATING_DB ": {}\n"),
		      updateJobId);
	}
#endif

	try {
		pc.LockCheckRethrowError();
	} catch (...) {
		r.Fmt(FMT_STRING(COMMAND_STATUS_ERROR ": {}\n"),
		      GetFullMessage(std::current_exception()));
	}

	song = playlist.GetNextPosition(1);
	if (song >= 0)
		r.Fmt(FMT_STRING(COMMAND_STATUS_NEXTSONG ": {}\n"
				 COMMAND_STATUS_NEXTSONGID ": {}\n"),
		      song, playlist.PositionToId(song));

	return CommandResult::OK;
}

CommandResult
handle_next(Client &client, [[maybe_unused]] Request args, [[maybe_unused]] Response &r)
{
	playlist &playlist = client.GetPlaylist();

	/* single mode is not considered when this is user who
	 * wants to change song. */
	const SingleMode single = playlist.queue.single;
	playlist.queue.single = SingleMode::OFF;

	AtScopeExit(&playlist, single) {
		playlist.queue.single = single;
	};

	client.GetPartition().PlayNext();
	return CommandResult::OK;
}

CommandResult
handle_previous(Client &client, [[maybe_unused]] Request args,
		[[maybe_unused]] Response &r)
{
	client.GetPartition().PlayPrevious();
	return CommandResult::OK;
}

CommandResult
handle_repeat(Client &client, Request args, [[maybe_unused]] Response &r)
{
	bool status = args.ParseBool(0);
	client.GetPartition().SetRepeat(status);
	return CommandResult::OK;
}

CommandResult
handle_single(Client &client, Request args, [[maybe_unused]] Response &r)
{
	auto new_mode = SingleFromString(args.front());
	client.GetPartition().SetSingle(new_mode);
	return CommandResult::OK;
}

CommandResult
handle_consume(Client &client, Request args, [[maybe_unused]] Response &r)
{
	bool status = args.ParseBool(0);
	client.GetPartition().SetConsume(status);
	return CommandResult::OK;
}

CommandResult
handle_random(Client &client, Request args, [[maybe_unused]] Response &r)
{
	bool status = args.ParseBool(0);
	auto &partition = client.GetPartition();
	partition.SetRandom(status);
	partition.UpdateEffectiveReplayGainMode();
	return CommandResult::OK;
}

CommandResult
handle_clearerror(Client &client, [[maybe_unused]] Request args,
		  [[maybe_unused]] Response &r)
{
	client.GetPlayerControl().LockClearError();
	return CommandResult::OK;
}

CommandResult
handle_seek(Client &client, Request args, [[maybe_unused]] Response &r)
{
	unsigned song = args.ParseUnsigned(0);
	SongTime seek_time = args.ParseSongTime(1);

	client.GetPartition().SeekSongPosition(song, seek_time);
	return CommandResult::OK;
}

CommandResult
handle_seekid(Client &client, Request args, [[maybe_unused]] Response &r)
{
	unsigned id = args.ParseUnsigned(0);
	SongTime seek_time = args.ParseSongTime(1);

	client.GetPartition().SeekSongId(id, seek_time);
	return CommandResult::OK;
}

CommandResult
handle_seekcur(Client &client, Request args, [[maybe_unused]] Response &r)
{
	const char *p = args.front();
	bool relative = *p == '+' || *p == '-';
	SignedSongTime seek_time = ParseCommandArgSignedSongTime(p);

	client.GetPartition().SeekCurrent(seek_time, relative);
	return CommandResult::OK;
}

CommandResult
handle_crossfade(Client &client, Request args, [[maybe_unused]] Response &r)
{
	FloatDuration duration{args.ParseUnsigned(0)};
	client.GetPlayerControl().SetCrossFade(duration);
	return CommandResult::OK;
}

CommandResult
handle_mixrampdb(Client &client, Request args, [[maybe_unused]] Response &r)
{
	float db = args.ParseFloat(0);
	client.GetPlayerControl().SetMixRampDb(db);
	return CommandResult::OK;
}

CommandResult
handle_mixrampdelay(Client &client, Request args, [[maybe_unused]] Response &r)
{
	FloatDuration delay_secs{args.ParseFloat(0)};
	client.GetPlayerControl().SetMixRampDelay(delay_secs);
	return CommandResult::OK;
}

CommandResult
handle_replay_gain_mode(Client &client, Request args, Response &)
{
	auto new_mode = FromString(args.front());
	auto &partition = client.GetPartition();
	partition.SetReplayGainMode(new_mode);
	partition.EmitIdle(IDLE_OPTIONS);
	return CommandResult::OK;
}

CommandResult
handle_replay_gain_status(Client &client, [[maybe_unused]] Request args,
			  Response &r)
{
	r.Fmt(FMT_STRING("replay_gain_mode: {}\n"),
	      ToString(client.GetPartition().replay_gain_mode));
	return CommandResult::OK;
}
