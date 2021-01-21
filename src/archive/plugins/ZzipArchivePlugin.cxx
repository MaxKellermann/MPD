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

/**
  * zip archive handling (requires zziplib)
  */

#include "ZzipArchivePlugin.hxx"
#include "../ArchivePlugin.hxx"
#include "../ArchiveFile.hxx"
#include "../ArchiveVisitor.hxx"
#include "input/InputStream.hxx"
#include "fs/Path.hxx"
#include "system/Error.hxx"
#include "util/RuntimeError.hxx"

#include <zzip/zzip.h>

#include <utility>

#include <inttypes.h> /* for PRIoffset (PRIu64) */

struct ZzipDir {
	ZZIP_DIR *const dir;

	explicit ZzipDir(Path path)
		:dir(zzip_dir_open(path.c_str(), nullptr)) {
		if (dir == nullptr)
			throw FormatRuntimeError("Failed to open ZIP file %s",
						 path.c_str());
	}

	~ZzipDir() noexcept {
		zzip_dir_close(dir);
	}

	ZzipDir(const ZzipDir &) = delete;
	ZzipDir &operator=(const ZzipDir &) = delete;
};

class ZzipArchiveFile final : public ArchiveFile {
	std::shared_ptr<ZzipDir> dir;

public:
	template<typename D>
	explicit ZzipArchiveFile(D &&_dir) noexcept
		:dir(std::forward<D>(_dir)) {}

	void Visit(ArchiveVisitor &visitor) override;

	InputStreamPtr OpenStream(const char *path,
				  Mutex &mutex) override;
};

/* archive open && listing routine */

static std::unique_ptr<ArchiveFile>
zzip_archive_open(Path pathname)
{
	return std::make_unique<ZzipArchiveFile>(std::make_shared<ZzipDir>(pathname));
}

inline void
ZzipArchiveFile::Visit(ArchiveVisitor &visitor)
{
	zzip_rewinddir(dir->dir);

	ZZIP_DIRENT dirent;
	while (zzip_dir_read(dir->dir, &dirent))
		//add only files
		if (dirent.st_size > 0)
			visitor.VisitArchiveEntry(dirent.d_name);
}

/* single archive handling */

class ZzipInputStream final : public InputStream {
	std::shared_ptr<ZzipDir> dir;

	ZZIP_FILE *const file;

public:
	template<typename D>
	ZzipInputStream(D &&_dir, const char *_uri,
			Mutex &_mutex,
			ZZIP_FILE *_file)
		:InputStream(_uri, _mutex),
		 dir(std::forward<D>(_dir)), file(_file) {
		//we are seekable (but its not recommendent to do so)
		seekable = true;

		ZZIP_STAT z_stat;
		zzip_file_stat(file, &z_stat);
		size = z_stat.st_size;

		SetReady();
	}

	~ZzipInputStream() noexcept override {
		zzip_file_close(file);
	}

	/* virtual methods from InputStream */
	[[nodiscard]] bool IsEOF() const noexcept override;
	size_t Read(std::unique_lock<Mutex> &lock,
		    void *ptr, size_t size) override;
	void Seek(std::unique_lock<Mutex> &lock, offset_type offset) override;
};

InputStreamPtr
ZzipArchiveFile::OpenStream(const char *pathname,
			    Mutex &mutex)
{
	ZZIP_FILE *_file = zzip_file_open(dir->dir, pathname, 0);
	if (_file == nullptr) {
		const auto error = zzip_error_t(zzip_error(dir->dir));
		switch (error) {
		case ZZIP_ENOENT:
			throw FormatFileNotFound("Failed to open '%s' in ZIP file",
						 pathname);

		default:
			throw FormatRuntimeError("Failed to open '%s' in ZIP file: %s",
						 pathname,
						 zzip_strerror(error));
		}
	}

	return std::make_unique<ZzipInputStream>(dir, pathname,
						 mutex,
						 _file);
}

size_t
ZzipInputStream::Read(std::unique_lock<Mutex> &, void *ptr, size_t read_size)
{
	const ScopeUnlock unlock(mutex);

	zzip_ssize_t nbytes = zzip_file_read(file, ptr, read_size);
	if (nbytes < 0)
		throw std::runtime_error("zzip_file_read() has failed");

	if (nbytes == 0 && !IsEOF())
		throw FormatRuntimeError("Unexpected end of file %s"
					 " at %" PRIoffset " of %" PRIoffset,
					 GetURI(), GetOffset(), GetSize());

	offset = zzip_tell(file);
	return nbytes;
}

bool
ZzipInputStream::IsEOF() const noexcept
{
	return offset_type(zzip_tell(file)) == size;
}

void
ZzipInputStream::Seek(std::unique_lock<Mutex> &, offset_type new_offset)
{
	const ScopeUnlock unlock(mutex);

	zzip_off_t ofs = zzip_seek(file, new_offset, SEEK_SET);
	if (ofs < 0)
		throw std::runtime_error("zzip_seek() has failed");

	offset = ofs;
}

/* exported structures */

static const char *const zzip_archive_extensions[] = {
	"zip",
	nullptr
};

const ArchivePlugin zzip_archive_plugin = {
	"zzip",
	nullptr,
	nullptr,
	zzip_archive_open,
	zzip_archive_extensions,
};
