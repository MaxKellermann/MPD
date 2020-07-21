/*
 * Copyright 2003-2020 The Music Player Daemon Project
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

#include "YoutubeInputPlugin.hxx"
#include "CurlInputPlugin.hxx"
#include "PluginUnavailable.hxx"
#include "../InputPlugin.hxx"
#include "../InputStream.hxx"
#include "util/ASCII.hxx"
#include "util/Alloc.hxx"
#include "util/ScopeExit.hxx"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *input_youtube_prefixes[] = {
	"https",
	nullptr
};

static void
input_youtube_init(EventLoop &, const ConfigBlock &)
{
	if(WEXITSTATUS(system("youtube-dl --version > /dev/null")) != 0)
		throw PluginUnavailable("youtube-dl not found");
}

static InputStreamPtr
input_youtube_open(const char *uri, Mutex &mutex)
{
	/* lazy way to prevent command injection */
	for(size_t i = 0; i < strlen(uri); i++) {
		if(uri[i] == '\'') {
			return nullptr;
		}
	}

	char *cmd = xstrcatdup("youtube-dl --no-playlist --extract-audio --get-url --youtube-skip-dash-manifest '", uri, "'");
	FILE *stream = popen(cmd, "r");
	free(cmd);

	if(!stream) return nullptr;

	char *video_url = nullptr;
	size_t url_length = 0;
	AtScopeExit(video_url) { free(video_url); };

	ssize_t read = getline(&video_url, &url_length, stream);
	int   status = WEXITSTATUS(pclose(stream));

	if(status != 0 || read < 0) return nullptr;

	/* Remove newline from the url */
	if(video_url[url_length-2] == '\n')
		video_url[url_length-2] = '\0';

	return OpenCurlInputStream(video_url, {}, mutex);
}

const InputPlugin input_plugin_youtube = {
	"youtube",
	input_youtube_prefixes,
	input_youtube_init,
	nullptr,
	input_youtube_open,
	nullptr
};

// vim: set noexpandtab
