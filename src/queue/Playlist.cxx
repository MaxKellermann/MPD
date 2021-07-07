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

#include "Playlist.hxx"
#include "Listener.hxx"
#include "PlaylistError.hxx"
#include "player/Control.hxx"
#include "song/DetachedSong.hxx"
#include "SingleMode.hxx"
#include "Log.hxx"

#include <cassert>

void
playlist::TagModified(DetachedSong &&song) noexcept
{
	if (!playing)
		return;

	assert(current >= 0);

	DetachedSong &current_song = queue.GetOrder(current);
	if (song.IsSame(current_song))
		current_song.MoveTagItemsFrom(std::move(song));

	queue.ModifyAtOrder(current);
	OnModified();
}

void
playlist::TagModified(const char *real_uri, const Tag &tag) noexcept
{
	bool modified = false;

	for (unsigned i = 0; i < queue.length; ++i) {
		auto &song = *queue.items[i].song;
		if (song.IsRealURI(real_uri)) {
			song.SetTag(tag);
			queue.ModifyAtPosition(i);
			modified = true;
		}
	}

	if (modified)
		OnModified();
}

inline void
playlist::QueueSongOrder(PlayerControl &pc, unsigned order) noexcept

{
	assert(queue.IsValidOrder(order));

	queued = order;

	const DetachedSong &song = queue.GetOrder(order);

	FmtDebug(playlist_domain, "queue song {}:\"{}\"",
		 queued, song.GetURI());

	pc.LockEnqueueSong(std::make_unique<DetachedSong>(song));
}

void
playlist::SongStarted() noexcept
{
	assert(current >= 0);

	/* reset a song's "priority" when playback starts */
	if (queue.SetPriority(queue.OrderToPosition(current), 0, -1, false))
		OnModified();
}

inline void
playlist::QueuedSongStarted(PlayerControl &pc) noexcept
{
	assert(!pc.LockGetSyncInfo().has_next_song);
	assert(queued >= -1);
	assert(current >= 0);

	/* queued song has started: copy queued to current,
	   and notify the clients */

	const int old_current = current;
	current = queued;
	queued = -1;

	if (queue.consume)
		DeleteOrder(pc, old_current);

	listener.OnQueueSongStarted();

	SongStarted();
}

const DetachedSong *
playlist::GetQueuedSong() const noexcept
{
	return playing && queued >= 0
		? &queue.GetOrder(queued)
		: nullptr;
}

void
playlist::UpdateQueuedSong(PlayerControl &pc,
			   const DetachedSong *prev) noexcept
{
	if (!playing)
		return;

	if (prev == nullptr && bulk_edit)
		/* postponed until CommitBulk() to avoid always
		   queueing the first song that is being added (in
		   random mode) */
		return;

	assert(!queue.IsEmpty());
	assert((queued < 0) == (prev == nullptr));

	const int next_order = current >= 0
		? queue.GetNextOrder(current)
		: 0;

	if (next_order == 0 && queue.random && queue.single == SingleMode::OFF) {
		/* shuffle the song order again, so we get a different
		   order each time the playlist is played
		   completely */
		const unsigned current_position =
			queue.OrderToPosition(current);

		queue.ShuffleOrder();

		/* make sure that the current still points to
		   the current song, after the song order has been
		   shuffled */
		current = queue.PositionToOrder(current_position);
	}

	const DetachedSong *const next_song = next_order >= 0
		? &queue.GetOrder(next_order)
		: nullptr;

	if (prev != nullptr && next_song != prev) {
		/* clear the currently queued song */
		pc.LockCancel();
		queued = -1;
	}

	if (next_order >= 0) {
		if (next_song != prev)
			QueueSongOrder(pc, next_order);
		else
			queued = next_order;
	}
}

void
playlist::PlayOrder(PlayerControl &pc, unsigned order)
{
	playing = true;
	queued = -1;

	const DetachedSong &song = queue.GetOrder(order);

	FmtDebug(playlist_domain, "play {}:\"{}\"", order, song.GetURI());

	current = order;

	pc.Play(std::make_unique<DetachedSong>(song));

	SongStarted();
}

