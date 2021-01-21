/*
 * Copyright (C) 2014-2018 Max Kellermann <max.kellermann@gmail.com>
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

#include "FileOutputStream.hxx"
#include "system/Error.hxx"
#include "util/StringFormat.hxx"

#ifdef __linux__
#include <fcntl.h>
#endif

#ifdef __linux__
FileOutputStream::FileOutputStream(FileDescriptor _directory_fd,
				   Path _path, Mode _mode)
	:path(_path),
	 directory_fd(_directory_fd),
	 mode(_mode)
{
	Open();
}
#endif

FileOutputStream::FileOutputStream(Path _path, Mode _mode)
	:path(_path),
#ifdef __linux__
	 directory_fd(AT_FDCWD),
#endif
	 mode(_mode)
{
	Open();
}

inline void
FileOutputStream::Open()
{
	switch (mode) {
	case Mode::CREATE:
		OpenCreate(false);
		break;

	case Mode::CREATE_VISIBLE:
		OpenCreate(true);
		break;

	case Mode::APPEND_EXISTING:
		OpenAppend(false);
		break;

	case Mode::APPEND_OR_CREATE:
		OpenAppend(true);
		break;
	}
}

#ifdef _WIN32

inline void
FileOutputStream::OpenCreate([[maybe_unused]] bool visible)
{
	handle = CreateFile(path.c_str(), GENERIC_WRITE, 0, nullptr,
			    CREATE_ALWAYS,
			    FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,
			    nullptr);
	if (!IsDefined())
		throw FormatLastError("Failed to create %s",
				      path.ToUTF8().c_str());
}

inline void
FileOutputStream::OpenAppend(bool create)
{
	handle = CreateFile(path.c_str(), GENERIC_WRITE, 0, nullptr,
			    create ? OPEN_ALWAYS : OPEN_EXISTING,
			    FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,
			    nullptr);
	if (!IsDefined())
		throw FormatLastError("Failed to append to %s",
				      path.ToUTF8().c_str());

	if (!SeekEOF()) {
		auto code = GetLastError();
		Close();
		throw FormatLastError(code, "Failed seek end-of-file of %s",
				      path.ToUTF8().c_str());
	}

}

uint64_t
FileOutputStream::Tell() const noexcept
{
	LONG high = 0;
	DWORD low = SetFilePointer(handle, 0, &high, FILE_CURRENT);
	if (low == 0xffffffff)
		return 0;

	return uint64_t(high) << 32 | uint64_t(low);
}

void
FileOutputStream::Write(const void *data, size_t size)
{
	assert(IsDefined());

	DWORD nbytes;
	if (!WriteFile(handle, data, size, &nbytes, nullptr))
		throw FormatLastError("Failed to write to %s",
				      GetPath().c_str());

	if (size_t(nbytes) != size)
		throw FormatLastError(ERROR_DISK_FULL, "Failed to write to %s",
				      GetPath().c_str());
}

void
FileOutputStream::Commit()
{
	assert(IsDefined());

	Close();
}

void
FileOutputStream::Cancel() noexcept
{
	assert(IsDefined());

	Close();

	DeleteFile(GetPath().c_str());
}

#else

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

#ifdef HAVE_O_TMPFILE
#ifndef O_TMPFILE
/* supported since Linux 3.11 */
#define __O_TMPFILE 020000000
#define O_TMPFILE (__O_TMPFILE | O_DIRECTORY)
#include <stdio.h>
#endif

/**
 * Open a file using Linux's O_TMPFILE for writing the given file.
 */
static bool
OpenTempFile(FileDescriptor directory_fd,
	     FileDescriptor &fd, Path path)
{
	if (directory_fd != FileDescriptor(AT_FDCWD))
		return fd.Open(directory_fd, ".", O_TMPFILE|O_WRONLY, 0666);

	const auto directory = path.GetDirectoryName();
	if (directory.IsNull())
		return false;

	return fd.Open(directory.c_str(), O_TMPFILE|O_WRONLY, 0666);
}

#endif /* HAVE_O_TMPFILE */

inline void
FileOutputStream::OpenCreate(bool visible)
{
#ifdef HAVE_O_TMPFILE
	/* try Linux's O_TMPFILE first */
	is_tmpfile = !visible && OpenTempFile(directory_fd, fd, GetPath());
	if (!is_tmpfile) {
#endif
		/* fall back to plain POSIX */
		if (!fd.Open(
#ifdef __linux__
			     directory_fd,
#endif
			     GetPath().c_str(),
			     O_WRONLY|O_CREAT|O_TRUNC,
			     0666))
			throw FormatErrno("Failed to create %s",
					  GetPath().c_str());
#ifdef HAVE_O_TMPFILE
	}
#else
	(void)visible;
#endif
}

inline void
FileOutputStream::OpenAppend(bool create)
{
	int flags = O_WRONLY|O_APPEND;
	if (create)
		flags |= O_CREAT;

	if (!fd.Open(
#ifdef __linux__
		     directory_fd,
#endif
		     path.c_str(), flags))
		throw FormatErrno("Failed to append to %s",
				  path.c_str());
}

uint64_t
FileOutputStream::Tell() const noexcept
{
	return fd.Tell();
}

void
FileOutputStream::Write(const void *data, size_t size)
{
	assert(IsDefined());

	ssize_t nbytes = fd.Write(data, size);
	if (nbytes < 0)
		throw FormatErrno("Failed to write to %s", GetPath().c_str());
	else if (size_t(nbytes) < size)
		throw FormatErrno(ENOSPC, "Failed to write to %s",
				  GetPath().c_str());
}

void
FileOutputStream::Commit()
{
	assert(IsDefined());

#ifdef HAVE_O_TMPFILE
	if (is_tmpfile) {
		unlinkat(directory_fd.Get(), GetPath().c_str(), 0);

		/* hard-link the temporary file to the final path */
		if (linkat(AT_FDCWD,
			   StringFormat<64>("/proc/self/fd/%d", fd.Get()),
			   directory_fd.Get(), path.c_str(),
			   AT_SYMLINK_FOLLOW) < 0)
			throw FormatErrno("Failed to commit %s",
					  path.c_str());
	}
#endif

	if (!Close()) {
		throw FormatErrno("Failed to commit %s", path.c_str());
	}
}

void
FileOutputStream::Cancel() noexcept
{
	assert(IsDefined());

	Close();

	switch (mode) {
	case Mode::CREATE:
#ifdef HAVE_O_TMPFILE
		if (!is_tmpfile)
#endif
#ifdef __linux__
			unlinkat(directory_fd.Get(), GetPath().c_str(), 0);
#else
		unlink(GetPath().c_str());
#endif
		break;

	case Mode::CREATE_VISIBLE:
	case Mode::APPEND_EXISTING:
	case Mode::APPEND_OR_CREATE:
		/* can't roll this back */
		break;
	}
}

#endif
