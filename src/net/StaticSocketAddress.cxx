/*
 * Copyright 2012-2019 Max Kellermann <max.kellermann@gmail.com>
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

#include "StaticSocketAddress.hxx"
#include "IPv4Address.hxx"
#include "IPv6Address.hxx"
#include "util/StringView.hxx"

#include <algorithm>

#include <string.h>

StaticSocketAddress &
StaticSocketAddress::operator=(SocketAddress other) noexcept
{
	size = std::min(other.GetSize(), GetCapacity());
	memcpy(&address, other.GetAddress(), size);
	return *this;
}

#ifdef HAVE_UN

StringView
StaticSocketAddress::GetLocalRaw() const noexcept
{
	return SocketAddress(*this).GetLocalRaw();
}

#endif

#ifdef HAVE_TCP

bool
StaticSocketAddress::SetPort(unsigned port) noexcept
{
	switch (GetFamily()) {
	case AF_INET:
		{
			auto &a = *static_cast<IPv4Address *>((void *)&address);
			a.SetPort(port);
			return true;
		}

	case AF_INET6:
		{
			auto &a = *static_cast<IPv6Address *>((void *)&address);
			a.SetPort(port);
			return true;
		}
	}

	return false;
}

#endif
