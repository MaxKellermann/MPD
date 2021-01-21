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

#include "Timer.hxx"
#include "pcm/AudioFormat.hxx"

#include <cassert>

Timer::Timer(const AudioFormat af)
	:rate(af.sample_rate * af.GetFrameSize())
{
}

void Timer::Start()
{
	time = Now();
	started = true;
}

void Timer::Reset()
{
	started = false;
}

void
Timer::Add(size_t size)
{
	assert(started);

	// (size samples) / (rate samples per second) = duration seconds
	// duration seconds * 1000000 = duration us
	time += Time((static_cast<uint64_t>(size) * Time::period::den) / (Time::period::num * rate));
}

std::chrono::steady_clock::duration
Timer::GetDelay() const
{
	assert(started);

	const auto delay = time - Now();
	if (delay < Time::zero())
		return Time::zero();

	return std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
}
