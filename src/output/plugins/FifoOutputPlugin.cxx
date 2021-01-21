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

#include "FifoOutputPlugin.hxx"
#include "../OutputAPI.hxx"
#include "../Timer.hxx"
#include "fs/AllocatedPath.hxx"
#include "fs/FileSystem.hxx"
#include "fs/FileInfo.hxx"
#include "util/Domain.hxx"
#include "util/RuntimeError.hxx"
#include "Log.hxx"
#include "open.h"

#include <cerrno>

#include <sys/stat.h>
#include <unistd.h>

class FifoOutput final : AudioOutput {
	const AllocatedPath path;
	std::string path_utf8;

	int input = -1;
	int output = -1;
	bool created = false;
	Timer *timer;

public:
	explicit FifoOutput(const ConfigBlock &block);

	~FifoOutput() override {
		CloseFifo();
	}

	static AudioOutput *Create(EventLoop &,
				   const ConfigBlock &block) {
		return new FifoOutput(block);
	}

private:
	void Create();
	void Check();
	void Delete();

	void OpenFifo();
	void CloseFifo();

	void Open(AudioFormat &audio_format) override;
	void Close() noexcept override;

	[[nodiscard]] std::chrono::steady_clock::duration Delay() const noexcept override;
	size_t Play(const void *chunk, size_t size) override;
	void Cancel() noexcept override;
};

static constexpr Domain fifo_output_domain("fifo_output");

FifoOutput::FifoOutput(const ConfigBlock &block)
	:AudioOutput(0),
	 path(block.GetPath("path"))
{
	if (path.IsNull())
		throw std::runtime_error("No \"path\" parameter specified");

	path_utf8 = path.ToUTF8();

	OpenFifo();
}

inline void
FifoOutput::Delete()
{
	FormatDebug(fifo_output_domain,
		    "Removing FIFO \"%s\"", path_utf8.c_str());

	try {
		RemoveFile(path);
	} catch (...) {
		LogError(std::current_exception(), "Could not remove FIFO");
		return;
	}

	created = false;
}

void
FifoOutput::CloseFifo()
{
	if (input >= 0) {
		close(input);
		input = -1;
	}

	if (output >= 0) {
		close(output);
		output = -1;
	}

	FileInfo fi;
	if (created && GetFileInfo(path, fi))
		Delete();
}

inline void
FifoOutput::Create()
{
	if (!MakeFifo(path, 0666))
		throw FormatErrno("Couldn't create FIFO \"%s\"",
				  path_utf8.c_str());

	created = true;
}

inline void
FifoOutput::Check()
{
	struct stat st;
	if (!StatFile(path, st)) {
		if (errno == ENOENT) {
			/* Path doesn't exist */
			Create();
			return;
		}

		throw FormatErrno("Failed to stat FIFO \"%s\"",
				  path_utf8.c_str());
	}

	if (!S_ISFIFO(st.st_mode))
		throw FormatRuntimeError("\"%s\" already exists, but is not a FIFO",
					 path_utf8.c_str());
}

inline void
FifoOutput::OpenFifo()
try {
	Check();

	input = OpenFile(path, O_RDONLY|O_NONBLOCK|O_BINARY, 0).Steal();
	if (input < 0)
		throw FormatErrno("Could not open FIFO \"%s\" for reading",
				  path_utf8.c_str());

	output = OpenFile(path, O_WRONLY|O_NONBLOCK|O_BINARY, 0).Steal();
	if (output < 0)
		throw FormatErrno("Could not open FIFO \"%s\" for writing",
				  path_utf8.c_str());
} catch (...) {
	CloseFifo();
	throw;
}

void
FifoOutput::Open(AudioFormat &audio_format)
{
	timer = new Timer(audio_format);
}

void
FifoOutput::Close() noexcept
{
	delete timer;
}

void
FifoOutput::Cancel() noexcept
{
	timer->Reset();

	ssize_t bytes;
	do {
		char buffer[16384];
		bytes = read(input, buffer, sizeof(buffer));
	} while (bytes > 0 && errno != EINTR);

	if (bytes < 0 && errno != EAGAIN) {
		FormatErrno(fifo_output_domain,
			    "Flush of FIFO \"%s\" failed",
			    path_utf8.c_str());
	}
}

std::chrono::steady_clock::duration
FifoOutput::Delay() const noexcept
{
	return timer->IsStarted()
		? timer->GetDelay()
		: std::chrono::steady_clock::duration::zero();
}

size_t
FifoOutput::Play(const void *chunk, size_t size)
{
	if (!timer->IsStarted())
		timer->Start();
	timer->Add(size);

	while (true) {
		ssize_t bytes = write(output, chunk, size);
		if (bytes > 0)
			return size_t(bytes);

		if (bytes < 0) {
			switch (errno) {
			case EAGAIN:
				/* The pipe is full, so empty it */
				Cancel();
				continue;
			case EINTR:
				continue;
			}

			throw FormatErrno("Failed to write to FIFO %s",
					  path_utf8.c_str());
		}
	}
}

const struct AudioOutputPlugin fifo_output_plugin = {
	"fifo",
	nullptr,
	&FifoOutput::Create,
	nullptr,
};
