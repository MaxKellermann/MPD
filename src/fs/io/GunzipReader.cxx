/*
 * Copyright 2014-2019 Max Kellermann <max.kellermann@gmail.com>
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

#include "GunzipReader.hxx"
#include "lib/zlib/Error.hxx"

GunzipReader::GunzipReader(Reader &_next)
	:next(_next)
{
	z.next_in = nullptr;
	z.avail_in = 0;
	z.zalloc = Z_NULL;
	z.zfree = Z_NULL;
	z.opaque = Z_NULL;

	int result = inflateInit2(&z, 16 + MAX_WBITS);
	if (result != Z_OK)
		throw ZlibError(result);
}

inline bool
GunzipReader::FillBuffer()
{
	auto w = buffer.Write();
	assert(!w.empty());

	size_t nbytes = next.Read(w.data, w.size);
	if (nbytes == 0)
		return false;

	buffer.Append(nbytes);
	return true;
}

size_t
GunzipReader::Read(void *data, size_t size)
{
	if (eof)
		return 0;

	z.next_out = static_cast<Bytef *>(data);
	z.avail_out = size;

	while (true) {
		int flush = Z_NO_FLUSH;

		auto r = buffer.Read();
		if (r.empty()) {
			if (FillBuffer())
				r = buffer.Read();
			else
				flush = Z_FINISH;
		}

		z.next_in = r.data;
		z.avail_in = r.size;

		int result = inflate(&z, flush);
		if (result == Z_STREAM_END) {
			eof = true;
			return size - z.avail_out;
		} else if (result != Z_OK)
			throw ZlibError(result);

		buffer.Consume(r.size - z.avail_in);

		if (z.avail_out < size)
			return size - z.avail_out;
	}
}
