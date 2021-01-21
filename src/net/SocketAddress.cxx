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

#include "SocketAddress.hxx"
#include "IPv4Address.hxx"
#include "IPv6Address.hxx"
#include "util/ConstBuffer.hxx"
#include "util/StringView.hxx"

#include <cassert>
#include <cstring>

#ifdef HAVE_UN
#include <sys/un.h>
#endif

#ifdef HAVE_TCP
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif
#endif

bool
SocketAddress::operator==(SocketAddress other) const noexcept
{
	return size == other.size && memcmp(address, other.address, size) == 0;
}

#ifdef HAVE_UN

StringView
SocketAddress::GetLocalRaw() const noexcept
{
	if (IsNull() || GetFamily() != AF_LOCAL)
		/* not applicable */
		return nullptr;

	const auto *sun = &CastTo<struct sockaddr_un>();
	const auto start = reinterpret_cast<const char *>(sun);
	const auto path = sun->sun_path;
	const size_t header_size = path - start;
	if (size < size_type(header_size))
		/* malformed address */
		return nullptr;

	return {path, size - header_size};
}

const char *
SocketAddress::GetLocalPath() const noexcept
{
	const auto raw = GetLocalRaw();
	return !raw.empty() &&
		/* must be an absolute path */
		raw.front() == '/' &&
		/* must be null-terminated */
		raw.back() == 0 &&
		/* there must not be any other null byte */
		std::memchr(raw.data, 0, raw.size - 1) == nullptr
		? raw.data
		: nullptr;
}

#endif

#ifdef HAVE_TCP

bool
SocketAddress::IsV6Any() const noexcept
{
	return GetFamily() == AF_INET6 && IPv6Address::Cast(*this).IsAny();
}

bool
SocketAddress::IsV4Mapped() const noexcept
{
	return GetFamily() == AF_INET6 && IPv6Address::Cast(*this).IsV4Mapped();
}

IPv4Address
SocketAddress::UnmapV4() const noexcept
{
	assert(IsV4Mapped());

	return IPv6Address::Cast(*this).UnmapV4();
}

unsigned
SocketAddress::GetPort() const noexcept
{
	if (IsNull())
		return 0;

	switch (GetFamily()) {
	case AF_INET:
		return IPv4Address::Cast(*this).GetPort();

	case AF_INET6:
		return IPv6Address::Cast(*this).GetPort();

	default:
		return 0;
	}
}

static constexpr ConstBuffer<void>
GetSteadyPart(const struct sockaddr_in &address) noexcept
{
	return {&address.sin_addr, sizeof(address.sin_addr)};
}

static constexpr ConstBuffer<void>
GetSteadyPart(const struct sockaddr_in6 &address) noexcept
{
	return {&address.sin6_addr, sizeof(address.sin6_addr)};
}

#endif

ConstBuffer<void>
SocketAddress::GetSteadyPart() const noexcept
{
	if (IsNull())
		return nullptr;

	switch (GetFamily()) {
#ifdef HAVE_UN
	case AF_LOCAL:
		return GetLocalRaw().ToVoid();
#endif

#ifdef HAVE_TCP
	case AF_INET:
		return ::GetSteadyPart(CastTo<struct sockaddr_in>());

	case AF_INET6:
		return ::GetSteadyPart(CastTo<struct sockaddr_in6>());
#endif

	default:
		return nullptr;
	}
}
