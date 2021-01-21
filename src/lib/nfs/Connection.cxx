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

#include "Connection.hxx"
#include "Error.hxx"
#include "Lease.hxx"
#include "Callback.hxx"
#include "event/Loop.hxx"
#include "net/SocketDescriptor.hxx"
#include "util/RuntimeError.hxx"

extern "C" {
#include <nfsc/libnfs.h>
}

#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h> /* for POLLIN, POLLOUT */
#endif

static constexpr Event::Duration NFS_MOUNT_TIMEOUT =
	std::chrono::minutes(1);

inline void
NfsConnection::CancellableCallback::Stat(nfs_context *ctx,
					 const char *path)
{
	assert(connection.GetEventLoop().IsInside());

	int result = nfs_stat64_async(ctx, path, Callback, this);
	if (result < 0)
		throw FormatRuntimeError("nfs_stat64_async() failed: %s",
					 nfs_get_error(ctx));
}

inline void
NfsConnection::CancellableCallback::Lstat(nfs_context *ctx,
					  const char *path)
{
	assert(connection.GetEventLoop().IsInside());

	int result = nfs_lstat64_async(ctx, path, Callback, this);
	if (result < 0)
		throw FormatRuntimeError("nfs_lstat64_async() failed: %s",
					 nfs_get_error(ctx));
}

inline void
NfsConnection::CancellableCallback::OpenDirectory(nfs_context *ctx,
						  const char *path)
{
	assert(connection.GetEventLoop().IsInside());

	int result = nfs_opendir_async(ctx, path, Callback, this);
	if (result < 0)
		throw FormatRuntimeError("nfs_opendir_async() failed: %s",
					 nfs_get_error(ctx));
}

inline void
NfsConnection::CancellableCallback::Open(nfs_context *ctx,
					 const char *path, int flags)
{
	assert(connection.GetEventLoop().IsInside());

	int result = nfs_open_async(ctx, path, flags,
				    Callback, this);
	if (result < 0)
		throw FormatRuntimeError("nfs_open_async() failed: %s",
					 nfs_get_error(ctx));
}

inline void
NfsConnection::CancellableCallback::Stat(nfs_context *ctx,
					 struct nfsfh *fh)
{
	assert(connection.GetEventLoop().IsInside());

	int result = nfs_fstat_async(ctx, fh, Callback, this);
	if (result < 0)
		throw FormatRuntimeError("nfs_fstat_async() failed: %s",
					 nfs_get_error(ctx));
}

inline void
NfsConnection::CancellableCallback::Read(nfs_context *ctx, struct nfsfh *fh,
					 uint64_t offset, size_t size)
{
	assert(connection.GetEventLoop().IsInside());

	int result = nfs_pread_async(ctx, fh, offset, size, Callback, this);
	if (result < 0)
		throw FormatRuntimeError("nfs_pread_async() failed: %s",
					 nfs_get_error(ctx));
}

inline void
NfsConnection::CancellableCallback::CancelAndScheduleClose(struct nfsfh *fh) noexcept
{
	assert(connection.GetEventLoop().IsInside());
	assert(!open);
	assert(close_fh == nullptr);
	assert(fh != nullptr);

	close_fh = fh;
	Cancel();
}

inline void
NfsConnection::CancellableCallback::PrepareDestroyContext() noexcept
{
	assert(IsCancelled());

	if (close_fh != nullptr) {
		connection.InternalClose(close_fh);
		close_fh = nullptr;
	}
}

inline void
NfsConnection::CancellableCallback::Callback(int err, void *data) noexcept
{
	assert(connection.GetEventLoop().IsInside());

	if (!IsCancelled()) {
		assert(close_fh == nullptr);

		NfsCallback &cb = Get();

		connection.callbacks.Remove(*this);

		if (err >= 0)
			cb.OnNfsCallback(unsigned(err), data);
		else
			cb.OnNfsError(std::make_exception_ptr(NfsClientError(-err, static_cast<const char *>(data))));
	} else {
		if (open) {
			/* a nfs_open_async() call was cancelled - to
			   avoid a memory leak, close the newly
			   allocated file handle immediately */
			assert(close_fh == nullptr);

			if (err >= 0) {
				auto *fh = static_cast<struct nfsfh *>(data);
				connection.Close(fh);
			}
		} else if (close_fh != nullptr)
			connection.DeferClose(close_fh);

		connection.callbacks.Remove(*this);
	}
}

