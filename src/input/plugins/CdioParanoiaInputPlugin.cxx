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

/**
 * CD-Audio handling (requires libcdio_paranoia)
 */

#include "CdioParanoiaInputPlugin.hxx"
#include "lib/cdio/Paranoia.hxx"
#include "../InputStream.hxx"
#include "../InputPlugin.hxx"
#include "util/TruncateString.hxx"
#include "util/StringCompare.hxx"
#include "util/RuntimeError.hxx"
#include "util/Domain.hxx"
#include "util/ByteOrder.hxx"
#include "fs/AllocatedPath.hxx"
#include "Log.hxx"
#include "config/Block.hxx"

#include <cassert>
#include <cstdint>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <cdio/cd_types.h>

class CdioParanoiaInputStream final : public InputStream {
	cdrom_drive_t *const drv;
	CdIo_t *const cdio;
	CdromParanoia para;

	const lsn_t lsn_from, lsn_to;
	int lsn_relofs;

	char buffer[CDIO_CD_FRAMESIZE_RAW];
	int buffer_lsn;

 public:
	CdioParanoiaInputStream(const char *_uri, Mutex &_mutex,
				cdrom_drive_t *_drv, CdIo_t *_cdio,
				bool reverse_endian,
				lsn_t _lsn_from, lsn_t _lsn_to)
		:InputStream(_uri, _mutex),
		 drv(_drv), cdio(_cdio), para(drv),
		 lsn_from(_lsn_from), lsn_to(_lsn_to),
		 lsn_relofs(0),
		 buffer_lsn(-1)
	{
		/* Set reading mode for full paranoia, but allow
		   skipping sectors. */
		para.SetMode(PARANOIA_MODE_FULL^PARANOIA_MODE_NEVERSKIP);

		/* seek to beginning of the track */
		para.Seek(lsn_from);

		seekable = true;
		size = (lsn_to - lsn_from + 1) * CDIO_CD_FRAMESIZE_RAW;

		/* hack to make MPD select the "pcm" decoder plugin */
		SetMimeType(reverse_endian
			    ? "audio/x-mpd-cdda-pcm-reverse"
			    : "audio/x-mpd-cdda-pcm");
		SetReady();
	}

	~CdioParanoiaInputStream() override {
		para = {};
		cdio_cddap_close_no_free_cdio(drv);
		cdio_destroy(cdio);
	}

	/* virtual methods from InputStream */
	[[nodiscard]] bool IsEOF() const noexcept override;
	size_t Read(std::unique_lock<Mutex> &lock,
		    void *ptr, size_t size) override;
	void Seek(std::unique_lock<Mutex> &lock, offset_type offset) override;
};

static constexpr Domain cdio_domain("cdio");

static bool default_reverse_endian;
static unsigned speed = 0;

static void
input_cdio_init(EventLoop &, const ConfigBlock &block)
{
	const char *value = block.GetBlockValue("default_byte_order");
	if (value != nullptr) {
		if (strcmp(value, "little_endian") == 0)
			default_reverse_endian = IsBigEndian();
		else if (strcmp(value, "big_endian") == 0)
			default_reverse_endian = IsLittleEndian();
		else
			throw FormatRuntimeError("Unrecognized 'default_byte_order' setting: %s",
						 value);
	}
	speed = block.GetBlockValue("speed",0U);
}

struct CdioUri {
	char device[64];
	int track;
};

static CdioUri
parse_cdio_uri(const char *src)
{
	CdioUri dest;

	if (*src == 0) {
		/* play the whole CD in the default drive */
		dest.device[0] = 0;
		dest.track = -1;
		return dest;
	}

	const char *slash = std::strrchr(src, '/');
	if (slash == nullptr) {
		/* play the whole CD in the specified drive */
		CopyTruncateString(dest.device, src, sizeof(dest.device));
		dest.track = -1;
		return dest;
	}

	size_t device_length = slash - src;
	if (device_length >= sizeof(dest.device))
		device_length = sizeof(dest.device) - 1;

	memcpy(dest.device, src, device_length);
	dest.device[device_length] = 0;

	const char *track = slash + 1;

	char *endptr;
	dest.track = strtoul(track, &endptr, 10);
	if (*endptr != 0)
		throw std::runtime_error("Malformed track number");

	if (endptr == track)
		/* play the whole CD */
		dest.track = -1;

	return dest;
}

static AllocatedPath
cdio_detect_device()
{
	char **devices = cdio_get_devices_with_cap(nullptr, CDIO_FS_AUDIO,
						   false);
	if (devices == nullptr)
		return nullptr;

	AllocatedPath path = AllocatedPath::FromFS(devices[0]);
	cdio_free_device_list(devices);
	return path;
}

