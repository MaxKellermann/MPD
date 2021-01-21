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

#include "ArgParser.hxx"
#include "RangeArg.hxx"
#include "Ack.hxx"
#include "Chrono.hxx"
#include "util/NumberParser.hxx"

#include <stdlib.h>

uint32_t
ParseCommandArgU32(const char *s)
{
	char *test;
	auto value = strtoul(s, &test, 10);
	if (test == s || *test != '\0')
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Integer expected: %s", s);

	return value;
}

int
ParseCommandArgInt(const char *s, int min_value, int max_value)
{
	char *test;
	auto value = strtol(s, &test, 10);
	if (test == s || *test != '\0')
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Integer expected: %s", s);

	if (value < min_value || value > max_value)
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Number too large: %s", s);

	return int(value);
}

int
ParseCommandArgInt(const char *s)
{
	return ParseCommandArgInt(s,
				  std::numeric_limits<int>::min(),
				  std::numeric_limits<int>::max());
}

RangeArg
ParseCommandArgRange(const char *s)
{
	char *test, *test2;
	auto value = strtol(s, &test, 10);
	if (test == s || (*test != '\0' && *test != ':'))
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Integer or range expected: %s", s);

	if (value == -1 && *test == 0)
		/* compatibility with older MPD versions: specifying
		   "-1" makes MPD display the whole list */
		return RangeArg::All();

	if (value < 0)
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Number is negative: %s", s);

	if (value > std::numeric_limits<int>::max())
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Number too large: %s", s);

	RangeArg range;
	range.start = unsigned(value);

	if (*test == ':') {
		value = strtol(++test, &test2, 10);
		if (*test2 != '\0')
			throw FormatProtocolError(ACK_ERROR_ARG,
						  "Integer or range expected: %s",
						  s);

		if (test == test2)
			value = std::numeric_limits<int>::max();

		if (value < 0)
			throw FormatProtocolError(ACK_ERROR_ARG,
						  "Number is negative: %s", s);


		if (value > std::numeric_limits<int>::max())
			throw FormatProtocolError(ACK_ERROR_ARG,
						  "Number too large: %s", s);

		range.end = unsigned(value);
	} else {
		range.end = unsigned(value) + 1;
	}

	return range;
}

unsigned
ParseCommandArgUnsigned(const char *s, unsigned max_value)
{
	char *endptr;
	auto value = strtoul(s, &endptr, 10);
	if (endptr == s || *endptr != 0)
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Integer expected: %s", s);

	if (value > max_value)
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Number too large: %s", s);

	return unsigned(value);
}

unsigned
ParseCommandArgUnsigned(const char *s)
{
	return ParseCommandArgUnsigned(s,
				       std::numeric_limits<unsigned>::max());
}

bool
ParseCommandArgBool(const char *s)
{
	char *endptr;
	auto value = strtol(s, &endptr, 10);
	if (endptr == s || *endptr != 0 || (value != 0 && value != 1))
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Boolean (0/1) expected: %s", s);

	return !!value;
}

float
ParseCommandArgFloat(const char *s)
{
	char *endptr;
	auto value = ParseFloat(s, &endptr);
	if (endptr == s || *endptr != 0)
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Float expected: %s", s);

	return value;
}

SongTime
ParseCommandArgSongTime(const char *s)
{
	auto value = ParseCommandArgFloat(s);
	if (value < 0)
		throw FormatProtocolError(ACK_ERROR_ARG,
					  "Negative value not allowed: %s", s);

	return SongTime::FromS(value);
}

SignedSongTime
ParseCommandArgSignedSongTime(const char *s)
{
	auto value = ParseCommandArgFloat(s);
	return SignedSongTime::FromS(value);
}
