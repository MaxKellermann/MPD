/*
 * Copyright 2003-2017 The Music Player Daemon Project
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

#include "config.h"
#include "SmbclientNeighborPlugin.hxx"
#include "lib/smbclient/Init.hxx"
#include "lib/smbclient/Domain.hxx"
#include "lib/smbclient/Mutex.hxx"
#include "neighbor/NeighborPlugin.hxx"
#include "neighbor/Explorer.hxx"
#include "neighbor/Listener.hxx"
#include "neighbor/Info.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"
#include "thread/Thread.hxx"
#include "thread/Name.hxx"
#include "Log.hxx"

#include <libsmbclient.h>

#include <algorithm>

#include "util/Domain.hxx"
#include "Log.hxx"

static constexpr Domain smbclient_neighbor("smbclient_neighbor");

class SmbclientNeighborExplorer final : public NeighborExplorer {
	struct Server {
		std::string name, comment, workgroup;

		bool alive;

		Server(std::string &&_name, std::string &&_comment, std::string &&_workgroup)
			:name(std::move(_name)), comment(std::move(_comment)), workgroup(_workgroup),
			 alive(true) {}
		Server(const Server &) = delete;

		gcc_pure
		bool operator==(const Server &other) const noexcept {
			return name == other.name;
		}

		gcc_pure
		NeighborInfo Export() const noexcept {
			return { "smb://" + name + "/", comment,"", workgroup};
		}
	};

	Thread thread;

	mutable Mutex mutex;
	Cond cond;

	List list;

	bool quit;

public:
	SmbclientNeighborExplorer(NeighborListener &_listener)
		:NeighborExplorer(_listener),
		 thread(BIND_THIS_METHOD(ThreadFunc)) {}

	/* virtual methods from class NeighborExplorer */
	void Open() override;
	void Reopen(int n) override;
	void Close() noexcept override;
	List GetList() const noexcept override;

private:
	void Run();
	void ThreadFunc();
};

void
SmbclientNeighborExplorer::Open()
{
	quit = false;
	list.clear();
	thread.Start();
}

void
SmbclientNeighborExplorer::Reopen(int n)
{
	FormatDefault(smbclient_neighbor, "%s %d", __func__, __LINE__);
	Close();
	if (n > 0) {
		SmbclientReinit();
		Open();
	}
}

void
SmbclientNeighborExplorer::Close() noexcept
{
	mutex.lock();
	quit = true;
	cond.signal();
	mutex.unlock();

	if (thread.IsDefined()) {
		thread.Join();
	}
	scanning = 0;
}

NeighborExplorer::List
SmbclientNeighborExplorer::GetList() const noexcept
{
	const std::lock_guard<Mutex> protect(mutex);
	/*
	List list;
	for (const auto &i : servers)
		list.emplace_front(i.Export());
	*/
	return list;
}

static void
ReadServer(NeighborExplorer::List &list, const smbc_dirent &e, const char *wg)
{
	const std::string name(e.name, e.namelen);
	std::string comment(e.comment, e.commentlen);

	if (comment.empty() || comment.compare("")==0) {
		comment = name;
	}
	list.emplace_front("smb://" + name, comment, "", wg);
}

static void
ReadServers(NeighborExplorer::List &list, const char *uri, const char *wg);

static void
ReadWorkgroup(NeighborExplorer::List &list, const std::string &name)
{
	std::string uri = "smb://" + name;
	ReadServers(list, uri.c_str(), name.c_str());
}

static void
ReadEntry(NeighborExplorer::List &list, const smbc_dirent &e, const char *wg)
{
	switch (e.smbc_type) {
	case SMBC_WORKGROUP:
		ReadWorkgroup(list, std::string(e.name, e.namelen));
		break;

	case SMBC_SERVER:
		ReadServer(list, e, wg);
		break;
	}
}

static void
ReadServers(NeighborExplorer::List &list, int fd, const char *wg)
{
	smbc_dirent *e;
	while ((e = smbc_readdir(fd)) != nullptr)
		ReadEntry(list, *e, wg);

	smbc_closedir(fd);
}

static void
ReadServers(NeighborExplorer::List &list, const char *uri, const char *wg)
{
	int fd = smbc_opendir(uri);
	if (fd >= 0) {
		ReadServers(list, fd, wg);
		smbc_closedir(fd);
	} else
		FormatErrno(smbclient_domain, "smbc_opendir('%s') failed",
			    uri);
}

gcc_pure
static NeighborExplorer::List
DetectServers() noexcept
{
	NeighborExplorer::List list;
	const std::lock_guard<Mutex> protect(smbclient_mutex);
	ReadServers(list, "smb://", "");
	return list;
}

gcc_pure
static NeighborExplorer::List::const_iterator
FindBeforeServerByURI(NeighborExplorer::List::const_iterator prev,
		      NeighborExplorer::List::const_iterator end,
		      const std::string &uri) noexcept
{
	for (auto i = std::next(prev); i != end; prev = i, i = std::next(prev))
		if (i->uri == uri)
			return prev;

	return end;
}

inline void
SmbclientNeighborExplorer::Run()
{
	List found = DetectServers(), lost;

	mutex.lock();

	const auto found_before_begin = found.before_begin();
	const auto found_end = found.end();

	for (auto prev = list.before_begin(), i = std::next(prev), end = list.end();
	     i != end; i = std::next(prev)) {
		auto f = FindBeforeServerByURI(found_before_begin, found_end,
					       i->uri);
		if (f != found_end) {
			/* still visible: remove from "found" so we
			   don't believe it's a new one */
			*i = std::move(*std::next(f));
			found.erase_after(f);
			prev = i;
		} else {
			/* can't see it anymore: move to "lost" */
			lost.splice_after(lost.before_begin(), list, prev);
		}
	}

	for (auto prev = found_before_begin, i = std::next(prev);
	     i != found_end; prev = i, i = std::next(prev))
		list.push_front(*i);

	mutex.unlock();

	for (auto &i : lost)
		listener.LostNeighbor(i);

	for (auto &i : found)
		listener.FoundNeighbor(i);
}

inline void
SmbclientNeighborExplorer::ThreadFunc()
{
	SetThreadName("smbclientNeighbor");

	mutex.lock();
	scanning = 10;

	do {
		mutex.unlock();
		//FormatDefault(smbclient_neighbor, "%s start", __func__);
		Run();
		//FormatDefault(smbclient_neighbor, "%s end", __func__);

		mutex.lock();
		if (quit)
			break;

		// TODO: sleep for how long?
		scanning--;
		cond.timed_wait(mutex, std::chrono::seconds(2));
	} while (!quit && scanning);
	scanning = 0;

	mutex.unlock();
}

static std::unique_ptr<NeighborExplorer>
smbclient_neighbor_create(gcc_unused EventLoop &loop,
			  NeighborListener &listener,
			  gcc_unused const ConfigBlock &block)
{
	SmbclientInit();

	return std::make_unique<SmbclientNeighborExplorer>(listener);
}

const NeighborPlugin smbclient_neighbor_plugin = {
	"smbclient",
	smbclient_neighbor_create,
};