void
NfsConnection::CancellableCallback::Callback(int err,
					     [[maybe_unused]] struct nfs_context *nfs,
					     void *data,
					     void *private_data) noexcept
{
	CancellableCallback &c = *static_cast<CancellableCallback *>(private_data);
	c.Callback(err, data);
}

static constexpr unsigned
libnfs_to_events(int i) noexcept
{
	return ((i & POLLIN) ? SocketEvent::READ : 0) |
		((i & POLLOUT) ? SocketEvent::WRITE : 0);
}

static constexpr int
events_to_libnfs(unsigned i) noexcept
{
	return ((i & SocketEvent::READ) ? POLLIN : 0) |
		((i & SocketEvent::WRITE) ? POLLOUT : 0) |
		((i & SocketEvent::HANGUP) ? POLLHUP : 0) |
		((i & SocketEvent::ERROR) ? POLLERR : 0);
}

NfsConnection::~NfsConnection() noexcept
{
	assert(GetEventLoop().IsInside());
	assert(new_leases.empty());
	assert(active_leases.empty());
	assert(callbacks.IsEmpty());
	assert(deferred_close.empty());

	if (context != nullptr)
		DestroyContext();
}

void
NfsConnection::AddLease(NfsLease &lease) noexcept
{
	assert(GetEventLoop().IsInside());

	new_leases.push_back(&lease);

	defer_new_lease.Schedule();
}

void
NfsConnection::RemoveLease(NfsLease &lease) noexcept
{
	assert(GetEventLoop().IsInside());

	new_leases.remove(&lease);
	active_leases.remove(&lease);
}

void
NfsConnection::Stat(const char *path, NfsCallback &callback)
{
	assert(GetEventLoop().IsInside());
	assert(!callbacks.Contains(callback));

	auto &c = callbacks.Add(callback, *this, false);
	try {
		c.Stat(context, path);
	} catch (...) {
		callbacks.Remove(c);
		throw;
	}

	ScheduleSocket();
}

void
NfsConnection::Lstat(const char *path, NfsCallback &callback)
{
	assert(GetEventLoop().IsInside());
	assert(!callbacks.Contains(callback));

	auto &c = callbacks.Add(callback, *this, false);
	try {
		c.Lstat(context, path);
	} catch (...) {
		callbacks.Remove(c);
		throw;
	}

	ScheduleSocket();
}

void
NfsConnection::OpenDirectory(const char *path, NfsCallback &callback)
{
	assert(GetEventLoop().IsInside());
	assert(!callbacks.Contains(callback));

	auto &c = callbacks.Add(callback, *this, true);
	try {
		c.OpenDirectory(context, path);
	} catch (...) {
		callbacks.Remove(c);
		throw;
	}

	ScheduleSocket();
}

const struct nfsdirent *
NfsConnection::ReadDirectory(struct nfsdir *dir) noexcept
{
	assert(GetEventLoop().IsInside());

	return nfs_readdir(context, dir);
}

void
NfsConnection::CloseDirectory(struct nfsdir *dir) noexcept
{
	assert(GetEventLoop().IsInside());

	return nfs_closedir(context, dir);
}

void
NfsConnection::Open(const char *path, int flags, NfsCallback &callback)
{
	assert(GetEventLoop().IsInside());
	assert(!callbacks.Contains(callback));

	auto &c = callbacks.Add(callback, *this, true);
	try {
		c.Open(context, path, flags);
	} catch (...) {
		callbacks.Remove(c);
		throw;
	}

	ScheduleSocket();
}

void
NfsConnection::Stat(struct nfsfh *fh, NfsCallback &callback)
{
	assert(GetEventLoop().IsInside());
	assert(!callbacks.Contains(callback));

	auto &c = callbacks.Add(callback, *this, false);
	try {
		c.Stat(context, fh);
	} catch (...) {
		callbacks.Remove(c);
		throw;
	}

	ScheduleSocket();
}

void
NfsConnection::Read(struct nfsfh *fh, uint64_t offset, size_t size,
		    NfsCallback &callback)
{
	assert(GetEventLoop().IsInside());
	assert(!callbacks.Contains(callback));

	auto &c = callbacks.Add(callback, *this, false);
	try {
		c.Read(context, fh, offset, size);
	} catch (...) {
		callbacks.Remove(c);
		throw;
	}

	ScheduleSocket();
}

