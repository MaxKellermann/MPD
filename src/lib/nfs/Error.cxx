/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
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

#include "Error.hxx"
#include "util/StringFormat.hxx"

extern "C" {
#include <nfsc/libnfs.h>
}

#include <cassert>

#include <string.h>

static StringBuffer<256>
FormatNfsClientError(struct nfs_context *nfs, const char *msg) noexcept
{
	assert(msg != nullptr);

	const char *msg2 = nfs_get_error(nfs);
	return StringFormat<256>("%s: %s", msg, msg2);
}

NfsClientError::NfsClientError(struct nfs_context *nfs, const char *msg) noexcept
	:std::runtime_error(FormatNfsClientError(nfs, msg).c_str()),
	 code(0) {}

static StringBuffer<256>
FormatNfsClientError(int err, struct nfs_context *nfs, void *data,
		     const char *msg) noexcept
{
	assert(msg != nullptr);
	assert(err < 0);

	const char *msg2 = static_cast<const char *>(data);
	if (data == nullptr || *static_cast<const char *>(data) == 0) {
		msg2 = nfs_get_error(nfs);
		if (msg2 == nullptr)
			msg2 = strerror(-err);
	}

	return StringFormat<256>("%s: %s", msg, msg2);
}

NfsClientError::NfsClientError(int err, struct nfs_context *nfs, void *data,
			       const char *msg) noexcept
	:std::runtime_error(FormatNfsClientError(err, nfs, data, msg).c_str()),
	 code(-err) {}
