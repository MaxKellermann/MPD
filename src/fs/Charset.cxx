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
#include "Charset.hxx"
#include "Domain.hxx"
#include "Limits.hxx"
#include "Log.hxx"
#include "Traits.hxx"
#include "lib/icu/Converter.hxx"
#include "util/Error.hxx"

#include <algorithm>

#include <assert.h>
#include <string.h>

#ifdef HAVE_FS_CHARSET

static std::string fs_charset;

static IcuConverter *fs_converter;

bool
SetFSCharset(const char *charset, Error &error)
{
	assert(charset != nullptr);
	assert(fs_converter == nullptr);

	fs_converter = IcuConverter::Create(charset, error);
	if (fs_converter == nullptr)
		return false;

	FormatDebug(path_domain,
		    "SetFSCharset: fs charset is: %s", fs_charset.c_str());
	return true;
}

#endif

void
DeinitFSCharset()
{
#ifdef HAVE_ICU_CONVERTER
	delete fs_converter;
	fs_converter = nullptr;
#endif
}

const char *
GetFSCharset()
{
#ifdef HAVE_FS_CHARSET
	return fs_charset.empty() ? "UTF-8" : fs_charset.c_str();
#else
	return "UTF-8";
#endif
}

static inline void FixSeparators(std::string &s)
{
#ifdef WIN32
	// For whatever reason GCC can't convert constexpr to value reference.
	// This leads to link errors when passing separators directly.
	auto from = PathTraitsFS::SEPARATOR;
	auto to = PathTraitsUTF8::SEPARATOR;
	std::replace(s.begin(), s.end(), from, to);
#else
	(void)s;
#endif
}

std::string
PathToUTF8(const char *path_fs)
{
	assert(path_fs != nullptr);

#ifdef HAVE_FS_CHARSET
	if (fs_converter == nullptr) {
#endif
		auto result = std::string(path_fs);
		FixSeparators(result);
		return result;
#ifdef HAVE_FS_CHARSET
	}

	auto result_path = fs_converter->ToUTF8(path_fs);
	FixSeparators(result_path);
	return result_path;
#endif
}

#ifdef HAVE_FS_CHARSET

std::string
PathFromUTF8(const char *path_utf8)
{
	assert(path_utf8 != nullptr);

	if (fs_converter == nullptr)
		return path_utf8;

	return fs_converter->FromUTF8(path_utf8);
}

#endif
