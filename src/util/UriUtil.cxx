/*
 * Copyright 2008-2019 Max Kellermann <max.kellermann@gmail.com>
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

#include "UriUtil.hxx"
#include "ASCII.hxx"

#include <cassert>
#include <cstring>

static const char *
verify_uri_segment(const char *p) noexcept
{
	unsigned dots = 0;
	while (*p == '.') {
		++p;
		++dots;
	}

	if (dots <= 2 && (*p == 0 || *p == '/'))
		return nullptr;

	const char *q = std::strchr(p + 1, '/');
	return q != nullptr ? q : "";
}

bool
uri_safe_local(const char *uri) noexcept
{
	while (true) {
		uri = verify_uri_segment(uri);
		if (uri == nullptr)
			return false;

		if (*uri == 0)
			return true;

		assert(*uri == '/');

		++uri;
	}
}

gcc_pure
static const char *
SkipUriScheme(const char *uri) noexcept
{
	static const char *const schemes[] = {
		"http://", "https://",
		"ftp://",
		"smb://",
	};

	for (auto scheme : schemes) {
		auto result = StringAfterPrefixCaseASCII(uri, scheme);
		if (result != nullptr)
			return result;
	}

	return nullptr;
}

std::string
uri_remove_auth(const char *uri) noexcept
{
	const char *auth = SkipUriScheme(uri);
	if (auth == nullptr)
		/* unrecognized URI */
		return std::string();

	const char *slash = std::strchr(auth, '/');
	if (slash == nullptr)
		slash = auth + strlen(auth);

	const char *at = static_cast<const char *>(std::memchr(auth, '@', slash - auth));
	if (at == nullptr)
		/* no auth info present, do nothing */
		return std::string();

	/* duplicate the full URI and then delete the auth
	   information */
	std::string result(uri);
	result.erase(auth - uri, at + 1 - auth);
	return result;
}
