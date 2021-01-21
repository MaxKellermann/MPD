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

#include "Thread.hxx"
#include "system/Error.hxx"

#ifdef ANDROID
#include "java/Global.hxx"
#endif

void
Thread::Start()
{
	assert(!IsDefined());

#ifdef _WIN32
	handle = ::CreateThread(nullptr, 0, ThreadProc, this, 0, &id);
	if (handle == nullptr)
		throw MakeLastError("Failed to create thread");
#else
	int e = pthread_create(&handle, nullptr, ThreadProc, this);

	if (e != 0)
		throw MakeErrno(e, "Failed to create thread");
#endif
}

void
Thread::Join() noexcept
{
	assert(IsDefined());
	assert(!IsInside());

#ifdef _WIN32
	::WaitForSingleObject(handle, INFINITE);
	::CloseHandle(handle);
	handle = nullptr;
#else
	pthread_join(handle, nullptr);
	handle = pthread_t();
#endif
}

inline void
Thread::Run() noexcept
{
	f();

#ifdef ANDROID
	Java::DetachCurrentThread();
#endif
}

#ifdef _WIN32

DWORD WINAPI
Thread::ThreadProc(LPVOID ctx) noexcept
{
	Thread &thread = *(Thread *)ctx;

	thread.Run();
	return 0;
}

#else

void *
Thread::ThreadProc(void *ctx) noexcept
{
	Thread &thread = *static_cast<Thread *>(ctx);

#ifndef NDEBUG
	thread.inside_handle = pthread_self();
#endif

	thread.Run();

	return nullptr;
}

#endif
