/*
 * Copyright 2012-2020 Max Kellermann <max.kellermann@gmail.com>
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

#include "IPv6Address.hxx"
#include "IPv4Address.hxx"

#include <cassert>

#include <string.h>

IPv6Address::IPv6Address(SocketAddress src) noexcept
	:address(src.CastTo<struct sockaddr_in6>())
{
	assert(!src.IsNull());
	assert(src.GetFamily() == AF_INET6);
}

bool
IPv6Address::IsAny() const noexcept
{
	assert(IsValid());

	return memcmp(&address.sin6_addr,
		      &in6addr_any, sizeof(in6addr_any)) == 0;
}

IPv4Address
IPv6Address::UnmapV4() const noexcept
{
	assert(IsV4Mapped());

	struct sockaddr_in buffer{};
	buffer.sin_family = AF_INET;
	memcpy(&buffer.sin_addr, (reinterpret_cast<const char *>(&address.sin6_addr)) + 12,
	       sizeof(buffer.sin_addr));
	buffer.sin_port = address.sin6_port;

	return buffer;
}

template<typename T>
static void
BitwiseAndT(T *dest, const T *a, const T *b, size_t n)
{
	while (n-- > 0)
		*dest++ = *a++ & *b++;
}

static void
BitwiseAnd32(void *dest, const void *a, const void *b, size_t n)
{
	using value_type = uint32_t;
	using pointer = value_type *;
	using const_pointer = const value_type *;

	BitwiseAndT(pointer(dest), const_pointer(a), const_pointer(b),
		    n / sizeof(value_type));
}

IPv6Address
IPv6Address::operator&(const IPv6Address &other) const
{
	IPv6Address result;
	BitwiseAnd32(&result, this, &other,
		     sizeof(result));
	return result;
}
