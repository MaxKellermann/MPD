/*
 * Copyright 2012-2020 Max Kellermann <max.kellermann@gmail.com>
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

#include "FileDescriptor.hxx"
#include "system/Error.hxx"

#include <cassert>
#include <cstdint>
#include <stdexcept>

#include <sys/stat.h>
#include <fcntl.h>

#ifndef _WIN32
#include <poll.h>
#endif

#ifdef __linux__
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#endif

#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef _WIN32

bool
FileDescriptor::IsValid() const noexcept
{
	return IsDefined() && fcntl(fd, F_GETFL) >= 0;
}

bool
FileDescriptor::IsRegularFile() const noexcept
{
	struct stat st;
	return IsDefined() && fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}

bool
FileDescriptor::IsPipe() const noexcept
{
	struct stat st;
	return IsDefined() && fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode);
}

bool
FileDescriptor::IsSocket() const noexcept
{
	struct stat st;
	return IsDefined() && fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode);
}

#endif

#ifdef __linux__

bool
FileDescriptor::Open(FileDescriptor dir, const char *pathname,
		     int flags, mode_t mode) noexcept
{
	fd = ::openat(dir.Get(), pathname, flags | O_NOCTTY | O_CLOEXEC, mode);
	return IsDefined();
}

#endif

bool
FileDescriptor::Open(const char *pathname, int flags, mode_t mode) noexcept
{
	fd = ::open(pathname, flags | O_NOCTTY | O_CLOEXEC, mode);
	return IsDefined();
}

#ifdef _WIN32

bool
FileDescriptor::Open(const wchar_t *pathname, int flags, mode_t mode) noexcept
{
	fd = ::_wopen(pathname, flags | O_NOCTTY | O_CLOEXEC, mode);
	return IsDefined();
}

#endif

bool
FileDescriptor::OpenReadOnly(const char *pathname) noexcept
{
	return Open(pathname, O_RDONLY);
}

#ifndef _WIN32

bool
FileDescriptor::OpenNonBlocking(const char *pathname) noexcept
{
	return Open(pathname, O_RDWR | O_NONBLOCK);
}

#endif

#ifdef __linux__

bool
FileDescriptor::CreatePipe(FileDescriptor &r, FileDescriptor &w,
			   int flags) noexcept
{
	int fds[2];
	const int result = pipe2(fds, flags);
	if (result < 0)
		return false;

	r = FileDescriptor(fds[0]);
	w = FileDescriptor(fds[1]);
	return true;
}

#endif

bool
FileDescriptor::CreatePipe(FileDescriptor &r, FileDescriptor &w) noexcept
{
#ifdef __linux__
	return CreatePipe(r, w, O_CLOEXEC);
#else
	int fds[2];

#ifdef _WIN32
	const int result = _pipe(fds, 512, _O_BINARY);
#else
	const int result = pipe(fds);
#endif

	if (result < 0)
		return false;

	r = FileDescriptor(fds[0]);
	w = FileDescriptor(fds[1]);
	return true;
#endif
}

#ifndef _WIN32

bool
FileDescriptor::CreatePipeNonBlock(FileDescriptor &r,
				   FileDescriptor &w) noexcept
{
#ifdef __linux__
	return CreatePipe(r, w, O_CLOEXEC|O_NONBLOCK);
#else
	if (!CreatePipe(r, w))
		return false;

	r.SetNonBlocking();
	w.SetNonBlocking();
	return true;
#endif
}

void
FileDescriptor::SetNonBlocking() noexcept
{
	assert(IsDefined());

	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void
FileDescriptor::SetBlocking() noexcept
{
	assert(IsDefined());

	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

void
FileDescriptor::EnableCloseOnExec() noexcept
{
	assert(IsDefined());

	const int old_flags = fcntl(fd, F_GETFD, 0);
	fcntl(fd, F_SETFD, old_flags | FD_CLOEXEC);
}

void
FileDescriptor::DisableCloseOnExec() noexcept
{
	assert(IsDefined());

	const int old_flags = fcntl(fd, F_GETFD, 0);
	fcntl(fd, F_SETFD, old_flags & ~FD_CLOEXEC);
}

bool
FileDescriptor::CheckDuplicate(FileDescriptor new_fd) noexcept
{
	if (*this == new_fd) {
		DisableCloseOnExec();
		return true;
	} else
		return Duplicate(new_fd);
}

#endif

#ifdef __linux__

bool
FileDescriptor::CreateEventFD(unsigned initval) noexcept
{
	fd = ::eventfd(initval, EFD_NONBLOCK|EFD_CLOEXEC);
	return fd >= 0;
}

bool
FileDescriptor::CreateSignalFD(const sigset_t *mask) noexcept
{
	int new_fd = ::signalfd(fd, mask, SFD_NONBLOCK|SFD_CLOEXEC);
	if (new_fd < 0)
		return false;

	fd = new_fd;
	return true;
}

bool
FileDescriptor::CreateInotify() noexcept
{
	int new_fd = inotify_init1(IN_CLOEXEC|IN_NONBLOCK);
	if (new_fd < 0)
		return false;

	fd = new_fd;
	return true;
}

#endif

bool
FileDescriptor::Rewind() noexcept
{
	assert(IsDefined());

	return lseek(fd, 0, SEEK_SET) == 0;
}

off_t
FileDescriptor::GetSize() const noexcept
{
	struct stat st;
	return ::fstat(fd, &st) >= 0
		? long(st.st_size)
		: -1;
}

void
FileDescriptor::FullRead(void *_buffer, std::size_t length)
{
	auto buffer = static_cast<uint8_t *>(_buffer);

	while (length > 0) {
		ssize_t nbytes = Read(buffer, length);
		if (nbytes <= 0) {
			if (nbytes < 0)
				throw MakeErrno("Failed to read");
			throw std::runtime_error("Unexpected end of file");
		}

		buffer += nbytes;
		length -= nbytes;
	}
}

void
FileDescriptor::FullWrite(const void *_buffer, std::size_t length)
{
	auto buffer = static_cast<const uint8_t *>(_buffer);

	while (length > 0) {
		ssize_t nbytes = Write(buffer, length);
		if (nbytes <= 0) {
			if (nbytes < 0)
				throw MakeErrno("Failed to write");
			throw std::runtime_error("Failed to write");
		}

		buffer += nbytes;
		length -= nbytes;
	}
}

#ifndef _WIN32

int
FileDescriptor::Poll(short events, int timeout) const noexcept
{
	assert(IsDefined());

	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = events;
	int result = poll(&pfd, 1, timeout);
	return result > 0
		? pfd.revents
		: result;
}

int
FileDescriptor::WaitReadable(int timeout) const noexcept
{
	return Poll(POLLIN, timeout);
}

int
FileDescriptor::WaitWritable(int timeout) const noexcept
{
	return Poll(POLLOUT, timeout);
}

bool
FileDescriptor::IsReadyForWriting() const noexcept
{
	return WaitWritable(0) > 0;
}

#endif
