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
  * iso archive handling (requires cdio, and iso9660)
  */

#include "Iso9660ArchivePlugin.hxx"
#include "../ArchivePlugin.hxx"
#include "../ArchiveFile.hxx"
#include "../ArchiveVisitor.hxx"
#include "input/InputStream.hxx"
#include "fs/Path.hxx"
#include "util/RuntimeError.hxx"
#include "util/StringCompare.hxx"
#include "util/WritableBuffer.hxx"

#include <cdio/iso9660.h>

#include <array>
#include <utility>

#include <stdlib.h>
#include <string.h>

struct Iso9660 {
	iso9660_t *const iso;

	explicit Iso9660(Path path)
		:iso(iso9660_open(path.c_str())) {
		if (iso == nullptr)
			throw FormatRuntimeError("Failed to open ISO9660 file %s",
						 path.c_str());
	}

	~Iso9660() noexcept {
		iso9660_close(iso);
	}

	Iso9660(const Iso9660 &) = delete;
	Iso9660 &operator=(const Iso9660 &) = delete;

	long SeekRead(void *ptr, lsn_t start, long int i_size) const {
		return iso9660_iso_seek_read(iso, ptr, start, i_size);
	}
};

class Iso9660ArchiveFile final : public ArchiveFile {
	std::shared_ptr<Iso9660> iso;

public:
	explicit Iso9660ArchiveFile(std::shared_ptr<Iso9660> &&_iso)
		:iso(std::move(_iso)) {}

	/**
	 * @param capacity the path buffer size
	 */
	void Visit(char *path, size_t length, size_t capacity,
		   ArchiveVisitor &visitor);

	void Visit(ArchiveVisitor &visitor) override;

	InputStreamPtr OpenStream(const char *path,
				  Mutex &mutex) override;
};

/* archive open && listing routine */

inline void
Iso9660ArchiveFile::Visit(char *path, size_t length, size_t capacity,
			  ArchiveVisitor &visitor)
{
	auto *entlist = iso9660_ifs_readdir(iso->iso, path);
	if (!entlist) {
		return;
	}
	/* Iterate over the list of nodes that iso9660_ifs_readdir gives  */
	CdioListNode_t *entnode;
	_CDIO_LIST_FOREACH (entnode, entlist) {
		auto *statbuf = static_cast<iso9660_stat_t *>(_cdio_list_node_data(entnode));
		const char *filename = statbuf->filename;
		if (StringIsEmpty(filename) ||
		    PathTraitsUTF8::IsSpecialFilename(filename))
			/* skip empty names (libcdio bug?) */
			/* skip special names like "." and ".." */
			continue;

		size_t filename_length = strlen(filename);
		if (length + filename_length + 1 >= capacity)
			/* file name is too long */
			continue;

		memcpy(path + length, filename, filename_length + 1);
		size_t new_length = length + filename_length;

		if (iso9660_stat_s::_STAT_DIR == statbuf->type ) {
			memcpy(path + new_length, "/", 2);
			Visit(path, new_length + 1, capacity, visitor);
		} else {
			//remove leading /
			visitor.VisitArchiveEntry(path + 1);
		}
	}

#if LIBCDIO_VERSION_NUM >= 20000
	iso9660_filelist_free(entlist);
#else
	_cdio_list_free (entlist, true);
#endif
}

static std::unique_ptr<ArchiveFile>
iso9660_archive_open(Path pathname)
{
	return std::make_unique<Iso9660ArchiveFile>(std::make_shared<Iso9660>(pathname));
}

void
Iso9660ArchiveFile::Visit(ArchiveVisitor &visitor)
{
	char path[4096] = "/";
	Visit(path, 1, sizeof(path), visitor);
}

/* single archive handling */

class Iso9660InputStream final : public InputStream {
	std::shared_ptr<Iso9660> iso;

	const lsn_t lsn;

	/**
	 * libiso9660 can only read whole sectors at a time, and this
	 * buffer is used to store one whole sector and allow Read()
	 * to handle partial sector reads.
	 */
	class BlockBuffer {
		size_t position = 0, fill = 0;

		std::array<uint8_t, ISO_BLOCKSIZE> data;

	public:
		ConstBuffer<uint8_t> Read() const noexcept {
			assert(fill <= data.size());
			assert(position <= fill);

			return {&data[position], &data[fill]};
		}

