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

#ifndef MPD_PLAYLIST_HXX
#define MPD_PLAYLIST_HXX

#include "SingleMode.hxx"
#include "queue/Queue.hxx"
#include "config.h"

enum TagType : uint8_t;
struct Tag;
struct RangeArg;
class PlayerControl;
class DetachedSong;
class Database;
class SongLoader;
class SongTime;
class SignedSongTime;
class QueueListener;

struct playlist {
	/**
	 * The song queue - it contains the "real" playlist.
	 */
	Queue queue;

	QueueListener &listener;

	/**
	 * This value is true if the player is currently playing (or
	 * should be playing).
	 */
	bool playing = false;

	/**
	 * If true, then any error is fatal; if false, MPD will
	 * attempt to play the next song on non-fatal errors.  During
	 * seeking, this flag is set.
	 */
	bool stop_on_error;

	/**
	 * If true, then a bulk edit has been initiated by
	 * BeginBulk(), and UpdateQueuedSong() and OnModified() will
	 * be postponed until CommitBulk()
	 */
	bool bulk_edit = false;

	/**
	 * Has the queue been modified during bulk edit mode?
	 */
	bool bulk_modified;

	/**
	 * Number of errors since playback was started.  If this
	 * number exceeds the length of the playlist, MPD gives up,
	 * because all songs have been tried.
	 */
	unsigned error_count;

	/**
	 * The "current song pointer" (the order number).  This is the
	 * song which is played when we get the "play" command.  It is
	 * also the song which is currently being played.
	 */
	int current = -1;

	/**
	 * The "next" song to be played (the order number), when the
	 * current one finishes.  The decoder thread may start
	 * decoding and buffering it, while the "current" song is
	 * still playing.
	 *
	 * This variable is only valid if #playing is true.
	 */
	int queued = -1;

	playlist(unsigned max_length,
		 QueueListener &_listener) noexcept
		:queue(max_length),
		 listener(_listener)
	{
	}

	uint32_t GetVersion() const noexcept {
		return queue.version;
	}

	unsigned GetLength() const noexcept {
		return queue.GetLength();
	}

	unsigned PositionToId(unsigned position) const noexcept {
		return queue.PositionToId(position);
	}

	gcc_pure
	int GetCurrentPosition() const noexcept;

	gcc_pure
	int GetNextPosition(unsigned index) const noexcept;

	/**
	 * Returns the song object which is currently queued.  Returns
	 * none if there is none (yet?) or if MPD isn't playing.
	 */
	gcc_pure
	const DetachedSong *GetQueuedSong() const noexcept;

	/**
	 * This is the "PLAYLIST" event handler.  It is invoked by the
	 * player thread whenever it requests a new queued song, or
	 * when it exits.
	 */
	void SyncWithPlayer(PlayerControl &pc) noexcept;

	/**
	 * This is the "BORDER_PAUSE" event handler.  It is invoked by
	 * the player thread whenever playback goes into border pause.
	 */
	void BorderPause(PlayerControl &pc) noexcept;

protected:
	/**
	 * Called by all editing methods after a modification.
	 * Updates the queue version and invokes
	 * QueueListener::OnQueueModified().
	 */
	void OnModified() noexcept;

	/**
	 * Called when playback of a new song starts.  Unlike
	 * QueuedSongStarted(), this also gets called when the user
	 * manually switches to another song.  It may be used for
	 * playlist fixups.
	 *
	 * The song being started is specified by the #current
	 * attribute.
	 */
	void SongStarted() noexcept;

	/**
	 * Updates the "queued song".  Calculates the next song
	 * according to the current one (if MPD isn't playing, it
	 * takes the first song), and queues this song.  Clears the
	 * old queued song if there was one.
	 *
	 * @param prev the song which was previously queued, as
	 * determined by playlist_get_queued_song()
	 */
	void UpdateQueuedSong(PlayerControl &pc, const DetachedSong *prev) noexcept;

	/**
	 * Queue a song, addressed by its order number.
	 */
	void QueueSongOrder(PlayerControl &pc, unsigned order) noexcept;

	/**
	 * Called when the player thread has started playing the
	 * "queued" song, i.e. it has switched from one song to the
	 * next automatically.
	 */
	void QueuedSongStarted(PlayerControl &pc) noexcept;

	/**
	 * The player has stopped for some reason.  Check the error,
	 * and decide whether to re-start playback.
	 */
	void ResumePlayback(PlayerControl &pc) noexcept;

public:
	void BeginBulk() noexcept;
	void CommitBulk(PlayerControl &pc) noexcept;

	void Clear(PlayerControl &pc) noexcept;

	/**
	 * A tag in the play queue has been modified by the player
	 * thread.  Apply the given song's tag to the current song if
	 * the song matches.
	 */
	void TagModified(DetachedSong &&song) noexcept;

