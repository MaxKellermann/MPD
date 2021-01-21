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

#include "ApeLoader.hxx"
#include "util/ByteOrder.hxx"
#include "input/InputStream.hxx"
#include "util/StringView.hxx"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

struct ApeFooter {
	unsigned char id[8];
	uint32_t version;
	uint32_t length;
	uint32_t count;
	unsigned char flags[4];
	unsigned char reserved[8];
};

bool
tag_ape_scan(InputStream &is, const ApeTagCallback& callback)
try {
	std::unique_lock<Mutex> lock(is.mutex);

	if (!is.KnownSize() || !is.CheapSeeking())
		return false;

	/* determine if file has an apeV2 tag */
	ApeFooter footer;
	is.Seek(lock, is.GetSize() - sizeof(footer));
	is.ReadFull(lock, &footer, sizeof(footer));

	if (memcmp(footer.id, "APETAGEX", sizeof(footer.id)) != 0 ||
	    FromLE32(footer.version) != 2000)
		return false;

	/* find beginning of ape tag */
	size_t remaining = FromLE32(footer.length);
	if (remaining <= sizeof(footer) + 10 ||
	    /* refuse to load more than one megabyte of tag data */
	    remaining > 1024 * 1024)
		return false;

	is.Seek(lock, is.GetSize() - remaining);

	/* read tag into buffer */
	remaining -= sizeof(footer);
	assert(remaining > 10);

	std::unique_ptr<char[]> buffer(new char[remaining]);
	is.ReadFull(lock, buffer.get(), remaining);

	/* read tags */
	unsigned n = FromLE32(footer.count);
	const char *p = buffer.get();
	while (n-- && remaining > 10) {
		size_t size = FromLE32(*reinterpret_cast<const uint32_t *>(p));
		p += 4;
		remaining -= 4;
		unsigned long flags = FromLE32(*reinterpret_cast<const uint32_t *>(p));
		p += 4;
		remaining -= 4;

		/* get the key */
		const char *key = p;
		const char *key_end = static_cast<const char *>(std::memchr(p, '\0', remaining));
		if (key_end == nullptr)
			break;

		p = key_end + 1;
		remaining -= p - key;

		/* get the value */
		if (remaining < size)
			break;

		if (!callback(flags, key, {p, size}))
			break;

		p += size;
		remaining -= size;
	}

	return true;
} catch (...) {
	return false;
}