void
NfsConnection::Cancel(NfsCallback &callback) noexcept
{
	callbacks.Cancel(callback);
}

static void
DummyCallback(int, struct nfs_context *, void *, void *) noexcept
{
}

inline void
NfsConnection::InternalClose(struct nfsfh *fh) noexcept
{
	assert(GetEventLoop().IsInside());
	assert(context != nullptr);
	assert(fh != nullptr);

	nfs_close_async(context, fh, DummyCallback, nullptr);
}

void
NfsConnection::Close(struct nfsfh *fh) noexcept
{
	assert(GetEventLoop().IsInside());

	InternalClose(fh);
	ScheduleSocket();
}

void
NfsConnection::CancelAndClose(struct nfsfh *fh, NfsCallback &callback) noexcept
{
	CancellableCallback &cancel = callbacks.Get(callback);
	cancel.CancelAndScheduleClose(fh);
}

void
NfsConnection::DestroyContext() noexcept
{
	assert(GetEventLoop().IsInside());
	assert(context != nullptr);

#ifndef NDEBUG
	assert(!in_destroy);
	in_destroy = true;
#endif

	if (!mount_finished) {
		assert(mount_timeout_event.IsPending());
		mount_timeout_event.Cancel();
	}

	/* cancel pending DeferEvent that was scheduled to notify
	   new leases */
	defer_new_lease.Cancel();

	socket_event.ReleaseSocket();

	callbacks.ForEach([](CancellableCallback &c){
			c.PrepareDestroyContext();
		});

	nfs_destroy_context(context);
	context = nullptr;
}

inline void
NfsConnection::DeferClose(struct nfsfh *fh) noexcept
{
	assert(GetEventLoop().IsInside());
	assert(in_event);
	assert(in_service);
	assert(context != nullptr);
	assert(fh != nullptr);

	deferred_close.push_front(fh);
}

void
NfsConnection::ScheduleSocket() noexcept
{
	assert(GetEventLoop().IsInside());
	assert(context != nullptr);

	const int which_events = nfs_which_events(context);

	if (which_events == POLLOUT)
		/* kludge: if libnfs asks only for POLLOUT, it means
		   that it is currently waiting for the connect() to
		   finish - rpc_reconnect_requeue() may have been
		   called from inside nfs_service(); we must now
		   unregister the old socket and register the new one
		   instead */
		socket_event.ReleaseSocket();

	if (!socket_event.IsDefined()) {
		SocketDescriptor _fd(nfs_get_fd(context));
		if (!_fd.IsDefined())
			return;

		_fd.EnableCloseOnExec();
		socket_event.Open(_fd);
	}

	socket_event.Schedule(libnfs_to_events(which_events));
}

inline int
NfsConnection::Service(unsigned flags) noexcept
{
	assert(GetEventLoop().IsInside());
	assert(context != nullptr);

#ifndef NDEBUG
	assert(!in_event);
	in_event = true;

	assert(!in_service);
	in_service = true;
#endif

	int result = nfs_service(context, events_to_libnfs(flags));

#ifndef NDEBUG
	assert(context != nullptr);
	assert(in_service);
	in_service = false;
#endif

	return result;
}

