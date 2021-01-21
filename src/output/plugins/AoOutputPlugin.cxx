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

#include "AoOutputPlugin.hxx"
#include "../OutputAPI.hxx"
#include "thread/SafeSingleton.hxx"
#include "system/Error.hxx"
#include "util/DivideString.hxx"
#include "util/IterableSplitString.hxx"
#include "util/RuntimeError.hxx"
#include "util/Domain.hxx"
#include "util/StringAPI.hxx"
#include "Log.hxx"

#include <ao/ao.h>

/* An ao_sample_format, with all fields set to zero: */
static ao_sample_format OUR_AO_FORMAT_INITIALIZER;

class AoInit {
public:
	AoInit() {
		ao_initialize();
	}

	~AoInit() noexcept {
		ao_shutdown();
	}
};

class AoOutput final : AudioOutput, SafeSingleton<AoInit> {
	const size_t write_size;
	int driver;
	ao_option *options = nullptr;
	ao_device *device;

	size_t frame_size;

	explicit AoOutput(const ConfigBlock &block);
	~AoOutput() override;

public:
	static AudioOutput *Create(EventLoop &, const ConfigBlock &block) {
		return new AoOutput(block);
	}

	void Open(AudioFormat &audio_format) override;
	void Close() noexcept override;

	size_t Play(const void *chunk, size_t size) override;
};

static constexpr Domain ao_output_domain("ao_output");


static std::system_error
MakeAoError()
{
	const char *error = "Unknown libao failure";

	switch (errno) {
	case AO_ENODRIVER:
		error = "No such libao driver";
		break;

	case AO_ENOTLIVE:
		error = "This driver is not a libao live device";
		break;

	case AO_EBADOPTION:
		error = "Invalid libao option";
		break;

	case AO_EOPENDEVICE:
		error = "Cannot open the libao device";
		break;

	case AO_EFAIL:
		error = "Generic libao failure";
		break;
	}

	return MakeErrno(errno, error);
}

AoOutput::AoOutput(const ConfigBlock &block)
	:AudioOutput(0),
	 write_size(block.GetPositiveValue("write_size", 1024U))
{
	const char *value = block.GetBlockValue("driver", "default");
	if (StringIsEqual(value, "default"))
		driver = ao_default_driver_id();
	else
		driver = ao_driver_id(value);

	if (driver < 0)
		throw FormatRuntimeError("\"%s\" is not a valid ao driver",
					 value);

	ao_info *ai = ao_driver_info(driver);
	if (ai == nullptr)
		throw std::runtime_error("problems getting driver info");

	FormatDebug(ao_output_domain, "using ao driver \"%s\" for \"%s\"\n",
		    ai->short_name, block.GetBlockValue("name", nullptr));

	value = block.GetBlockValue("options", nullptr);
	if (value != nullptr) {
		for (StringView i : IterableSplitString(value, ';')) {
			i.Strip();

			auto s = i.Split('=');
			if (s.first.empty() || s.second.IsNull())
				throw FormatRuntimeError("problems parsing option \"%.*s\"",
							 int(i.size), i.data);

			const std::string n(s.first), v(s.second);
			ao_append_option(&options, n.c_str(), v.c_str());
		}
	}
}

AoOutput::~AoOutput()
{
	ao_free_options(options);
}

void
AoOutput::Open(AudioFormat &audio_format)
{
	ao_sample_format format = OUR_AO_FORMAT_INITIALIZER;

	switch (audio_format.format) {
	case SampleFormat::S8:
		format.bits = 8;
		break;

	case SampleFormat::S16:
		format.bits = 16;
		break;

	default:
		/* support for 24 bit samples in libao is currently
		   dubious, and until we have sorted that out,
		   convert everything to 16 bit */
		audio_format.format = SampleFormat::S16;
		format.bits = 16;
		break;
	}

	frame_size = audio_format.GetFrameSize();

	format.rate = audio_format.sample_rate;
	format.byte_format = AO_FMT_NATIVE;
	format.channels = audio_format.channels;

	device = ao_open_live(driver, &format, options);
	if (device == nullptr)
		throw MakeAoError();
}

void
AoOutput::Close() noexcept
{
	ao_close(device);
}

size_t
AoOutput::Play(const void *chunk, size_t size)
{
	assert(size % frame_size == 0);

	if (size > write_size) {
		/* round down to a multiple of the frame size */
		size = (write_size / frame_size) * frame_size;

		if (size < frame_size)
			/* no matter how small "write_size" was
			   configured, we must pass at least one frame
			   to libao */
			size = frame_size;
	}

	/* For whatever reason, libao wants a non-const pointer.
	   Let's hope it does not write to the buffer, and use the
	   union deconst hack to * work around this API misdesign. */
	char *data = const_cast<char *>(static_cast<const char *>(chunk));

	if (ao_play(device, data, size) == 0)
		throw MakeAoError();

	return size;
}

const struct AudioOutputPlugin ao_output_plugin = {
	"ao",
	nullptr,
	&AoOutput::Create,
	nullptr,
};