void
playlist::SyncWithPlayer(PlayerControl &pc) noexcept
{
	if (!playing)
		/* this event has reached us out of sync: we aren't
		   playing anymore; ignore the event */
		return;

	const auto i = pc.LockGetSyncInfo();

	if (i.state == PlayerState::STOP)
		/* the player thread has stopped: check if playback
		   should be restarted with the next song.  That can
		   happen if the playlist isn't filling the queue fast
		   enough */
		ResumePlayback(pc);
	else {
		/* check if the player thread has already started
		   playing the queued song */
		if (!i.has_next_song && queued != -1)
			QueuedSongStarted(pc);

		/* make sure the queued song is always set (if
		   possible) */
		if (!pc.LockGetSyncInfo().has_next_song && queued < 0)
			UpdateQueuedSong(pc, nullptr);
	}
}

inline void
playlist::ResumePlayback(PlayerControl &pc) noexcept
{
	assert(playing);
	assert(pc.GetState() == PlayerState::STOP);

	const auto error = pc.GetErrorType();
	if (error == PlayerError::NONE)
		error_count = 0;
	else
		++error_count;

	if ((stop_on_error && error != PlayerError::NONE) ||
	    error == PlayerError::OUTPUT ||
	    error_count >= queue.GetLength())
		/* too many errors, or critical error: stop
		   playback */
		Stop(pc);
	else
		/* continue playback at the next song */
		try {
			PlayNext(pc);
		} catch (...) {
			/* TODO: log error? */
		}
}

void
playlist::SetRepeat(PlayerControl &pc, bool status) noexcept
{
	if (status == queue.repeat)
		return;

	queue.repeat = status;

	pc.LockSetBorderPause(queue.single != SingleMode::OFF && !queue.repeat);

	/* if the last song is currently being played, the "next song"
	   might change when repeat mode is toggled */
	UpdateQueuedSong(pc, GetQueuedSong());

	listener.OnQueueOptionsChanged();
}

static void
playlist_order(playlist &playlist) noexcept
{
	if (playlist.current >= 0)
		/* update playlist.current, order==position now */
		playlist.current = playlist.queue.OrderToPosition(playlist.current);

	playlist.queue.RestoreOrder();
}

void
playlist::SetSingle(PlayerControl &pc, SingleMode status) noexcept
{
	if (status == queue.single)
		return;

	queue.single = status;


	pc.LockSetBorderPause(queue.single != SingleMode::OFF && !queue.repeat);

	/* if the last song is currently being played, the "next song"
	   might change when single mode is toggled */
	UpdateQueuedSong(pc, GetQueuedSong());

	listener.OnQueueOptionsChanged();
}

void
playlist::SetConsume(bool status) noexcept
{
	if (status == queue.consume)
		return;

	queue.consume = status;
	listener.OnQueueOptionsChanged();
}

void
playlist::SetRandom(PlayerControl &pc, bool status) noexcept
{
	if (status == queue.random)
		return;

	const DetachedSong *const queued_song = GetQueuedSong();

	queue.random = status;

	if (queue.random) {
		/* shuffle the queue order, but preserve current */

		const int current_position = playing
			? GetCurrentPosition()
			: -1;

		queue.ShuffleOrder();

		if (current_position >= 0) {
			/* make sure the current song is the first in
			   the order list, so the whole rest of the
			   playlist is played after that */
			unsigned current_order =
				queue.PositionToOrder(current_position);
			current = queue.MoveOrder(current_order, 0);
		} else
			current = -1;
	} else
		playlist_order(*this);

	UpdateQueuedSong(pc, queued_song);

	listener.OnQueueOptionsChanged();
}

int
playlist::GetCurrentPosition() const noexcept
{
	return current >= 0
		? queue.OrderToPosition(current)
		: -1;
}

int
playlist::GetNextPosition(unsigned index) const noexcept
{
	if (current < 0)
		return -1;

	if (queue.single != SingleMode::OFF && queue.repeat)
		return queue.OrderToPosition(current);
	else if (queue.IsValidOrder(current + index))
		return queue.OrderToPosition(current + index);
	else if (queue.repeat)
		return queue.OrderToPosition(0);

	return -1;
}

void
playlist::BorderPause(PlayerControl &pc) noexcept
{
	if (queue.single == SingleMode::ONE_SHOT) {
		queue.single = SingleMode::OFF;
		pc.LockSetBorderPause(false);

		listener.OnQueueOptionsChanged();
	}
}