	/**
	 * @param real_uri the song's "real uri" (see
	 * DetachedSong::GetRealURI(), DetachedSong::IsRealURI())
	 */
	void TagModified(const char *real_uri, const Tag &tag) noexcept;

#ifdef ENABLE_DATABASE
	/**
	 * The database has been modified.  Pull all updates.
	 */
	void DatabaseModified(const Database &db);
#endif

	/**
	 * Throws PlaylistError if the queue would be too large.
	 *
	 * @return the new song id
	 */
	unsigned AppendSong(PlayerControl &pc, DetachedSong &&song);

	/**
	 * Throws #std::runtime_error on error.
	 *
	 * @return the new song id
	 */
	unsigned AppendURI(PlayerControl &pc,
			   const SongLoader &loader,
			   const char *uri_utf8);

protected:
	void DeleteInternal(PlayerControl &pc,
			    unsigned song, const DetachedSong **queued_p) noexcept;

public:
	void DeletePosition(PlayerControl &pc, unsigned position);

	void DeleteOrder(PlayerControl &pc, unsigned order) {
		DeletePosition(pc, queue.OrderToPosition(order));
	}

	void DeleteId(PlayerControl &pc, unsigned id);

	/**
	 * Deletes a range of songs from the playlist.
	 *
	 * @param start the position of the first song to delete
	 * @param end the position after the last song to delete
	 */
	void DeleteRange(PlayerControl &pc, RangeArg range);

	/**
	 * Mark the given song as "stale", i.e. as not being available
	 * anymore.  This gets called when a song is removed from the
	 * database.  The method attempts to remove all instances of
	 * this song from the queue.
	 */
	void StaleSong(PlayerControl &pc, const char *uri) noexcept;

	void Shuffle(PlayerControl &pc, RangeArg range);

	void MoveRange(PlayerControl &pc, RangeArg range, int to);

	void MoveId(PlayerControl &pc, unsigned id, int to);

	void SwapPositions(PlayerControl &pc, unsigned song1, unsigned song2);

	void SwapIds(PlayerControl &pc, unsigned id1, unsigned id2);

	void SetPriorityRange(PlayerControl &pc,
			      RangeArg position_range,
			      uint8_t priority);

	void SetPriorityId(PlayerControl &pc,
			   unsigned song_id, uint8_t priority);

	/**
	 * Sets the start_time and end_time attributes on the song
	 * with the specified id.
	 *
	 * Throws on error.
	 */
	void SetSongIdRange(PlayerControl &pc, unsigned id,
			    SongTime start, SongTime end);

	/**
	 * Throws on error.
	 */
	void AddSongIdTag(unsigned id, TagType tag_type,
			  const char *value);

	/**
	 * Throws on error.
	 */
	void ClearSongIdTag(unsigned id, TagType tag_type);

	void Stop(PlayerControl &pc) noexcept;

	/**
	 * Throws on error.
	 */
	void PlayPosition(PlayerControl &pc, int position);

	/**
	 * Throws on error.
	 */
	void PlayOrder(PlayerControl &pc, unsigned order);

	/**
	 * Throws on error.
	 */
	void PlayId(PlayerControl &pc, int id);

	/**
	 * Throws on error.
	 */
	void PlayNext(PlayerControl &pc);

	/**
	 * Throws on error.
	 */
	void PlayPrevious(PlayerControl &pc);

	/**
	 * Throws on error.
	 */
	void SeekSongOrder(PlayerControl &pc,
			   unsigned song_order,
			   SongTime seek_time);

	/**
	 * Throws on error.
	 */
	void SeekSongPosition(PlayerControl &pc,
			      unsigned sonag_position,
			      SongTime seek_time);

	/**
	 * Throws on error.
	 */
	void SeekSongId(PlayerControl &pc,
			unsigned song_id, SongTime seek_time);

	/**
	 * Seek within the current song.  Fails if MPD is not currently
	 * playing.
	 *
	 * Throws on error.
	 *
	 * @param seek_time the time
	 * @param relative if true, then the specified time is relative to the
	 * current position
	 */
	void SeekCurrent(PlayerControl &pc,
			 SignedSongTime seek_time, bool relative);

	bool GetRepeat() const noexcept {
		return queue.repeat;
	}

	void SetRepeat(PlayerControl &pc, bool new_value) noexcept;

	bool GetRandom() const noexcept {
		return queue.random;
	}

	void SetRandom(PlayerControl &pc, bool new_value) noexcept;

	SingleMode GetSingle() const noexcept {
		return queue.single;
	}

	void SetSingle(PlayerControl &pc, SingleMode new_value) noexcept;

	bool GetConsume() const noexcept {
		return queue.consume;
	}

	void SetConsume(bool new_value) noexcept;

private:
	/**
	 * Prepare a manual song change: move the given song to the
	 * current playback order.  This is done to avoid skipping
	 * upcoming songs in the order list.  The newly selected song
	 * shall be inserted in the order list, and the rest shall be
	 * played after that as previously planned.
	 *
	 * @return the new order number of the given song
	 */
	unsigned MoveOrderToCurrent(unsigned old_order) noexcept;
};

#endif
