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

#include "InputStream.hxx"
#include "Handler.hxx"
#include "tag/Tag.hxx"
#include "util/ASCII.hxx"

#include <cassert>
#include <stdexcept>

InputStream::~InputStream() noexcept = default;

void
InputStream::Check()
{
}

void
InputStream::Update() noexcept
{
}

void
InputStream::SetReady() noexcept
{
	assert(!ready);

	ready = true;

	InvokeOnReady();
}

/**
 * Is seeking on resources behind this URI "expensive"?  For example,
 * seeking in a HTTP file requires opening a new connection with a new
 * HTTP request.
 */
gcc_pure
static bool
ExpensiveSeeking(const char *uri) noexcept
{
	return StringStartsWithCaseASCII(uri, "http://") ||
		StringStartsWithCaseASCII(uri, "tidal://") ||
		StringStartsWithCaseASCII(uri, "qobuz://") ||
		StringStartsWithCaseASCII(uri, "https://");
}

bool
InputStream::CheapSeeking() const noexcept
{
	return IsSeekable() && !ExpensiveSeeking(uri.c_str());
}

//[[noreturn]]
void
InputStream::Seek(std::unique_lock<Mutex> &, [[maybe_unused]] offset_type new_offset)
{
	throw std::runtime_error("Seeking is not implemented");
}

void
InputStream::LockSeek(offset_type _offset)
{
	std::unique_lock<Mutex> lock(mutex);
	Seek(lock, _offset);
}

void
InputStream::LockSkip(offset_type _offset)
{
	std::unique_lock<Mutex> lock(mutex);
	Skip(lock, _offset);
}

std::unique_ptr<Tag>
InputStream::ReadTag() noexcept
{
	return nullptr;
}

std::unique_ptr<Tag>
InputStream::LockReadTag() noexcept
{
	const std::lock_guard<Mutex> protect(mutex);
	return ReadTag();
}

bool
InputStream::IsAvailable() const noexcept
{
	return true;
}

size_t
InputStream::LockRead(void *ptr, size_t _size)
{
#if !CLANG_CHECK_VERSION(3,6)
	/* disabled on clang due to -Wtautological-pointer-compare */
	assert(ptr != nullptr);
#endif
	assert(_size > 0);

	std::unique_lock<Mutex> lock(mutex);
	return Read(lock, ptr, _size);
}

void
InputStream::ReadFull(std::unique_lock<Mutex> &lock, void *_ptr, size_t _size)
{
	auto *ptr = static_cast<uint8_t *>(_ptr);

	size_t nbytes_total = 0;
	while (_size > 0) {
		size_t nbytes = Read(lock, ptr + nbytes_total, _size);
		if (nbytes == 0)
			throw std::runtime_error("Unexpected end of file");

		nbytes_total += nbytes;
		_size -= nbytes;
	}
}

void
InputStream::LockReadFull(void *ptr, size_t _size)
{
#if !CLANG_CHECK_VERSION(3,6)
	/* disabled on clang due to -Wtautological-pointer-compare */
	assert(ptr != nullptr);
#endif
	assert(_size > 0);

	std::unique_lock<Mutex> lock(mutex);
	ReadFull(lock, ptr, _size);
}

bool
InputStream::LockIsEOF() const noexcept
{
	const std::lock_guard<Mutex> protect(mutex);
	return IsEOF();
}

void
InputStream::InvokeOnReady() noexcept
{
	if (handler != nullptr)
		handler->OnInputStreamReady();
}

void
InputStream::InvokeOnAvailable() noexcept
{
	if (handler != nullptr)
		handler->OnInputStreamAvailable();
}
