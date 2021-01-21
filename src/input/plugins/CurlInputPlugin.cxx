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

#include "CurlInputPlugin.hxx"
#include "lib/curl/Error.hxx"
#include "lib/curl/Global.hxx"
#include "lib/curl/Init.hxx"
#include "lib/curl/Request.hxx"
#include "lib/curl/Handler.hxx"
#include "lib/curl/Slist.hxx"
#include "../MaybeBufferedInputStream.hxx"
#include "../AsyncInputStream.hxx"
#include "../IcyInputStream.hxx"
#include "IcyMetaDataParser.hxx"
#include "../InputPlugin.hxx"
#include "config/Block.hxx"
#include "tag/Builder.hxx"
#include "tag/Tag.hxx"
#include "event/Call.hxx"
#include "event/Loop.hxx"
#include "util/ASCII.hxx"
#include "util/StringFormat.hxx"
#include "util/StringView.hxx"
#include "util/NumberParser.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"
#include "PluginUnavailable.hxx"
#include "config.h"

#ifdef HAVE_ICU_CONVERTER
#include "lib/icu/Converter.hxx"
#include "util/AllocatedString.hxx"
#include "util/UriExtract.hxx"
#include "util/UriQueryParser.hxx"
#endif

#include <cassert>
#include <cinttypes>

#include <string.h>

#include <curl/curl.h>

/**
 * Do not buffer more than this number of bytes.  It should be a
 * reasonable limit that doesn't make low-end machines suffer too
 * much, but doesn't cause stuttering on high-latency lines.
 */
static const size_t CURL_MAX_BUFFERED = 512 * 1024;

/**
 * Resume the stream at this number of bytes after it has been paused.
 */
static const size_t CURL_RESUME_AT = 384 * 1024;

class CurlInputStream final : public AsyncInputStream, CurlResponseHandler {
	/* some buffers which were passed to libcurl, which we have
	   too free */
	CurlSlist request_headers;

	CurlRequest *request = nullptr;

	/** parser for icy-metadata */
	std::shared_ptr<IcyMetaDataParser> icy;

public:
	template<typename I>
	CurlInputStream(EventLoop &event_loop, const char *_url,
			const std::multimap<std::string, std::string> &headers,
			I &&_icy,
			Mutex &_mutex);

	~CurlInputStream() noexcept override;

	CurlInputStream(const CurlInputStream &) = delete;
	CurlInputStream &operator=(const CurlInputStream &) = delete;

	static InputStreamPtr Open(const char *url,
				   const std::multimap<std::string, std::string> &headers,
				   Mutex &mutex);

private:
	/**
	 * Create and initialize a new #CurlRequest instance.  After
	 * this, you may add more request headers and set options.  To
	 * actually start the request, call StartRequest().
	 */
	void InitEasy();

	/**
	 * Start the request after having called InitEasy().  After
	 * this, you must not set any CURL options.
	 */
	void StartRequest();

	/**
	 * Frees the current "libcurl easy" handle, and everything
	 * associated with it.
	 *
	 * Runs in the I/O thread.
	 */
	void FreeEasy() noexcept;

	/**
	 * Frees the current "libcurl easy" handle, and everything associated
	 * with it.
	 *
	 * The mutex must not be locked.
	 */
	void FreeEasyIndirect() noexcept;

	/**
	 * The DoSeek() implementation invoked in the IOThread.
	 */
	void SeekInternal(offset_type new_offset);

	/* virtual methods from CurlResponseHandler */
	void OnHeaders(unsigned status,
		       std::multimap<std::string, std::string> &&headers) override;
	void OnData(ConstBuffer<void> data) override;
	void OnEnd() override;
	void OnError(std::exception_ptr e) noexcept override;

	/* virtual methods from AsyncInputStream */
	void DoResume() override;
	void DoSeek(offset_type new_offset) override;
};

/** libcurl should accept "ICY 200 OK" */
static struct curl_slist *http_200_aliases;

/** HTTP proxy settings */
static const char *proxy, *proxy_user, *proxy_password;
static unsigned proxy_port;

static bool verify_peer, verify_host;

static CurlInit *curl_init;

static constexpr Domain curl_domain("curl");

void
CurlInputStream::DoResume()
{
	assert(GetEventLoop().IsInside());

	const ScopeUnlock unlock(mutex);
	request->Resume();
}

void
CurlInputStream::FreeEasy() noexcept
{
	assert(GetEventLoop().IsInside());

	if (request == nullptr)
		return;

	delete request;
	request = nullptr;
}

void
CurlInputStream::FreeEasyIndirect() noexcept
{
	BlockingCall(GetEventLoop(), [this](){
			FreeEasy();
		});
}

#ifdef HAVE_ICU_CONVERTER

