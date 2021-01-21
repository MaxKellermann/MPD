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

#include "AsyncInputStream.hxx"
#include "CondHandler.hxx"
#include "tag/Tag.hxx"
#include "thread/Cond.hxx"
#include "event/Loop.hxx"

#include <cassert>
#include <stdexcept>

#include <string.h>

AsyncInputStream::AsyncInputStream(EventLoop &event_loop, const char *_url,
				   Mutex &_mutex,
				   size_t _buffer_size,
				   size_t _resume_at) noexcept
	:InputStream(_url, _mutex),
	 deferred_resume(event_loop, BIND_THIS_METHOD(DeferredResume)),
	 deferred_seek(event_loop, BIND_THIS_METHOD(DeferredSeek)),
	 allocation(_buffer_size),
	 buffer(&allocation.front(), allocation.size()),
	 resume_at(_resume_at)
{
	allocation.ForkCow(false);
}

AsyncInputStream::~AsyncInputStream() noexcept
{
	buffer.Clear();
}

void
AsyncInputStream::SetTag(std::unique_ptr<Tag> _tag) noexcept
{
	tag = std::move(_tag);
}

void
AsyncInputStream::ClearTag() noexcept
{
	tag.reset();
}

void
AsyncInputStream::Pause() noexcept
{
	assert(GetEventLoop().IsInside());

	paused = true;
}

inline void
AsyncInputStream::Resume()
{
	assert(GetEventLoop().IsInside());

	if (paused) {
		paused = false;

		DoResume();
	}
}

void
AsyncInputStream::Check()
{
	if (postponed_exception)
		std::rethrow_exception(std::exchange(postponed_exception,
						     std::exception_ptr()));
}

bool
AsyncInputStream::IsEOF() const noexcept
{
	return (KnownSize() && offset >= size) ||
		(!open && buffer.empty());
}

void
AsyncInputStream::Seek(std::unique_lock<Mutex> &lock,
		       offset_type new_offset)
{
	assert(IsReady());
	assert(seek_state == SeekState::NONE);

	if (new_offset == offset)
		/* no-op */
		return;

	if (!IsSeekable())
		throw std::runtime_error("Not seekable");

	/* check if we can fast-forward the buffer */

	while (new_offset > offset) {
		auto r = buffer.Read();
		if (r.empty())
			break;

		const size_t nbytes =
			new_offset - offset < offset_type(r.size)
					       ? new_offset - offset
					       : r.size;

		buffer.Consume(nbytes);
		offset += nbytes;
	}

	if (new_offset == offset)
		return;

	/* no: ask the implementation to seek */

	seek_offset = new_offset;
	seek_state = SeekState::SCHEDULED;

	deferred_seek.Schedule();

	CondInputStreamHandler cond_handler;
	const ScopeExchangeInputStreamHandler h(*this, &cond_handler);
	cond_handler.cond.wait(lock,
			       [this]{ return seek_state == SeekState::NONE; });

	Check();
}

void
AsyncInputStream::SeekDone() noexcept
{
	assert(GetEventLoop().IsInside());
	assert(IsSeekPending());

	/* we may have reached end-of-file previously, and the
	   connection may have been closed already; however after
	   seeking successfully, the connection must be alive again */
	open = true;

	seek_state = SeekState::NONE;
	InvokeOnAvailable();
}

std::unique_ptr<Tag>
AsyncInputStream::ReadTag() noexcept
{
	return std::exchange(tag, nullptr);
}

bool
AsyncInputStream::IsAvailable() const noexcept
{
	return postponed_exception ||
		IsEOF() ||
		!buffer.empty();
}

size_t
AsyncInputStream::Read(std::unique_lock<Mutex> &lock,
		       void *ptr, size_t read_size)
{
	assert(!GetEventLoop().IsInside());

	CondInputStreamHandler cond_handler;

	/* wait for data */
	CircularBuffer<uint8_t>::Range r;
	while (true) {
		Check();

		r = buffer.Read();
		if (!r.empty() || IsEOF())
			break;

		const ScopeExchangeInputStreamHandler h(*this, &cond_handler);
		cond_handler.cond.wait(lock);
	}

	const size_t nbytes = std::min(read_size, r.size);
	memcpy(ptr, r.data, nbytes);
	buffer.Consume(nbytes);

	offset += offset_type(nbytes);

	if (paused && buffer.GetSize() < resume_at)
		deferred_resume.Schedule();

	return nbytes;
}

void
AsyncInputStream::CommitWriteBuffer(size_t nbytes) noexcept
{
	buffer.Append(nbytes);

	if (!IsReady())
		SetReady();
	else
		InvokeOnAvailable();
}

void
AsyncInputStream::AppendToBuffer(const void *data, size_t append_size) noexcept
{
	auto w = buffer.Write();
	assert(!w.empty());

	size_t nbytes = std::min(w.size, append_size);
	memcpy(w.data, data, nbytes);
	buffer.Append(nbytes);

	const size_t remaining = append_size - nbytes;
	if (remaining > 0) {
		w = buffer.Write();
		assert(!w.empty());
		assert(w.size >= remaining);

		memcpy(w.data, static_cast<const uint8_t *>(data) + nbytes, remaining);
		buffer.Append(remaining);
	}

	if (!IsReady())
		SetReady();
	else
		InvokeOnAvailable();
}

void
AsyncInputStream::DeferredResume() noexcept
{
	const std::lock_guard<Mutex> protect(mutex);

	try {
		Resume();
	} catch (...) {
		postponed_exception = std::current_exception();
		InvokeOnAvailable();
	}
}

void
AsyncInputStream::DeferredSeek() noexcept
{
	const std::lock_guard<Mutex> protect(mutex);
	if (seek_state != SeekState::SCHEDULED)
		return;

	try {
		Resume();

		seek_state = SeekState::PENDING;
		buffer.Clear();
		paused = false;

		DoSeek(seek_offset);
	} catch (...) {
		seek_state = SeekState::NONE;
		postponed_exception = std::current_exception();
		InvokeOnAvailable();
	}
}
