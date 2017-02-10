/*
 * Copyright 2003-2017 The Music Player Daemon Project
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
#include "Thread.hxx"
#include "thread/Name.hxx"

void
EventThread::Start()
{
	assert(!thread.IsDefined());

	const std::lock_guard<Mutex> protect(mutex);
	thread.Start(ThreadFunc, this);
}

void
EventThread::Stop()
{
	if (thread.IsDefined()) {
		event_loop.Break();
		thread.Join();
	}
}

void
EventThread::ThreadFunc()
{
	SetThreadName("io");

	/* lock+unlock to synchronize with io_thread_start(), to be
	   sure that io.thread is set */
	mutex.lock();
	mutex.unlock();

	event_loop.Run();
};

void
EventThread::ThreadFunc(void *arg)
{
	auto &et = *(EventThread *)arg;

	et.ThreadFunc();
};
