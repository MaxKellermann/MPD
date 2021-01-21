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

#include "HttpdClient.hxx"
#include "HttpdInternal.hxx"
#include "util/ASCII.hxx"
#include "util/AllocatedString.hxx"
#include "Page.hxx"
#include "IcyMetaDataServer.hxx"
#include "net/SocketError.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "Log.hxx"

#include <cassert>
#include <cstring>

#include <stdio.h>

HttpdClient::~HttpdClient() noexcept
{
	if (IsDefined())
		BufferedSocket::Close();
}

void
HttpdClient::Close() noexcept
{
	httpd.RemoveClient(*this);
}

void
HttpdClient::LockClose() noexcept
{
	const std::lock_guard<Mutex> protect(httpd.mutex);
	Close();
}

void
HttpdClient::BeginResponse() noexcept
{
	assert(state != State::RESPONSE);

	state = State::RESPONSE;
	current_page = nullptr;

	if (!head_method)
		httpd.SendHeader(*this);
}

/**
 * Handle a line of the HTTP request.
 */
bool
HttpdClient::HandleLine(const char *line) noexcept
{
	assert(state != State::RESPONSE);

	if (state == State::REQUEST) {
		if (strncmp(line, "HEAD /", 6) == 0) {
			line += 6;
			head_method = true;
		} else if (strncmp(line, "GET /", 5) == 0) {
			line += 5;
		} else {
			/* only GET is supported */
			LogWarning(httpd_output_domain,
				   "malformed request line from client");
			return false;
		}

		/* blacklist some well-known request paths */
		if ((strncmp(line, "favicon.ico", 11) == 0 &&
		     (line[11] == '\0' || line[11] == ' ')) ||
		    (strncmp(line, "robots.txt", 10) == 0 &&
		     (line[10] == '\0' || line[10] == ' ')) ||
		    (strncmp(line, "sitemap.xml", 11) == 0 &&
		     (line[11] == '\0' || line[11] == ' ')) ||
		    (strncmp(line, ".well-known/", 12) == 0)) {
			should_reject = true;
		}

		line = std::strchr(line, ' ');
		if (line == nullptr || strncmp(line + 1, "HTTP/", 5) != 0) {
			/* HTTP/0.9 without request headers */

			if (head_method)
				return false;

			BeginResponse();
			return true;
		}

		/* after the request line, request headers follow */
		state = State::HEADERS;
		return true;
	} else {
		if (*line == 0) {
			/* empty line: request is finished */

			BeginResponse();
			return true;
		}

		if (StringEqualsCaseASCII(line, "Icy-MetaData: 1", 15) ||
		    StringEqualsCaseASCII(line, "Icy-MetaData:1", 14)) {
			/* Send icy metadata */
			metadata_requested = metadata_supported;
			return true;
		}

		/* expect more request headers */
		return true;
	}
}

/**
 * Sends the status line and response headers to the client.
 */
bool
HttpdClient::SendResponse() noexcept
{
	char buffer[1024];
	AllocatedString<> allocated = nullptr;
	const char *response;

	assert(state == State::RESPONSE);

	if (should_reject) {
		response =
			"HTTP/1.1 404 not found\r\n"
			"Content-Type: text/plain\r\n"
			"Connection: close\r\n"
			"\r\n"
			"404 not found";
	} else if (metadata_requested) {
		allocated =
			icy_server_metadata_header(httpd.name, httpd.genre,
						   httpd.website,
						   httpd.content_type,
						   metaint);
		response = allocated.c_str();
	} else { /* revert to a normal HTTP request */
		snprintf(buffer, sizeof(buffer),
			 "HTTP/1.1 200 OK\r\n"
			 "Content-Type: %s\r\n"
			 "Connection: close\r\n"
			 "Pragma: no-cache\r\n"
			 "Cache-Control: no-cache, no-store\r\n"
			 "\r\n",
			 httpd.content_type);
		response = buffer;
	}

	ssize_t nbytes = GetSocket().Write(response, strlen(response));
	if (gcc_unlikely(nbytes < 0)) {
		const SocketErrorMessage msg;
		FormatWarning(httpd_output_domain,
			      "failed to write to client: %s",
			      (const char *)msg);
		LockClose();
		return false;
	}

	return true;
}

HttpdClient::HttpdClient(HttpdOutput &_httpd, UniqueSocketDescriptor _fd,
			 EventLoop &_loop,
			 bool _metadata_supported)
	:BufferedSocket(_fd.Release(), _loop),
	 httpd(_httpd),
	 metadata_supported(_metadata_supported)
{
}

void
HttpdClient::ClearQueue() noexcept
{
	assert(state == State::RESPONSE);

	while (!pages.empty()) {
#ifndef NDEBUG
		auto &page = pages.front();
		assert(queue_size >= page->GetSize());
		queue_size -= page->GetSize();
#endif

		pages.pop();
	}

	assert(queue_size == 0);
}

void
HttpdClient::CancelQueue() noexcept
{
	if (state != State::RESPONSE)
		return;

	ClearQueue();

	if (current_page == nullptr)
		event.CancelWrite();
}

ssize_t
HttpdClient::TryWritePage(const Page &page, size_t position) noexcept
{
	assert(position < page.GetSize());

	return GetSocket().Write(page.GetData() + position,
				 page.GetSize() - position);
}

