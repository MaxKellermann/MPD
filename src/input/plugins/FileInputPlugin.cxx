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

#include "FileInputPlugin.hxx"
#include "../InputStream.hxx"
#include "fs/Path.hxx"
#include "fs/FileInfo.hxx"
#include "fs/io/FileReader.hxx"
#include "io/FileDescriptor.hxx"
#include "util/RuntimeError.hxx"

#include <cinttypes> // for PRIu64 (PRIoffset)

#include <sys/stat.h>
#include <fcntl.h>

class FileInputStream final : public InputStream {
	FileReader reader;

public:
	FileInputStream(const char *path, FileReader &&_reader, off_t _size,
			Mutex &_mutex)
		:InputStream(path, _mutex),
		 reader(std::move(_reader)) {
		size = _size;
		seekable = true;
		SetReady();
	}

	/* virtual methods from InputStream */

	[[nodiscard]] bool IsEOF() const noexcept override {
		return GetOffset() >= GetSize();
	}

	size_t Read(std::unique_lock<Mutex> &lock,
		    void *ptr, size_t size) override;
	void Seek(std::unique_lock<Mutex> &lock,
		  offset_type offset) override;
};

InputStreamPtr
OpenFileInputStream(Path path, Mutex &mutex)
{
	FileReader reader(path);

	const FileInfo info = reader.GetFileInfo();

	if (!info.IsRegular())
		throw FormatRuntimeError("Not a regular file: %s",
					 path.c_str());

#ifdef POSIX_FADV_SEQUENTIAL
	posix_fadvise(reader.GetFD().Get(), off_t(0), info.GetSize(),
		      POSIX_FADV_SEQUENTIAL);
#endif

	return std::make_unique<FileInputStream>(path.ToUTF8Throw().c_str(),
						 std::move(reader), info.GetSize(),
						 mutex);
}

void
FileInputStream::Seek(std::unique_lock<Mutex> &,
		      offset_type new_offset)
{
	{
		const ScopeUnlock unlock(mutex);
		reader.Seek(off_t(new_offset));
	}

	offset = new_offset;
}

size_t
FileInputStream::Read(std::unique_lock<Mutex> &,
		      void *ptr, size_t read_size)
{
	size_t nbytes;

	{
		const ScopeUnlock unlock(mutex);
		nbytes = reader.Read(ptr, read_size);
	}

	if (nbytes == 0 && !IsEOF())
		throw FormatRuntimeError("Unexpected end of file %s"
					 " at %" PRIoffset " of %" PRIoffset,
					 GetURI(), GetOffset(), GetSize());

	offset += nbytes;
	return nbytes;
}
