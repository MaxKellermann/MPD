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

#include "Parser.hxx"
#include "util/RuntimeError.hxx"
#include "util/StringStrip.hxx"
#include "util/StringUtil.hxx"

#include <cstdlib>

bool
ParseBool(const char *value)
{
	static const char *const t[] = { "yes", "true", "1", nullptr };
	static const char *const f[] = { "no", "false", "0", nullptr };

	if (StringArrayContainsCase(t, value))
		return true;

	if (StringArrayContainsCase(f, value))
		return false;

	throw FormatRuntimeError(R"(Not a valid boolean ("yes" or "no"): "%s")", value);
}

long
ParseLong(const char *s)
{
	char *endptr;
	long value = strtol(s, &endptr, 10);
	if (endptr == s || *endptr != 0)
		throw std::runtime_error("Failed to parse number");

	return value;
}

unsigned
ParseUnsigned(const char *s)
{
	auto value = ParseLong(s);
	if (value < 0)
		throw std::runtime_error("Value must not be negative");

	return unsigned(value);
}

unsigned
ParsePositive(const char *s)
{
	auto value = ParseLong(s);
	if (value <= 0)
		throw std::runtime_error("Value must be positive");

	return unsigned(value);
}

template<std::size_t OPERAND>
static std::size_t
Multiply(std::size_t value)
{
	static constexpr std::size_t MAX_VALUE = SIZE_MAX / OPERAND;
	if (value > MAX_VALUE)
		throw std::runtime_error("Value too large");

	return value * OPERAND;
}

std::size_t
ParseSize(const char *s, std::size_t default_factor)
{
	char *endptr;
	std::size_t value = strtoul(s, &endptr, 10);
	if (endptr == s)
		throw std::runtime_error("Failed to parse integer");

	static constexpr std::size_t KILO = 1024;
	static constexpr std::size_t MEGA = 1024 * KILO;
	static constexpr std::size_t GIGA = 1024 * MEGA;

	s = StripLeft(endptr);

	bool apply_factor = false;

	switch (*s) {
	case 'k':
		value = Multiply<KILO>(value);
		++s;
		break;

	case 'M':
		value = Multiply<MEGA>(value);
		++s;
		break;

	case 'G':
		value = Multiply<GIGA>(value);
		++s;
		break;

	case '\0':
		apply_factor = true;
		break;

	default:
		throw std::runtime_error("Unknown size suffix");
	}

	/* ignore 'B' for "byte" */
	if (*s == 'B') {
		apply_factor = false;
		++s;
	}

	if (*s != '\0')
		throw std::runtime_error("Unknown size suffix");

	if (apply_factor)
		value *= default_factor;

	return value;
}

std::chrono::steady_clock::duration
ParseDuration(const char *s)
{
	// TODO: allow unit suffixes
	const std::chrono::seconds seconds(ParseLong(s));
	return std::chrono::duration_cast<std::chrono::steady_clock::duration>(seconds);
}
