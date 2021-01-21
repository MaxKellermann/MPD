/*
 * Copyright 2018-2019 Max Kellermann <max.kellermann@gmail.com>
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

#include "MD5.hxx"
#include "util/HexFormat.hxx"
#include "config.h"

#ifdef HAVE_LIBAVUTIL
extern "C" {
#include <libavutil/md5.h>
}
#else
#include "lib/gcrypt/MD5.hxx"
#include "lib/gcrypt/Init.hxx"
#endif

void
GlobalInitMD5() noexcept
{
#ifdef HAVE_LIBAVUTIL
	/* no initialization necessary */
#else
	Gcrypt::Init();
#endif
}

std::array<uint8_t, 16>
MD5(ConstBuffer<void> input) noexcept
{
#ifdef HAVE_LIBAVUTIL
	std::array<uint8_t, 16> result;
	av_md5_sum(&result.front(), static_cast<const uint8_t *>(input.data), input.size);
	return result;
#else
	return Gcrypt::MD5(input);
#endif
}

StringBuffer<33>
MD5Hex(ConstBuffer<void> input) noexcept
{
	const auto raw = MD5(input);
	return HexFormatBuffer<raw.size()>(&raw.front());
}
