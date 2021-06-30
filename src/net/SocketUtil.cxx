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

#include "config.h"
#include "SocketUtil.hxx"
#include "SocketAddress.hxx"
#include "SocketError.hxx"
#include "UniqueSocketDescriptor.hxx"

#include <sys/stat.h>

UniqueSocketDescriptor
socket_bind_listen(int domain, int type, int protocol,
		   SocketAddress address,
		   int backlog)
{
	UniqueSocketDescriptor fd;
	if (!fd.CreateNonBlock(domain, type, protocol))
		throw MakeSocketError("Failed to create socket");

#ifdef HAVE_UN
	if (domain == AF_LOCAL) {
		/* Prevent access until right permissions are set */
		fchmod(fd.Get(), 0);
	}
#endif

	if (domain == AF_INET6 && !fd.SetV6Only(true))
		throw MakeSocketError("setsockopt() failed for V6ONLY");

	if (!fd.SetReuseAddress())
		throw MakeSocketError("setsockopt() failed for REUSEADDR");

	if (!fd.Bind(address))
		throw MakeSocketError("Failed to bind socket");

	if (!fd.Listen(backlog))
		throw MakeSocketError("Failed to listen on socket");

#if defined(HAVE_STRUCT_UCRED) && defined(SO_PASSCRED)
	fd.SetBoolOption(SOL_SOCKET, SO_PASSCRED, true);
#endif

	return fd;
}
