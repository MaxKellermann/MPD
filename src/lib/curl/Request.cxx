/*
 * Copyright 2008-2018 Max Kellermann <max.kellermann@gmail.com>
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

#include "config.h"
#include "Request.hxx"
#include "Global.hxx"
#include "Handler.hxx"
#include "event/Call.hxx"
#include "util/RuntimeError.hxx"
#include "util/StringStrip.hxx"
#include "util/StringView.hxx"
#include "util/CharUtil.hxx"
#include "Version.h"

#include <curl/curl.h>

#include <algorithm>
#include <cassert>

#include <string.h>

CurlRequest::CurlRequest(CurlGlobal &_global,
			 CurlResponseHandler &_handler)
	:global(_global), handler(_handler)
{
	error_buffer[0] = 0;

	easy.SetPrivate((void *)this);
	easy.SetUserAgent("Music Player Daemon " VERSION);
	easy.SetHeaderFunction(_HeaderFunction, this);
	easy.SetWriteFunction(WriteFunction, this);
#if !defined(ANDROID) && !defined(_WIN32)
	easy.SetOption(CURLOPT_NETRC, 1L);
#endif
	easy.SetErrorBuffer(error_buffer);
	easy.SetNoProgress();
	easy.SetNoSignal();
	easy.SetConnectTimeout(10);
	easy.SetOption(CURLOPT_HTTPAUTH, long(CURLAUTH_ANY));
}

CurlRequest::~CurlRequest() noexcept
{
	FreeEasy();
}

void
CurlRequest::Start()
{
	assert(!registered);

	global.Add(*this);
	registered = true;
}

void
CurlRequest::StartIndirect()
{
	BlockingCall(global.GetEventLoop(), [this](){
			Start();
		});
}

void
CurlRequest::Stop() noexcept
{
	if (!registered)
		return;

	global.Remove(*this);
	registered = false;
}

void
CurlRequest::StopIndirect()
{
	BlockingCall(global.GetEventLoop(), [this](){
			Stop();
		});
}

void
CurlRequest::FreeEasy() noexcept
{
	if (!easy)
		return;

	Stop();
	easy = nullptr;
}

void
CurlRequest::Resume() noexcept
{
	assert(registered);

	easy.Unpause();

	global.InvalidateSockets();
}

void
CurlRequest::FinishHeaders()
{
	if (state != State::HEADERS)
		return;

	state = State::BODY;

	long status = 0;
	easy.GetInfo(CURLINFO_RESPONSE_CODE, &status);

	handler.OnHeaders(status, std::move(headers));
}

void
CurlRequest::FinishBody()
{
	FinishHeaders();

	if (state != State::BODY)
		return;

	state = State::CLOSED;
	handler.OnEnd();
}

void
CurlRequest::Done(CURLcode result) noexcept
{
	Stop();

	try {
		if (result != CURLE_OK) {
			StripRight(error_buffer);
			const char *msg = error_buffer;
			if (*msg == 0)
				msg = curl_easy_strerror(result);
			throw FormatRuntimeError("CURL failed: %s", msg);
		}

		FinishBody();
	} catch (...) {
		state = State::CLOSED;
		handler.OnError(std::current_exception());
	}
}

gcc_pure
static bool
IsResponseBoundaryHeader(StringView s) noexcept
{
	return s.size > 5 && (s.StartsWith("HTTP/") ||
			      /* the proprietary "ICY 200 OK" is
				 emitted by Shoutcast */
			      s.StartsWith("ICY 2"));
}

inline void
CurlRequest::HeaderFunction(StringView s) noexcept
{
	if (state > State::HEADERS)
		return;

	if (IsResponseBoundaryHeader(s)) {
		/* this is the boundary to a new response, for example
		   after a redirect */
		headers.clear();
		return;
	}

	const char *header = s.data;
	const char *end = StripRight(header, header + s.size);

	const char *value = s.Find(':');
	if (value == nullptr)
		return;

	std::string name(header, value);
	std::transform(name.begin(), name.end(), name.begin(),
		       static_cast<char(*)(char)>(ToLowerASCII));

	/* skip the colon */

	++value;

	/* strip the value */

	value = StripLeft(value, end);
	end = StripRight(value, end);

	headers.emplace(std::move(name), std::string(value, end));
}

size_t
CurlRequest::_HeaderFunction(char *ptr, size_t size, size_t nmemb,
			     void *stream) noexcept
{
	CurlRequest &c = *static_cast<CurlRequest *>(stream);

	size *= nmemb;

	c.HeaderFunction({ptr, size});
	return size;
}

inline size_t
CurlRequest::DataReceived(const void *ptr, size_t received_size) noexcept
{
	assert(received_size > 0);

	try {
		FinishHeaders();
		handler.OnData({ptr, received_size});
		return received_size;
	} catch (CurlResponseHandler::Pause) {
		return CURL_WRITEFUNC_PAUSE;
	}

}

size_t
CurlRequest::WriteFunction(char *ptr, size_t size, size_t nmemb,
			   void *stream) noexcept
{
	CurlRequest &c = *static_cast<CurlRequest *>(stream);

	size *= nmemb;
	if (size == 0)
		return 0;

	return c.DataReceived(ptr, size);
}
