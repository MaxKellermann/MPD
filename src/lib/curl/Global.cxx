/*
 * Copyright 2008-2020 Max Kellermann <max.kellermann@gmail.com>
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

#include "Global.hxx"
#include "Request.hxx"
#include "event/Loop.hxx"
#include "event/SocketEvent.hxx"

#include <cassert>

/**
 * Monitor for one socket created by CURL.
 */
class CurlSocket final {
	CurlGlobal &global;

	SocketEvent socket_event;

public:
	CurlSocket(CurlGlobal &_global, EventLoop &_loop, SocketDescriptor _fd)
		:global(_global),
		 socket_event(_loop, BIND_THIS_METHOD(OnSocketReady), _fd) {}

	~CurlSocket() noexcept {
		/* TODO: sometimes, CURL uses CURL_POLL_REMOVE after
		   closing the socket, and sometimes, it uses
		   CURL_POLL_REMOVE just to move the (still open)
		   connection to the pool; in the first case,
		   Abandon() would be most appropriate, but it breaks
		   the second case - is that a CURL bug?  is there a
		   better solution? */
	}

	auto &GetEventLoop() const noexcept {
		return socket_event.GetEventLoop();
	}

	/**
	 * Callback function for CURLMOPT_SOCKETFUNCTION.
	 */
	static int SocketFunction(CURL *easy,
				  curl_socket_t s, int action,
				  void *userp, void *socketp) noexcept;

private:
	SocketDescriptor GetSocket() const noexcept {
		return socket_event.GetSocket();
	}

	void OnSocketReady(unsigned events) noexcept;

	static constexpr int FlagsToCurlCSelect(unsigned flags) noexcept {
		return (flags & (SocketEvent::READ | SocketEvent::HANGUP) ? CURL_CSELECT_IN : 0) |
			(flags & SocketEvent::WRITE ? CURL_CSELECT_OUT : 0) |
			(flags & SocketEvent::ERROR ? CURL_CSELECT_ERR : 0);
	}

	gcc_const
	static unsigned CurlPollToFlags(int action) noexcept {
		switch (action) {
		case CURL_POLL_NONE:
			return 0;

		case CURL_POLL_IN:
			return SocketEvent::READ;

		case CURL_POLL_OUT:
			return SocketEvent::WRITE;

		case CURL_POLL_INOUT:
			return SocketEvent::READ|SocketEvent::WRITE;
		}

		assert(false);
		gcc_unreachable();
	}
};

CurlGlobal::CurlGlobal(EventLoop &_loop)
	:defer_read_info(_loop, BIND_THIS_METHOD(ReadInfo)),
	 timeout_event(_loop, BIND_THIS_METHOD(OnTimeout))
{
	multi.SetOption(CURLMOPT_SOCKETFUNCTION, CurlSocket::SocketFunction);
	multi.SetOption(CURLMOPT_SOCKETDATA, this);

	multi.SetOption(CURLMOPT_TIMERFUNCTION, TimerFunction);
	multi.SetOption(CURLMOPT_TIMERDATA, this);
}

int
CurlSocket::SocketFunction([[maybe_unused]] CURL *easy,
			   curl_socket_t s, int action,
			   void *userp, void *socketp) noexcept
{
	auto &global = *static_cast<CurlGlobal *>(userp);
	auto *cs = static_cast<CurlSocket *>(socketp);

	assert(global.GetEventLoop().IsInside());

	if (action == CURL_POLL_REMOVE) {
		delete cs;
		return 0;
	}

	if (cs == nullptr) {
		cs = new CurlSocket(global, global.GetEventLoop(),
				    SocketDescriptor(s));
		global.Assign(s, *cs);
	}

	unsigned flags = CurlPollToFlags(action);
	if (flags != 0)
		cs->socket_event.Schedule(flags);
	return 0;
}

void
CurlSocket::OnSocketReady(unsigned flags) noexcept
{
	assert(GetEventLoop().IsInside());

	global.SocketAction(GetSocket().Get(), FlagsToCurlCSelect(flags));
}

void
CurlGlobal::Add(CurlRequest &r)
{
	assert(GetEventLoop().IsInside());

	multi.Add(r.Get());

	InvalidateSockets();
}

void
CurlGlobal::Remove(CurlRequest &r) noexcept
{
	assert(GetEventLoop().IsInside());

	multi.Remove(r.Get());
}

/**
 * Find a request by its CURL "easy" handle.
 */
gcc_pure
static CurlRequest *
ToRequest(CURL *easy) noexcept
{
	void *p;
	CURLcode code = curl_easy_getinfo(easy, CURLINFO_PRIVATE, &p);
	if (code != CURLE_OK)
		return nullptr;

	return static_cast<CurlRequest *>(p);
}

inline void
CurlGlobal::ReadInfo() noexcept
{
	assert(GetEventLoop().IsInside());

	CURLMsg *msg;

	while ((msg = multi.InfoRead()) != nullptr) {
		if (msg->msg == CURLMSG_DONE) {
			auto *request = ToRequest(msg->easy_handle);
			if (request != nullptr)
				request->Done(msg->data.result);
		}
	}
}

void
CurlGlobal::SocketAction(curl_socket_t fd, int ev_bitmask) noexcept
{
	int running_handles;
	CURLMcode mcode = curl_multi_socket_action(multi.Get(), fd, ev_bitmask,
						   &running_handles);
	(void)mcode;

	defer_read_info.Schedule();
}

inline void
CurlGlobal::UpdateTimeout(long timeout_ms) noexcept
{
	if (timeout_ms < 0) {
		timeout_event.Cancel();
		return;
	}

	if (timeout_ms < 1)
		/* CURL's threaded resolver sets a timeout of 0ms, which
		   means we're running in a busy loop.  Quite a bad
		   idea to waste so much CPU.  Let's use a lower limit
		   of 1ms. */
		timeout_ms = 1;

	timeout_event.Schedule(std::chrono::milliseconds(timeout_ms));
}

int
CurlGlobal::TimerFunction([[maybe_unused]] CURLM *_multi, long timeout_ms,
			  void *userp) noexcept
{
	auto &global = *static_cast<CurlGlobal *>(userp);
	assert(_multi == global.multi.Get());

	global.UpdateTimeout(timeout_ms);
	return 0;
}

void
CurlGlobal::OnTimeout() noexcept
{
	SocketAction(CURL_SOCKET_TIMEOUT, 0);
}
