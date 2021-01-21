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

#include "Client.hxx"
#include "protocol/Ack.hxx"
#include "fs/Path.hxx"
#include "fs/FileInfo.hxx"

void
Client::AllowFile(Path path_fs) const
{
#ifdef _WIN32
	(void)path_fs;

	throw ProtocolError(ACK_ERROR_PERMISSION, "Access denied");
#else
	if (uid >= 0 && uid_t(uid) == geteuid())
		/* always allow access if user runs his own MPD
		   instance */
		return;

	if (uid < 0)
		/* unauthenticated client */
		throw ProtocolError(ACK_ERROR_PERMISSION, "Access denied");

	const FileInfo fi(path_fs);

	if (fi.GetUid() != uid_t(uid) && (fi.GetMode() & 0444) != 0444)
		/* client is not owner */
		throw ProtocolError(ACK_ERROR_PERMISSION, "Access denied");
#endif
}