		void Consume(size_t nbytes) noexcept {
			assert(nbytes <= Read().size);

			position += nbytes;
		}

		WritableBuffer<uint8_t> Write() noexcept {
			assert(Read().empty());

			return {data.data(), data.size()};
		}

		void Append(size_t nbytes) noexcept {
			assert(Read().empty());
			assert(nbytes <= data.size());

			fill = nbytes;
			position = 0;
		}

		void Clear() noexcept {
			position = fill = 0;
		}
	};

	BlockBuffer buffer;

	/**
	 * Skip this number of bytes of the first sector after filling
	 * the buffer next time.  This is used for seeking into the
	 * middle of a sector.
	 */
	size_t skip = 0;

public:
	Iso9660InputStream(std::shared_ptr<Iso9660> _iso,
			   const char *_uri,
			   Mutex &_mutex,
			   lsn_t _lsn, offset_type _size)
		:InputStream(_uri, _mutex),
		 iso(std::move(_iso)),
		 lsn(_lsn)
	{
		size = _size;
		seekable = true;
		SetReady();
	}

	/* virtual methods from InputStream */
	[[nodiscard]] bool IsEOF() const noexcept override;
	size_t Read(std::unique_lock<Mutex> &lock,
		    void *ptr, size_t size) override;

	void Seek(std::unique_lock<Mutex> &, offset_type new_offset) override {
		if (new_offset > size)
			throw std::runtime_error("Invalid seek offset");

		skip = new_offset % ISO_BLOCKSIZE;
		offset = new_offset - skip;
		buffer.Clear();
	}
};

InputStreamPtr
Iso9660ArchiveFile::OpenStream(const char *pathname,
			       Mutex &mutex)
{
	auto statbuf = iso9660_ifs_stat_translate(iso->iso, pathname);
	if (statbuf == nullptr)
		throw FormatRuntimeError("not found in the ISO file: %s",
					 pathname);

	const lsn_t lsn = statbuf->lsn;
	const offset_type size = statbuf->size;
	free(statbuf);

	return std::make_unique<Iso9660InputStream>(iso, pathname, mutex,
						    lsn, size);
}

size_t
Iso9660InputStream::Read(std::unique_lock<Mutex> &,
			 void *ptr, size_t read_size)
{
	const offset_type remaining = size - offset;
	if (remaining == 0)
		return 0;

	if (offset_type(read_size) > remaining)
		read_size = remaining;

	auto r = buffer.Read();

	if (r.empty()) {
		/* the buffer is empty - read more data from the ISO file */

		assert(offset % ISO_BLOCKSIZE == 0);

		const ScopeUnlock unlock(mutex);

		const lsn_t read_lsn = lsn + offset / ISO_BLOCKSIZE;

		if (read_size >= ISO_BLOCKSIZE) {
			/* big read - read right into the caller's buffer */

			auto nbytes = iso->SeekRead(ptr, read_lsn,
						    read_size / ISO_BLOCKSIZE);
			if (nbytes <= 0)
				throw std::runtime_error("Failed to read ISO9660 file");

			offset += nbytes;
			return nbytes;
		}

		/* fill the buffer */

		auto w = buffer.Write();
		auto nbytes = iso->SeekRead(w.data, read_lsn,
					    w.size / ISO_BLOCKSIZE);
		if (nbytes <= 0)
			throw std::runtime_error("Failed to read ISO9660 file");

		buffer.Append(nbytes);

		r = buffer.Read();

		if (skip > 0) {
			if (skip >= r.size)
				throw std::runtime_error("Premature end of ISO9660 track");

			buffer.Consume(skip);
			skip = 0;

			r = buffer.Read();
		}
	}

	assert(!r.empty());
	assert(skip == 0);

	size_t nbytes = std::min(read_size, r.size);
	memcpy(ptr, r.data, nbytes);
	buffer.Consume(nbytes);
	offset += nbytes;
	return nbytes;
}

bool
Iso9660InputStream::IsEOF() const noexcept
{
	return offset == size;
}

/* exported structures */

static const char *const iso9660_archive_extensions[] = {
	"iso",
	nullptr
};

const ArchivePlugin iso9660_archive_plugin = {
	"iso",
	nullptr,
	nullptr,
	iso9660_archive_open,
	iso9660_archive_extensions,
};
