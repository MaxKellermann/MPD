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

#include "FileReader.hxx"
#include "Glue.hxx"
#include "Base.hxx"
#include "Connection.hxx"
#include "event/Call.hxx"
#include "util/ASCII.hxx"

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <fcntl.h>

NfsFileReader::NfsFileReader() noexcept
	:defer_open(nfs_get_event_loop(), BIND_THIS_METHOD(OnDeferredOpen))
{
}

NfsFileReader::~NfsFileReader() noexcept
{
	assert(state == State::INITIAL);
}

void
NfsFileReader::Close() noexcept
{
	if (state == State::INITIAL)
		return;

	if (state == State::DEFER) {
		state = State::INITIAL;
		defer_open.Cancel();
		return;
	}

	/* this cancels State::MOUNT */
	connection->RemoveLease(*this);

	CancelOrClose();
}

void
NfsFileReader::CancelOrClose() noexcept
{
	assert(state != State::INITIAL &&
	       state != State::DEFER);

	if (state == State::IDLE)
		/* no async operation in progress: can close
		   immediately */
		connection->Close(fh);
	else if (state > State::OPEN)
		/* one async operation in progress: cancel it and
		   defer the nfs_close_async() call */
		connection->CancelAndClose(fh, *this);
	else if (state > State::MOUNT)
		/* we don't have a file handle yet - just cancel the
		   async operation */
		connection->Cancel(*this);

	state = State::INITIAL;
}

void
NfsFileReader::DeferClose() noexcept
{
	BlockingCall(GetEventLoop(), [this](){ Close(); });
}

void
NfsFileReader::Open(const char *uri)
{
	assert(state == State::INITIAL);

	if (!StringStartsWithCaseASCII(uri, "nfs://"))
		throw std::runtime_error("Malformed nfs:// URI");

	uri += 6;

	const char *slash = std::strchr(uri, '/');
	if (slash == nullptr)
		throw std::runtime_error("Malformed nfs:// URI");

	server = std::string(uri, slash);

	uri = slash;

	const char *new_path = nfs_check_base(server.c_str(), uri);
	if (new_path != nullptr) {
		export_name = std::string(uri, new_path);
		if (*new_path == 0)
			new_path = "/";
		path = new_path;
	} else {
		slash = std::strrchr(uri + 1, '/');
		if (slash == nullptr || slash[1] == 0)
			throw std::runtime_error("Malformed nfs:// URI");

		export_name = std::string(uri, slash);
		path = slash;
	}

	state = State::DEFER;
	defer_open.Schedule();
}

void
NfsFileReader::Read(uint64_t offset, size_t size)
{
	assert(state == State::IDLE);

	connection->Read(fh, offset, size, *this);
	state = State::READ;
}

void
NfsFileReader::CancelRead() noexcept
{
	if (state == State::READ) {
		connection->Cancel(*this);
		state = State::IDLE;
	}
}

void
NfsFileReader::OnNfsConnectionReady() noexcept
{
	assert(state == State::MOUNT);

	try {
		connection->Open(path, O_RDONLY, *this);
	} catch (...) {
		OnNfsFileError(std::current_exception());
		return;
	}

	state = State::OPEN;
}

void
NfsFileReader::OnNfsConnectionFailed(std::exception_ptr e) noexcept
{
	assert(state == State::MOUNT);

	state = State::INITIAL;

	OnNfsFileError(std::move(e));
}

void
NfsFileReader::OnNfsConnectionDisconnected(std::exception_ptr e) noexcept
{
	assert(state > State::MOUNT);

	CancelOrClose();

	OnNfsFileError(std::move(e));
}

inline void
NfsFileReader::OpenCallback(nfsfh *_fh) noexcept
{
	assert(connection != nullptr);
	assert(_fh != nullptr);

	fh = _fh;

	try {
		connection->Stat(fh, *this);
	} catch (...) {
		OnNfsFileError(std::current_exception());
		return;
	}

	state = State::STAT;
}

inline void
NfsFileReader::StatCallback(const struct stat *_st) noexcept
{
	assert(connection != nullptr);
	assert(fh != nullptr);
	assert(_st != nullptr);

#if defined(_WIN32) && !defined(_WIN64)
	/* on 32-bit Windows, libnfs enables -D_FILE_OFFSET_BITS=64,
	   but MPD (Meson) doesn't - to work around this mismatch, we
	   cast explicitly to "struct stat64" */
	const auto *st = (const struct stat64 *)_st;
#else
	const auto *st = _st;
#endif

	if (!S_ISREG(st->st_mode)) {
		OnNfsFileError(std::make_exception_ptr(std::runtime_error("Not a regular file")));
		return;
	}

	OnNfsFileOpen(st->st_size);
}

void
NfsFileReader::OnNfsCallback(unsigned status, void *data) noexcept
{
	switch (std::exchange(state, State::IDLE)) {
	case State::INITIAL:
	case State::DEFER:
	case State::MOUNT:
	case State::IDLE:
		assert(false);
		gcc_unreachable();

	case State::OPEN:
		OpenCallback(static_cast<struct nfsfh *>(data));
		break;

	case State::STAT:
		StatCallback(static_cast<const struct stat *>(data));
		break;

	case State::READ:
		OnNfsFileRead(data, status);
		break;
	}
}

void
NfsFileReader::OnNfsError(std::exception_ptr &&e) noexcept
{
	switch (state) {
	case State::INITIAL:
	case State::DEFER:
	case State::MOUNT:
	case State::IDLE:
		assert(false);
		gcc_unreachable();

	case State::OPEN:
		connection->RemoveLease(*this);
		state = State::INITIAL;
		break;

	case State::STAT:
		connection->RemoveLease(*this);
		connection->Close(fh);
		state = State::INITIAL;
		break;

	case State::READ:
		state = State::IDLE;
		break;
	}

	OnNfsFileError(std::move(e));
}

void
NfsFileReader::OnDeferredOpen() noexcept
{
	assert(state == State::DEFER);

	state = State::MOUNT;

	connection = &nfs_get_connection(server.c_str(), export_name.c_str());
	connection->AddLease(*this);
}