void
NfsConnection::OnSocketReady(unsigned flags) noexcept
{
	assert(GetEventLoop().IsInside());
	assert(deferred_close.empty());

	const bool was_mounted = mount_finished;
	if (!mount_finished || (flags & SocketEvent::HANGUP) != 0)
		/* until the mount is finished, the NFS client may use
		   various sockets, therefore we unregister and
		   re-register it each time */
		/* also re-register the socket if we got a HANGUP,
		   which is a sure sign that libnfs will close the
		   socket, which can lead to a race condition if
		   epoll_ctl() is called later */
		socket_event.ReleaseSocket();

	const int result = Service(flags);

	while (!deferred_close.empty()) {
		InternalClose(deferred_close.front());
		deferred_close.pop_front();
	}

	if (!was_mounted && mount_finished) {
		if (postponed_mount_error) {
			DestroyContext();
			BroadcastMountError(std::move(postponed_mount_error));
		} else if (result == 0)
			BroadcastMountSuccess();
	} else if (result < 0) {
		/* the connection has failed */

		auto e = FormatRuntimeError("NFS connection has failed: %s",
					    nfs_get_error(context));
		BroadcastError(std::make_exception_ptr(e));

		DestroyContext();
	} else if (nfs_get_fd(context) < 0) {
		/* this happens when rpc_reconnect_requeue() is called
		   after the connection broke, but autoreconnect was
		   disabled - nfs_service() returns 0 */

		const char *msg = nfs_get_error(context);
		if (msg == nullptr)
			msg = "<unknown>";
		auto e = FormatRuntimeError("NFS socket disappeared: %s", msg);

		BroadcastError(std::make_exception_ptr(e));

		DestroyContext();
	}

	assert(context == nullptr || nfs_get_fd(context) >= 0);

#ifndef NDEBUG
	assert(in_event);
	in_event = false;
#endif

	if (context != nullptr)
		ScheduleSocket();
}

inline void
NfsConnection::MountCallback(int status, [[maybe_unused]] nfs_context *nfs,
			     [[maybe_unused]] void *data) noexcept
{
	assert(GetEventLoop().IsInside());
	assert(context == nfs);

	mount_finished = true;

	assert(mount_timeout_event.IsPending() || in_destroy);
	mount_timeout_event.Cancel();

	if (status < 0) {
		auto e = FormatRuntimeError("nfs_mount_async() failed: %s",
					    nfs_get_error(context));
		postponed_mount_error = std::make_exception_ptr(e);
		return;
	}
}

void
NfsConnection::MountCallback(int status, nfs_context *nfs, void *data,
			     void *private_data) noexcept
{
	auto *c = static_cast<NfsConnection *>(private_data);

	c->MountCallback(status, nfs, data);
}

inline void
NfsConnection::MountInternal()
{
	assert(GetEventLoop().IsInside());
	assert(context == nullptr);

	context = nfs_init_context();
	if (context == nullptr)
		throw std::runtime_error("nfs_init_context() failed");

	postponed_mount_error = std::exception_ptr();
	mount_finished = false;

	mount_timeout_event.Schedule(NFS_MOUNT_TIMEOUT);

#ifndef NDEBUG
	in_service = false;
	in_event = false;
	in_destroy = false;
#endif

	if (nfs_mount_async(context, server.c_str(), export_name.c_str(),
			    MountCallback, this) != 0) {
		auto e = FormatRuntimeError("nfs_mount_async() failed: %s",
					    nfs_get_error(context));
		nfs_destroy_context(context);
		context = nullptr;
		throw e;
	}

	ScheduleSocket();
}

void
NfsConnection::BroadcastMountSuccess() noexcept
{
	assert(GetEventLoop().IsInside());

	while (!new_leases.empty()) {
		auto i = new_leases.begin();
		active_leases.splice(active_leases.end(), new_leases, i);
		(*i)->OnNfsConnectionReady();
	}
}

void
NfsConnection::BroadcastMountError(std::exception_ptr &&e) noexcept
{
	assert(GetEventLoop().IsInside());

	while (!new_leases.empty()) {
		auto l = new_leases.front();
		new_leases.pop_front();
		l->OnNfsConnectionFailed(e);
	}

	OnNfsConnectionError(std::move(e));
}

void
NfsConnection::BroadcastError(std::exception_ptr &&e) noexcept
{
	assert(GetEventLoop().IsInside());

	while (!active_leases.empty()) {
		auto l = active_leases.front();
		active_leases.pop_front();
		l->OnNfsConnectionDisconnected(e);
	}

	BroadcastMountError(std::move(e));
}

void
NfsConnection::OnMountTimeout() noexcept
{
	assert(GetEventLoop().IsInside());
	assert(!mount_finished);

	mount_finished = true;
	DestroyContext();

	BroadcastMountError(std::make_exception_ptr(std::runtime_error("Mount timeout")));
}

void
NfsConnection::RunDeferred() noexcept
{
	assert(GetEventLoop().IsInside());

	if (context == nullptr) {
		try {
			MountInternal();
		} catch (...) {
			BroadcastMountError(std::current_exception());
			return;
		}
	}

	if (mount_finished)
		BroadcastMountSuccess();
}
