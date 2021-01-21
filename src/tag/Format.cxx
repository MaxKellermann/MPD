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

#include "Format.hxx"
#include "Tag.hxx"
#include "ParseName.hxx"
#include "time/Convert.hxx"
#include "util/format.h"
#include "util/TruncateString.hxx"

#include <algorithm>

#include <string.h>
#include <time.h>

struct FormatTagContext {
	const Tag &tag;

	char buffer[256];

	explicit FormatTagContext(const Tag &_tag):tag(_tag) {}
};

/**
 * Is this a character unsafe to use in a path name segment?
 */
static constexpr bool
IsUnsafeChar(char ch)
{
	return
		/* disallow characters illegal in file names on
		   Windows (Linux allows almost anything) */
		ch == '\\' || ch == '/' || ch == ':' || ch == '*' ||
		ch == '?' || ch == '<' || ch == '>' || ch == '|' ||
		/* allow space, but disallow all other whitespace */
		uint8_t(ch) < 0x20;
}

gcc_pure
static bool
HasUnsafeChar(const char *s) noexcept
{
	for (; *s; ++s)
		if (IsUnsafeChar(*s))
			return true;

	return false;
}

static const char *
SanitizeString(const char *s, char *buffer, size_t buffer_size) noexcept
{
	/* skip leading dots to avoid generating "../" sequences */
	while (*s == '.')
		++s;

	if (!HasUnsafeChar(s))
		return s;

	char *end = CopyTruncateString(buffer, s, buffer_size);
	std::replace_if(buffer, end, IsUnsafeChar, ' ');
	return buffer;
}

gcc_pure gcc_nonnull_all
static const char *
TagGetter(const void *object, const char *name) noexcept
{
	const auto &_ctx = *static_cast<const FormatTagContext *>(object);
	auto &ctx = const_cast<FormatTagContext &>(_ctx);

	if (strcmp(name, "iso8601") == 0) {
		struct tm tm;

		try {
			tm = GmTime(std::chrono::system_clock::now());
		} catch (...) {
			return "";
		}

		strftime(ctx.buffer, sizeof(ctx.buffer),
#ifdef _WIN32
			 /* kludge: use underscore instead of colon on
			    Windows because colons are not allowed in
			    file names, and this library is mostly
			    used to generate file names */
			 "%Y-%m-%dT%H_%M_%SZ",
#else
			 "%FT%TZ",
#endif
			 &tm);
		return ctx.buffer;
	}

	const Tag &tag = ctx.tag;

	TagType tag_type = tag_name_parse_i(name);
	if (tag_type == TAG_NUM_OF_ITEM_TYPES)
		/* unknown tag name */
		return nullptr;

	const char *value = tag.GetValue(tag_type);
	if (value == nullptr)
		/* known tag name, but not present in this object */
		value = "";

	// TODO: handle multiple tag values
	return SanitizeString(value, ctx.buffer, sizeof(ctx.buffer));
}

char *
FormatTag(const Tag &tag, const char *format) noexcept
{
	FormatTagContext ctx(tag);
	return format_object(format, &ctx, TagGetter);
}
