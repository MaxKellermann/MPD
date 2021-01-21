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

#include "NullEncoderPlugin.hxx"
#include "../EncoderAPI.hxx"
#include "util/DynamicFifoBuffer.hxx"
#include "util/Compiler.h"

class NullEncoder final : public Encoder {
	DynamicFifoBuffer<uint8_t> buffer;

public:
	NullEncoder()
		:Encoder(false),
		 buffer(8192) {}

	/* virtual methods from class Encoder */
	void Write(const void *data, size_t length) override {
		buffer.Append(static_cast<const uint8_t *>(data), length);
	}

	size_t Read(void *dest, size_t length) override {
		return buffer.Read(static_cast<uint8_t *>(dest), length);
	}
};

class PreparedNullEncoder final : public PreparedEncoder {
public:
	/* virtual methods from class PreparedEncoder */
	Encoder *Open(AudioFormat &) override {
		return new NullEncoder();
	}
};

static PreparedEncoder *
null_encoder_init([[maybe_unused]] const ConfigBlock &block)
{
	return new PreparedNullEncoder();
}

const EncoderPlugin null_encoder_plugin = {
	"null",
	null_encoder_init,
};