ssize_t
HttpdClient::TryWritePageN(const Page &page,
			   size_t position, ssize_t n) noexcept
{
	return n >= 0
		? GetSocket().Write(page.GetData() + position, n)
		: TryWritePage(page, position);
}

ssize_t
HttpdClient::GetBytesTillMetaData() const noexcept
{
	if (metadata_requested &&
	    current_page->GetSize() - current_position > metaint - metadata_fill)
		return metaint - metadata_fill;

	return -1;
}

inline bool
HttpdClient::TryWrite() noexcept
{
	const std::lock_guard<Mutex> protect(httpd.mutex);

	assert(state == State::RESPONSE);

	if (current_page == nullptr) {
		if (pages.empty()) {
			/* another thread has removed the event source
			   while this thread was waiting for
			   httpd.mutex */
			event.CancelWrite();
			return true;
		}

		current_page = pages.front();
		pages.pop();
		current_position = 0;

		assert(queue_size >= current_page->GetSize());
		queue_size -= current_page->GetSize();
	}

	const ssize_t bytes_to_write = GetBytesTillMetaData();
	if (bytes_to_write == 0) {
		if (!metadata_sent) {
			ssize_t nbytes = TryWritePage(*metadata,
						      metadata_current_position);
			if (nbytes < 0) {
				auto e = GetSocketError();
				if (IsSocketErrorAgain(e))
					return true;

				if (!IsSocketErrorClosed(e)) {
					SocketErrorMessage msg(e);
					FormatWarning(httpd_output_domain,
						      "failed to write to client: %s",
						      (const char *)msg);
				}

				Close();
				return false;
			}

			metadata_current_position += nbytes;

			if (metadata->GetSize() - metadata_current_position == 0) {
				metadata_fill = 0;
				metadata_current_position = 0;
				metadata_sent = true;
			}
		} else {
			char empty_data = 0;

			ssize_t nbytes = GetSocket().Write(&empty_data, 1);
			if (nbytes < 0) {
				auto e = GetSocketError();
				if (IsSocketErrorAgain(e))
					return true;

				if (!IsSocketErrorClosed(e)) {
					SocketErrorMessage msg(e);
					FormatWarning(httpd_output_domain,
						      "failed to write to client: %s",
						      (const char *)msg);
				}

				Close();
				return false;
			}

			metadata_fill = 0;
			metadata_current_position = 0;
		}
	} else {
		ssize_t nbytes =
			TryWritePageN(*current_page, current_position,
				      bytes_to_write);
		if (nbytes < 0) {
			auto e = GetSocketError();
			if (IsSocketErrorAgain(e))
				return true;

			if (!IsSocketErrorClosed(e)) {
				SocketErrorMessage msg(e);
				FormatWarning(httpd_output_domain,
					      "failed to write to client: %s",
					      (const char *)msg);
			}

			Close();
			return false;
		}

		current_position += nbytes;
		assert(current_position <= current_page->GetSize());

		if (metadata_requested)
			metadata_fill += nbytes;

		if (current_position >= current_page->GetSize()) {
			current_page.reset();

			if (pages.empty())
				/* all pages are sent: remove the
				   event source */
				event.CancelWrite();
		}
	}

	return true;
}

void
HttpdClient::PushPage(PagePtr page) noexcept
{
	if (state != State::RESPONSE)
		/* the client is still writing the HTTP request */
		return;

	if (queue_size > 256 * 1024) {
		FormatDebug(httpd_output_domain,
			    "client is too slow, flushing its queue");
		ClearQueue();
	}

	queue_size += page->GetSize();
	pages.emplace(std::move(page));

	event.ScheduleWrite();
}

void
HttpdClient::PushMetaData(PagePtr page) noexcept
{
	assert(page != nullptr);

	metadata = std::move(page);
	metadata_sent = false;
}

void
HttpdClient::OnSocketReady(unsigned flags) noexcept
{
	if (flags & SocketEvent::WRITE)
		if (!TryWrite())
			return;

	BufferedSocket::OnSocketReady(flags);
}

BufferedSocket::InputResult
HttpdClient::OnSocketInput(void *data, size_t length) noexcept
{
	if (state == State::RESPONSE) {
		LogWarning(httpd_output_domain,
			   "unexpected input from client");
		LockClose();
		return InputResult::CLOSED;
	}

	char *line = static_cast<char *>(data);
	char *newline = static_cast<char *>(std::memchr(line, '\n', length));
	if (newline == nullptr)
		return InputResult::MORE;

	ConsumeInput(newline + 1 - line);

	if (newline > line && newline[-1] == '\r')
		--newline;

	/* terminate the string at the end of the line */
	*newline = 0;

	if (!HandleLine(line)) {
		LockClose();
		return InputResult::CLOSED;
	}

	if (state == State::RESPONSE) {
		if (!SendResponse())
			return InputResult::CLOSED;

		if (head_method || should_reject) {
			LockClose();
			return InputResult::CLOSED;
		}
	}

	return InputResult::AGAIN;
}

void
HttpdClient::OnSocketError(std::exception_ptr ep) noexcept
{
	LogError(ep);
	LockClose();
}

void
HttpdClient::OnSocketClosed() noexcept
{
	LockClose();
}
