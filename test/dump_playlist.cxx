/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
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
#include "TagSave.hxx"
#include "DetachedSong.hxx"
#include "playlist/SongEnumerator.hxx"
#include "input/InputStream.hxx"
#include "config/ConfigGlobal.hxx"
#include "decoder/DecoderList.hxx"
#include "input/Init.hxx"
#include "IOThread.hxx"
#include "playlist/PlaylistRegistry.hxx"
#include "playlist/PlaylistPlugin.hxx"
#include "fs/Path.hxx"
#include "util/Error.hxx"
#include "thread/Cond.hxx"
#include "Log.hxx"

#ifdef HAVE_GLIB
#include <glib.h>
#endif

#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	const char *uri;
	InputStream *is = NULL;

	if (argc != 3) {
		fprintf(stderr, "Usage: dump_playlist CONFIG URI\n");
		return EXIT_FAILURE;
	}

	const Path config_path = Path::FromFS(argv[1]);
	uri = argv[2];

	/* initialize GLib */

#ifdef HAVE_GLIB
#if !GLIB_CHECK_VERSION(2,32,0)
	g_thread_init(NULL);
#endif
#endif

	/* initialize MPD */

	config_global_init();

	Error error;
	if (!ReadConfigFile(config_path, error)) {
		LogError(error);
		return EXIT_FAILURE;
	}

	io_thread_init();
	io_thread_start();

	if (!input_stream_global_init(error)) {
		LogError(error);
		return EXIT_FAILURE;
	}

	playlist_list_global_init();
	decoder_plugin_init_all();

	/* open the playlist */

	Mutex mutex;
	Cond cond;

	auto playlist = playlist_list_open_uri(uri, mutex, cond);
	if (playlist == NULL) {
		/* open the stream and wait until it becomes ready */

		is = InputStream::OpenReady(uri, mutex, cond, error);
		if (is == NULL) {
			if (error.IsDefined())
				LogError(error);
			else
				fprintf(stderr,
					"input/InputStream::Open() failed\n");
			return 2;
		}

		/* open the playlist */

		playlist = playlist_list_open_stream(*is, uri);
		if (playlist == NULL) {
			is->Close();
			fprintf(stderr, "Failed to open playlist\n");
			return 2;
		}
	}

	/* dump the playlist */

	DetachedSong *song;
	while ((song = playlist->NextSong()) != NULL) {
		printf("%s\n", song->GetURI());

		const unsigned start_ms = song->GetStartMS();
		const unsigned end_ms = song->GetEndMS();

		if (end_ms > 0)
			printf("range: %u:%02u..%u:%02u\n",
			       start_ms / 60000,
			       (start_ms / 1000) % 60,
			       end_ms / 60000,
			       (end_ms / 1000) % 60);
		else if (start_ms > 0)
			printf("range: %u:%02u..\n",
			       start_ms / 60000,
			       (start_ms / 1000) % 60);

		tag_save(stdout, song->GetTag());

		delete song;
	}

	/* deinitialize everything */

	delete playlist;
	if (is != NULL)
		is->Close();

	decoder_plugin_deinit_all();
	playlist_list_global_finish();
	input_stream_global_finish();
	io_thread_deinit();
	config_global_finish();

	return 0;
}