static InputStreamPtr
input_cdio_open(const char *uri,
		Mutex &mutex)
{
	uri = StringAfterPrefixIgnoreCase(uri, "cdda://");
	assert(uri != nullptr);

	const auto parsed_uri = parse_cdio_uri(uri);

	/* get list of CD's supporting CD-DA */
	const AllocatedPath device = parsed_uri.device[0] != 0
		? AllocatedPath::FromFS(parsed_uri.device)
		: cdio_detect_device();
	if (device.IsNull())
		throw std::runtime_error("Unable find or access a CD-ROM drive with an audio CD in it.");

	/* Found such a CD-ROM with a CD-DA loaded. Use the first drive in the list. */
	const auto cdio = cdio_open(device.c_str(), DRIVER_UNKNOWN);
	if (cdio == nullptr)
		throw std::runtime_error("Failed to open CD drive");

	const auto drv = cdio_cddap_identify_cdio(cdio, 1, nullptr);
	if (drv == nullptr) {
		cdio_destroy(cdio);
		throw std::runtime_error("Unable to identify audio CD disc.");
	}

	cdio_cddap_verbose_set(drv, CDDA_MESSAGE_FORGETIT, CDDA_MESSAGE_FORGETIT);
	if (speed > 0) {
		FormatDebug(cdio_domain,"Attempting to set CD speed to %dx",speed);
		cdio_cddap_speed_set(drv,speed);
	}

	if (0 != cdio_cddap_open(drv)) {
		cdio_cddap_close_no_free_cdio(drv);
		cdio_destroy(cdio);
		throw std::runtime_error("Unable to open disc.");
	}

	bool reverse_endian;
	const int be = data_bigendianp(drv);
	switch (be) {
	case -1:
		LogDebug(cdio_domain, "drive returns unknown audio data");
		reverse_endian = default_reverse_endian;
		break;

	case 0:
		LogDebug(cdio_domain, "drive returns audio data Little Endian");
		reverse_endian = IsBigEndian();
		break;

	case 1:
		LogDebug(cdio_domain, "drive returns audio data Big Endian");
		reverse_endian = IsLittleEndian();
		break;

	default:
		cdio_cddap_close_no_free_cdio(drv);
		cdio_destroy(cdio);
		throw FormatRuntimeError("Drive returns unknown data type %d",
					 be);
	}

	lsn_t lsn_from, lsn_to;
	if (parsed_uri.track >= 0) {
		lsn_from = cdio_get_track_lsn(cdio, parsed_uri.track);
		lsn_to = cdio_get_track_last_lsn(cdio, parsed_uri.track);
	} else {
		lsn_from = 0;
		lsn_to = cdio_get_disc_last_lsn(cdio);
	}

	return std::make_unique<CdioParanoiaInputStream>(uri, mutex,
							 drv, cdio,
							 reverse_endian,
							 lsn_from, lsn_to);
}

void
CdioParanoiaInputStream::Seek(std::unique_lock<Mutex> &,
			      offset_type new_offset)
{
	if (new_offset > size)
		throw FormatRuntimeError("Invalid offset to seek %ld (%ld)",
					 long(new_offset), long(size));

	/* simple case */
	if (new_offset == offset)
		return;

	/* calculate current LSN */
	lsn_relofs = new_offset / CDIO_CD_FRAMESIZE_RAW;
	offset = new_offset;

	{
		const ScopeUnlock unlock(mutex);
		para.Seek(lsn_from + lsn_relofs);
	}
}

size_t
CdioParanoiaInputStream::Read(std::unique_lock<Mutex> &,
			      void *ptr, size_t length)
{
	size_t nbytes = 0;
	char *wptr = static_cast<char *>(ptr);

	while (length > 0) {
		/* end of track ? */
		if (lsn_from + lsn_relofs > lsn_to)
			break;

		//current sector was changed ?
		const int16_t *rbuf;
		if (lsn_relofs != buffer_lsn) {
			const ScopeUnlock unlock(mutex);

			try {
				rbuf = para.Read().data;
			} catch (...) {
				char *s_err = cdio_cddap_errors(drv);
				if (s_err) {
					FormatError(cdio_domain,
						    "paranoia_read: %s", s_err);
					cdio_cddap_free_messages(s_err);
				}

				throw;
			}

			//store current buffer
			memcpy(buffer, rbuf, CDIO_CD_FRAMESIZE_RAW);
			buffer_lsn = lsn_relofs;
		} else {
			//use cached sector
			rbuf = reinterpret_cast<const int16_t *>(buffer);
		}

		//correct offset
		const int diff = offset - lsn_relofs * CDIO_CD_FRAMESIZE_RAW;

		assert(diff >= 0 && diff < CDIO_CD_FRAMESIZE_RAW);

		const size_t maxwrite = CDIO_CD_FRAMESIZE_RAW - diff;  //# of bytes pending in current buffer
		const size_t len = (length < maxwrite? length : maxwrite);

		//skip diff bytes from this lsn
		memcpy(wptr, (reinterpret_cast<const char *>(rbuf)) + diff, len);
		//update pointer
		wptr += len;
		nbytes += len;

		//update offset
		offset += len;
		lsn_relofs = offset / CDIO_CD_FRAMESIZE_RAW;
		//update length
		length -= len;
	}

	return nbytes;
}

bool
CdioParanoiaInputStream::IsEOF() const noexcept
{
	return lsn_from + lsn_relofs > lsn_to;
}

static constexpr const char *cdio_paranoia_prefixes[] = {
	"cdda://",
	nullptr
};

const InputPlugin input_plugin_cdio_paranoia = {
	"cdio_paranoia",
	cdio_paranoia_prefixes,
	input_cdio_init,
	nullptr,
	input_cdio_open,
	nullptr
};
