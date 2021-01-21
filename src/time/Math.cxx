/*
 * Copyright 2007-2020 CM4all GmbH
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Math.hxx"
#include "Calendar.hxx"
#include "Convert.hxx"

#include <time.h>

std::chrono::system_clock::time_point
PrecedingMidnightLocal(std::chrono::system_clock::time_point t) noexcept
try {
	auto tm = LocalTime(t);
	tm.tm_sec = 0;
	tm.tm_min = 0;
	tm.tm_hour = 0;
	return MakeTime(tm);
} catch (...) {
	/* best-effort fallback for this exotic error condition */
	return t;
 }

static void
IncrementMonth(struct tm &tm) noexcept
{
	++tm.tm_mon;

	if (tm.tm_mon >= 12) {
		/* roll over to next year */
		tm.tm_mon = 0;
		++tm.tm_year;
	}
}

void
EndOfMonth(struct tm &tm) noexcept
{
	tm.tm_sec = 0;
	tm.tm_min = 0;
	tm.tm_hour = 0;
	tm.tm_mday = 1;
	IncrementMonth(tm);
}

void
IncrementDay(struct tm &tm) noexcept
{
	const unsigned max_day = DaysInMonth(tm.tm_mon + 1, tm.tm_year + 1900);

	++tm.tm_mday;

	if (unsigned(tm.tm_mday) > max_day) {
		/* roll over to next month */
		tm.tm_mday = 1;
		IncrementMonth(tm);
	}

	++tm.tm_wday;
	if (tm.tm_wday >= 7)
		tm.tm_wday = 0;
}

void
DecrementDay(struct tm &tm) noexcept
{
	--tm.tm_mday;

	if (tm.tm_mday < 1) {
		/* roll over to previous month */

		--tm.tm_mon;
		if (tm.tm_mon < 0) {
			/* roll over to previous eyar */
			tm.tm_mon = 11;
			--tm.tm_year;
		}

		const unsigned max_day = DaysInMonth(tm.tm_mon + 1,
						     tm.tm_year + 1900);
		tm.tm_mday = max_day;
	}

	--tm.tm_wday;
	if (tm.tm_wday < 0)
		tm.tm_wday = 6;
}