static std::unique_ptr<IcuConverter>
CreateIcuConverterForUri(const char *uri)
{
	const char *fragment = uri_get_fragment(uri);
	if (fragment == nullptr)
		return nullptr;

	const auto charset = UriFindRawQueryParameter(fragment, "charset");
	if (charset == nullptr)
		return nullptr;

	const std::string copy(charset.data, charset.size);
	return IcuConverter::Create(copy.c_str());
}

#endif

template<typename F>
static void
WithConvertedTagValue(const char *uri, const char *value, F &&f) noexcept
{
#ifdef HAVE_ICU_CONVERTER
	try {
		auto converter = CreateIcuConverterForUri(uri);
		if (converter) {
			f(converter->ToUTF8(value).c_str());
			return;
		}
	} catch (...) {
	}
#else
	(void)uri;
#endif

	f(value);
}

void
CurlInputStream::OnHeaders(unsigned status,
			   std::multimap<std::string, std::string> &&headers)
{
	assert(GetEventLoop().IsInside());
	assert(!postponed_exception);
	assert(!icy || !icy->IsDefined());

	if (status < 200 || status >= 300)
		throw HttpStatusError(status,
				      StringFormat<40>("got HTTP status %u",
						       status).c_str());

	const std::lock_guard<Mutex> protect(mutex);

	if (IsSeekPending()) {
		/* don't update metadata while seeking */
		SeekDone();
		return;
	}

	if (headers.find("accept-ranges") != headers.end())
		seekable = true;

	auto i = headers.find("content-length");
	if (i != headers.end())
		size = offset + ParseUint64(i->second.c_str());

	i = headers.find("content-type");
	if (i != headers.end())
		SetMimeType(std::move(i->second));

	i = headers.find("icy-name");
	if (i == headers.end()) {
		i = headers.find("ice-name");
		if (i == headers.end())
			i = headers.find("x-audiocast-name");
	}

	if (i != headers.end()) {
		TagBuilder tag_builder;

		WithConvertedTagValue(GetURI(), i->second.c_str(),
				      [&tag_builder](const char *value){
					      tag_builder.AddItem(TAG_NAME,
								  value);
				      });

		SetTag(tag_builder.CommitNew());
	}

	if (icy) {
		i = headers.find("icy-metaint");

		if (i != headers.end()) {
			size_t icy_metaint = ParseUint64(i->second.c_str());
#ifndef _WIN32
			/* Windows doesn't know "%z" */
			FormatDebug(curl_domain, "icy-metaint=%zu", icy_metaint);
#endif

			if (icy_metaint > 0) {
				icy->Start(icy_metaint);

				/* a stream with icy-metadata is not
				   seekable */
				seekable = false;
			}
		}
	}

	SetReady();
}

void
CurlInputStream::OnData(ConstBuffer<void> data)
{
	assert(data.size > 0);

	const std::lock_guard<Mutex> protect(mutex);

	if (IsSeekPending())
		SeekDone();

	if (data.size > GetBufferSpace()) {
		AsyncInputStream::Pause();
		throw CurlResponseHandler::Pause{};
	}

	AppendToBuffer(data.data, data.size);
}

void
CurlInputStream::OnEnd()
{
	const std::lock_guard<Mutex> protect(mutex);
	InvokeOnAvailable();

	AsyncInputStream::SetClosed();
}

void
CurlInputStream::OnError(std::exception_ptr e) noexcept
{
	const std::lock_guard<Mutex> protect(mutex);
	postponed_exception = std::move(e);

	if (IsSeekPending())
		SeekDone();
	else if (!IsReady())
		SetReady();
	else
		InvokeOnAvailable();

	AsyncInputStream::SetClosed();
}

/*
 * InputPlugin methods
 *
 */

static void
input_curl_init(EventLoop &event_loop, const ConfigBlock &block)
{
	try {
		curl_init = new CurlInit(event_loop);
	} catch (...) {
		std::throw_with_nested(PluginUnavailable("CURL initialization failed"));
	}

	const auto version_info = curl_version_info(CURLVERSION_FIRST);
	if (version_info != nullptr) {
		FormatDebug(curl_domain, "version %s", version_info->version);
		if (version_info->features & CURL_VERSION_SSL)
			FormatDebug(curl_domain, "with %s",
				    version_info->ssl_version);
	}

	http_200_aliases = curl_slist_append(http_200_aliases, "ICY 200 OK");

	proxy = block.GetBlockValue("proxy");
	proxy_port = block.GetBlockValue("proxy_port", 0U);
	proxy_user = block.GetBlockValue("proxy_user");
	proxy_password = block.GetBlockValue("proxy_password");

	verify_peer = block.GetBlockValue("verify_peer", true);
	verify_host = block.GetBlockValue("verify_host", true);
}

