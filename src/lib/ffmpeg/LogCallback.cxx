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

/* necessary because libavutil/common.h uses UINT64_C */
#define __STDC_CONSTANT_MACROS

#include "LogCallback.hxx"
#include "Domain.hxx"
#include "LogV.hxx"
#include "util/Domain.hxx"
#include "util/StringFormat.hxx"

extern "C" {
#include <libavutil/log.h>
}

gcc_const
static LogLevel
FfmpegImportLogLevel(int level) noexcept
{
	if (level <= AV_LOG_FATAL)
		return LogLevel::ERROR;

	if (level <= AV_LOG_WARNING)
		return LogLevel::WARNING;

	if (level <= AV_LOG_INFO)
		return LogLevel::INFO;

	return LogLevel::DEBUG;
}

void
FfmpegLogCallback(void *ptr, int level, const char *fmt, std::va_list vl)
{
	const AVClass * cls = nullptr;

	if (ptr != nullptr)
		cls = *static_cast<const AVClass *const*>(ptr);

	if (cls != nullptr) {
		const auto domain =
			StringFormat<64>("%s/%s",
					 ffmpeg_domain.GetName(),
					 cls->item_name(ptr));
		const Domain d(domain);
		LogFormatV(FfmpegImportLogLevel(level), d, fmt, vl);
	}
}
