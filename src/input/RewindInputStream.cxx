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

#include "RewindInputStream.hxx"
#include "ProxyInputStream.hxx"

#include <cassert>

#include <string.h>

class RewindInputStream final : public ProxyInputStream {
	/**
	 * The read position within the buffer.  Undefined as long as
	 * ReadingFromBuffer() returns false.
	 */
	size_t head;

	/**
	 * The write/append position within the buffer.
	 */
	size_t tail = 0;

	/**
	 * The size of this buffer is the maximum number of bytes
	 * which can be rewinded cheaply without passing the "seek"
	 * call to CURL.
	 *
	 * The origin of this buffer is always the beginning of the
	 * stream (offset 0).
	 */
	char buffer[64 * 1024];

public:
	explicit RewindInputStream(InputStreamPtr _input)
		:ProxyInputStream(std::move(_input)) {}

	/* virtual methods from InputStream */

	void Update() noexcept override {
		if (!ReadingFromBuffer())
			ProxyInputStream::Update();
	}

	[[nodiscard]] bool IsEOF() const noexcept override {
		return !ReadingFromBuffer() && ProxyInputStream::IsEOF();
	}

	size_t Read(std::unique_lock<Mutex> &lock,
		    void *ptr, size_t size) override;
	void Seek(std::unique_lock<Mutex> &lock, offset_type offset) override;

private:
	/**
	 * Are we currently reading from the buffer, and does the
	 * buffer contain more data for the next read operation?
	 */
	[[nodiscard]] bool ReadingFromBuffer() const noexcept {
		return tail > 0 && offset < input->GetOffset();
	}
};

size_t
RewindInputStream::Read(std::unique_lock<Mutex> &lock,
			void *ptr, size_t read_size)
{
	if (ReadingFromBuffer()) {
		/* buffered read */

		assert(head == (size_t)offset);
		assert(tail == (size_t)input->GetOffset());

		if (read_size > tail - head)
			read_size = tail - head;

		memcpy(ptr, buffer + head, read_size);
		head += read_size;
		offset += read_size;

		return read_size;
	} else {
		/* pass method call to underlying stream */

		size_t nbytes = input->Read(lock, ptr, read_size);

		if (input->GetOffset() > offset_type(sizeof(buffer)))
			/* disable buffering */
			tail = 0;
		else if (tail == size_t(offset)) {
			/* append to buffer */

			memcpy(buffer + tail, ptr, nbytes);
			tail += nbytes;

			assert(tail == (size_t)input->GetOffset());
		}

		CopyAttributes();

		return nbytes;
	}
}

void
RewindInputStream::Seek(std::unique_lock<Mutex> &lock, offset_type new_offset)
{
	assert(IsReady());

	if (tail > 0 && new_offset <= offset_type(tail)) {
		/* buffered seek */

		assert(!ReadingFromBuffer() ||
		       head == size_t(offset));
		assert(tail == size_t(input->GetOffset()));

		head = size_t(new_offset);
		offset = new_offset;
	} else {
		/* disable the buffer, because input has left the
		   buffered range now */
		tail = 0;

		ProxyInputStream::Seek(lock, new_offset);
	}
}

InputStreamPtr
input_rewind_open(InputStreamPtr is)
{
	assert(is != nullptr);
	assert(!is->IsReady() || is->GetOffset() == 0);

	if (is->IsReady() && is->IsSeekable())
		/* seekable resources don't need this plugin */
		return is;

	return std::make_unique<RewindInputStream>(std::move(is));
}