static void
input_curl_finish() noexcept
{
	delete curl_init;

	curl_slist_free_all(http_200_aliases);
	http_200_aliases = nullptr;
}

template<typename I>
inline
CurlInputStream::CurlInputStream(EventLoop &event_loop, const char *_url,
				 const std::multimap<std::string, std::string> &headers,
				 I &&_icy,
				 Mutex &_mutex)
	:AsyncInputStream(event_loop, _url, _mutex,
			  CURL_MAX_BUFFERED,
			  CURL_RESUME_AT),
	 icy(std::forward<I>(_icy))
{
	request_headers.Append("Icy-Metadata: 1");

	for (const auto &i : headers)
		request_headers.Append((i.first + ":" + i.second).c_str());
}

CurlInputStream::~CurlInputStream() noexcept
{
	FreeEasyIndirect();
}

void
CurlInputStream::InitEasy()
{
	request = new CurlRequest(**curl_init, GetURI(), *this);

	request->SetOption(CURLOPT_HTTP200ALIASES, http_200_aliases);
	request->SetOption(CURLOPT_FOLLOWLOCATION, 1L);
	request->SetOption(CURLOPT_MAXREDIRS, 5L);
	request->SetOption(CURLOPT_FAILONERROR, 1L);

	if (proxy != nullptr)
		request->SetOption(CURLOPT_PROXY, proxy);

	if (proxy_port > 0)
		request->SetOption(CURLOPT_PROXYPORT, long(proxy_port));

	if (proxy_user != nullptr && proxy_password != nullptr)
		request->SetOption(CURLOPT_PROXYUSERPWD,
				   StringFormat<1024>("%s:%s", proxy_user,
						      proxy_password).c_str());

	request->SetOption(CURLOPT_SSL_VERIFYPEER, verify_peer ? 1L : 0L);
	request->SetOption(CURLOPT_SSL_VERIFYHOST, verify_host ? 2L : 0L);
	request->SetOption(CURLOPT_HTTPHEADER, request_headers.Get());
}

void
CurlInputStream::StartRequest()
{
	request->Start();
}

void
CurlInputStream::SeekInternal(offset_type new_offset)
{
	/* close the old connection and open a new one */

	FreeEasy();

	offset = new_offset;
	if (offset == size) {
		/* seek to EOF: simulate empty result; avoid
		   triggering a "416 Requested Range Not Satisfiable"
		   response */
		SeekDone();
		return;
	}

	InitEasy();

	/* send the "Range" header */

	if (offset > 0)
		request->SetOption(CURLOPT_RANGE,
				   StringFormat<40>("%" PRIoffset "-",
						    offset).c_str());

	StartRequest();
}

void
CurlInputStream::DoSeek(offset_type new_offset)
{
	assert(IsReady());
	assert(seekable);

	const ScopeUnlock unlock(mutex);

	BlockingCall(GetEventLoop(), [this, new_offset](){
			SeekInternal(new_offset);
		});
}

inline InputStreamPtr
CurlInputStream::Open(const char *url,
		      const std::multimap<std::string, std::string> &headers,
		      Mutex &mutex)
{
	auto icy = std::make_shared<IcyMetaDataParser>();

	auto c = std::make_unique<CurlInputStream>((*curl_init)->GetEventLoop(),
						   url, headers,
						   icy,
						   mutex);

	BlockingCall(c->GetEventLoop(), [&c](){
			c->InitEasy();
			c->StartRequest();
		});

	return std::make_unique<MaybeBufferedInputStream>(std::make_unique<IcyInputStream>(std::move(c), std::move(icy)));
}

InputStreamPtr
OpenCurlInputStream(const char *uri,
		    const std::multimap<std::string, std::string> &headers,
		    Mutex &mutex)
{
	return CurlInputStream::Open(uri, headers, mutex);
}

static InputStreamPtr
input_curl_open(const char *url, Mutex &mutex)
{
	if (!StringStartsWithCaseASCII(url, "http://") &&
	    !StringStartsWithCaseASCII(url, "https://"))
		return nullptr;

	return CurlInputStream::Open(url, {}, mutex);
}

static std::set<std::string>
input_curl_protocols() noexcept
{
	std::set<std::string> protocols;
	auto version_info = curl_version_info(CURLVERSION_FIRST);
	for (auto proto_ptr = version_info->protocols; *proto_ptr != nullptr; proto_ptr++) {
		if (protocol_is_whitelisted(*proto_ptr)) {
			std::string schema(*proto_ptr);
			schema.append("://");
			protocols.emplace(schema);
		}
	}
	return protocols;
}

const struct InputPlugin input_plugin_curl = {
	"curl",
	nullptr,
	input_curl_init,
	input_curl_finish,
	input_curl_open,
	input_curl_protocols
};
