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
#include "JackOutputPlugin.hxx"
#include "../OutputAPI.hxx"
#include "../Error.hxx"
#include "output/Features.h"
#include "thread/Mutex.hxx"
#include "util/ScopeExit.hxx"
#include "util/ConstBuffer.hxx"
#include "util/IterableSplitString.hxx"
#include "util/RuntimeError.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <atomic>
#include <cassert>

#include <jack/jack.h>
#include <jack/types.h>
#include <jack/ringbuffer.h>

#include <unistd.h> /* for usleep() */
#include <stdlib.h>

static constexpr unsigned MAX_PORTS = 16;

static constexpr size_t jack_sample_size = sizeof(jack_default_audio_sample_t);

class JackOutput final : public AudioOutput {
	/**
	 * libjack options passed to jack_client_open().
	 */
	jack_options_t options = JackNullOption;

	const char *name;

	const char *const server_name;

	/* configuration */

	std::string source_ports[MAX_PORTS];
	unsigned num_source_ports;

	std::string destination_ports[MAX_PORTS];
	unsigned num_destination_ports;
	/* overrides num_destination_ports*/
	bool auto_destination_ports;

	size_t ringbuffer_size;

	/* the current audio format */
	AudioFormat audio_format;

	/* jack library stuff */
	jack_port_t *ports[MAX_PORTS];
	jack_client_t *client;
	jack_ringbuffer_t *ringbuffer[MAX_PORTS];

	/**
	 * While this flag is set, the "process" callback generates
	 * silence.
	 */
	std::atomic_bool pause;

	/**
	 * Was Interrupt() called?  This will unblock Play().  It will
	 * be reset by Cancel() and Pause(), as documented by the
	 * #AudioOutput interface.
	 *
	 * Only initialized while the output is open.
	 */
	bool interrupted;

	/**
	 * Protects #error.
	 */
	mutable Mutex mutex;

	/**
	 * The error reported to the "on_info_shutdown" callback.
	 */
	std::exception_ptr error;

public:
	explicit JackOutput(const ConfigBlock &block);

private:
	/**
	 * Connect the JACK client and performs some basic setup
	 * (e.g. register callbacks).
	 *
	 * Throws on error.
	 */
	void Connect();

	/**
	 * Disconnect the JACK client.
	 */
	void Disconnect() noexcept;

	void Shutdown(const char *reason) noexcept {
		const std::lock_guard<Mutex> lock(mutex);
		error = std::make_exception_ptr(FormatRuntimeError("JACK connection shutdown: %s",
								   reason));
	}

	static void OnShutdown(jack_status_t, const char *reason,
			       void *arg) noexcept {
		auto &j = *static_cast<JackOutput *>(arg);
		j.Shutdown(reason);
	}


	/**
	 * Throws on error.
	 */
	void Start();
	void Stop() noexcept;

	/**
	 * Determine the number of frames guaranteed to be available
	 * on all channels.
	 */
	gcc_pure
	jack_nframes_t GetAvailable() const noexcept;

	void Process(jack_nframes_t nframes);
	static int Process(jack_nframes_t nframes, void *arg) noexcept {
		auto &j = *static_cast<JackOutput *>(arg);
		j.Process(nframes);
		return 0;
	}

	/**
	 * @return the number of frames that were written
	 */
	size_t WriteSamples(const float *src, size_t n_frames);

public:
	/* virtual methods from class AudioOutput */

	void Enable() override;
	void Disable() noexcept override;

	void Open(AudioFormat &new_audio_format) override;

	void Close() noexcept override {
		Stop();
	}

	void Interrupt() noexcept override;

	std::chrono::steady_clock::duration Delay() const noexcept override {
		return pause && !LockWasShutdown()
			? std::chrono::seconds(1)
			: std::chrono::steady_clock::duration::zero();
	}

	size_t Play(const void *chunk, size_t size) override;

	void Cancel() noexcept override;
	bool Pause() override;

private:
	bool LockWasShutdown() const noexcept {
		const std::lock_guard<Mutex> lock(mutex);
		return !!error;
	}
};

static constexpr Domain jack_output_domain("jack_output");

/**
 * Throws on error.
 */
static unsigned
parse_port_list(const char *source, std::string dest[])
{
	unsigned n = 0;
	for (auto i : IterableSplitString(source, ',')) {
		if (n >= MAX_PORTS)
			throw std::runtime_error("too many port names");

		dest[n++] = std::string(i.data, i.size);
	}

	if (n == 0)
		throw std::runtime_error("at least one port name expected");

	return n;
}

JackOutput::JackOutput(const ConfigBlock &block)
	:AudioOutput(FLAG_ENABLE_DISABLE|FLAG_PAUSE),
	 name(block.GetBlockValue("client_name", nullptr)),
	 server_name(block.GetBlockValue("server_name", nullptr))
{
	if (name != nullptr)
		options = jack_options_t(options | JackUseExactName);
	else
		/* if there's a no configured client name, we don't
		   care about the JackUseExactName option */
		name = "Music Player Daemon";

	if (server_name != nullptr)
		options = jack_options_t(options | JackServerName);

	if (!block.GetBlockValue("autostart", false))
		options = jack_options_t(options | JackNoStartServer);

	/* configure the source ports */

	const char *value = block.GetBlockValue("source_ports", "left,right");
	num_source_ports = parse_port_list(value, source_ports);

	/* configure the destination ports */

	value = block.GetBlockValue("destination_ports", nullptr);
	if (value == nullptr) {
		/* compatibility with MPD < 0.16 */
		value = block.GetBlockValue("ports", nullptr);
		if (value != nullptr)
			FormatWarning(jack_output_domain,
				      "deprecated option 'ports' in line %d",
				      block.line);
	}

	if (value != nullptr) {
		num_destination_ports =
			parse_port_list(value, destination_ports);
	} else {
		num_destination_ports = 0;
	}

	auto_destination_ports = block.GetBlockValue("auto_destination_ports", true);

	if (num_destination_ports > 0 &&
	    num_destination_ports != num_source_ports)
		FormatWarning(jack_output_domain,
			      "number of source ports (%u) mismatches the "
			      "number of destination ports (%u) in line %d",
			      num_source_ports, num_destination_ports,
			      block.line);

	ringbuffer_size = block.GetPositiveValue("ringbuffer_size", 32768U);
}

inline jack_nframes_t
JackOutput::GetAvailable() const noexcept
{
	size_t min = jack_ringbuffer_read_space(ringbuffer[0]);

	for (unsigned i = 1; i < audio_format.channels; ++i) {
		size_t current = jack_ringbuffer_read_space(ringbuffer[i]);
		if (current < min)
			min = current;
	}

	assert(min % jack_sample_size == 0);

	return min / jack_sample_size;
}

/**
 * Call jack_ringbuffer_read_advance() on all buffers in the list.
 */
static void
MultiReadAdvance(ConstBuffer<jack_ringbuffer_t *> buffers,
		 size_t size)
{
	for (auto *i : buffers)
		jack_ringbuffer_read_advance(i, size);
}

/**
 * Write a specific amount of "silence" to the given port.
 */
static void
WriteSilence(jack_port_t &port, jack_nframes_t nframes)
{
	auto *out =
		static_cast<jack_default_audio_sample_t *>(jack_port_get_buffer(&port, nframes));
	if (out == nullptr)
		/* workaround for libjack1 bug: if the server
		   connection fails, the process callback is invoked
		   anyway, but unable to get a buffer */
			return;

	std::fill_n(out, nframes, 0.0);
}

/**
 * Write a specific amount of "silence" to all ports in the list.
 */
static void
MultiWriteSilence(ConstBuffer<jack_port_t *> ports, jack_nframes_t nframes)
{
	for (auto *i : ports)
		WriteSilence(*i, nframes);
}

/**
 * Copy data from the buffer to the port.  If the buffer underruns,
 * fill with silence.
 */
static void
Copy(jack_port_t &dest, jack_nframes_t nframes,
     jack_ringbuffer_t &src, jack_nframes_t available)
{
	auto *out =
		static_cast<jack_default_audio_sample_t *>(jack_port_get_buffer(&dest, nframes));
	if (out == nullptr)
		/* workaround for libjack1 bug: if the server
		   connection fails, the process callback is
		   invoked anyway, but unable to get a
		   buffer */
		return;

	/* copy from buffer to port */
	jack_ringbuffer_read(&src, reinterpret_cast<char *>(out),
			     available * jack_sample_size);

	/* ringbuffer underrun, fill with silence */
	std::fill(out + available, out + nframes, 0.0);
}

inline void
JackOutput::Process(jack_nframes_t nframes)
{
	if (nframes <= 0)
		return;

	jack_nframes_t available = GetAvailable();

	const unsigned n_channels = audio_format.channels;

	if (pause) {
		/* empty the ring buffers */

		MultiReadAdvance({ringbuffer, n_channels},
				 available * jack_sample_size);

		/* generate silence while MPD is paused */

		MultiWriteSilence({ports, n_channels}, nframes);

		return;
	}

	if (available > nframes)
		available = nframes;

	for (unsigned i = 0; i < n_channels; ++i)
		Copy(*ports[i], nframes, *ringbuffer[i], available);

	/* generate silence for the unused source ports */

	MultiWriteSilence({ports + n_channels, num_source_ports - n_channels},
			  nframes);
}

static void
mpd_jack_error(const char *msg)
{
	LogError(jack_output_domain, msg);
}

#ifdef HAVE_JACK_SET_INFO_FUNCTION
static void
mpd_jack_info(const char *msg)
{
	LogNotice(jack_output_domain, msg);
}
#endif

void
JackOutput::Disconnect() noexcept
{
	assert(client != nullptr);

	jack_deactivate(client);
	jack_client_close(client);
	client = nullptr;
}

void
JackOutput::Connect()
{
	error = {};

	jack_status_t status;
	client = jack_client_open(name, options, &status, server_name);
	if (client == nullptr)
		throw FormatRuntimeError("Failed to connect to JACK server, status=%d",
					 status);

	jack_set_process_callback(client, Process, this);
	jack_on_info_shutdown(client, OnShutdown, this);

	for (unsigned i = 0; i < num_source_ports; ++i) {
		unsigned long portflags = JackPortIsOutput | JackPortIsTerminal;
		ports[i] = jack_port_register(client,
					      source_ports[i].c_str(),
					      JACK_DEFAULT_AUDIO_TYPE,
					      portflags, 0);
		if (ports[i] == nullptr) {
			Disconnect();
			throw FormatRuntimeError("Cannot register output port \"%s\"",
						 source_ports[i].c_str());
		}
	}
}

static bool
mpd_jack_test_default_device()
{
	return true;
}

inline void
JackOutput::Enable()
{
	for (unsigned i = 0; i < num_source_ports; ++i)
		ringbuffer[i] = nullptr;

	Connect();
}

inline void
JackOutput::Disable() noexcept
{
	if (client != nullptr)
		Disconnect();

	for (unsigned i = 0; i < num_source_ports; ++i) {
		if (ringbuffer[i] != nullptr) {
			jack_ringbuffer_free(ringbuffer[i]);
			ringbuffer[i] = nullptr;
		}
	}
}

static AudioOutput *
mpd_jack_init(EventLoop &, const ConfigBlock &block)
{
	jack_set_error_function(mpd_jack_error);

#ifdef HAVE_JACK_SET_INFO_FUNCTION
	jack_set_info_function(mpd_jack_info);
#endif

	return new JackOutput(block);
}

/**
 * Stops the playback on the JACK connection.
 */
void
JackOutput::Stop() noexcept
{
	if (client == nullptr)
		return;

	if (LockWasShutdown())
		/* the connection has failed; close it */
		Disconnect();
	else
		/* the connection is alive: just stop playback */
		jack_deactivate(client);
}

inline void
JackOutput::Start()
{
	assert(client != nullptr);
	assert(audio_format.channels <= num_source_ports);

	/* allocate the ring buffers on the first open(); these
	   persist until MPD exits.  It's too unsafe to delete them
	   because we can never know when mpd_jack_process() gets
	   called */
	for (unsigned i = 0; i < num_source_ports; ++i) {
		if (ringbuffer[i] == nullptr)
			ringbuffer[i] =
				jack_ringbuffer_create(ringbuffer_size);

		/* clear the ring buffer to be sure that data from
		   previous playbacks are gone */
		jack_ringbuffer_reset(ringbuffer[i]);
	}

	if ( jack_activate(client) ) {
		Stop();
		throw std::runtime_error("cannot activate client");
	}

	const char *dports[MAX_PORTS], **jports;
	unsigned num_dports;
	if (num_destination_ports == 0) {
		/* if user requests no auto connect, we are done */
		if (!auto_destination_ports) {
			return;
		}
		/* no output ports were configured - ask libjack for
		   defaults */
		jports = jack_get_ports(client, nullptr, nullptr,
					JackPortIsPhysical | JackPortIsInput);
		if (jports == nullptr) {
			Stop();
			throw std::runtime_error("no ports found");
		}

		assert(*jports != nullptr);

		for (num_dports = 0; num_dports < MAX_PORTS &&
			     jports[num_dports] != nullptr;
		     ++num_dports) {
			FormatDebug(jack_output_domain,
				    "destination_port[%u] = '%s'\n",
				    num_dports,
				    jports[num_dports]);
			dports[num_dports] = jports[num_dports];
		}
	} else {
		/* use the configured output ports */

		num_dports = num_destination_ports;
		for (unsigned i = 0; i < num_dports; ++i)
			dports[i] = destination_ports[i].c_str();

		jports = nullptr;
	}

	AtScopeExit(jports) {
		if (jports != nullptr)
			jack_free(jports);
	};

	assert(num_dports > 0);

	const char *duplicate_port = nullptr;
	if (audio_format.channels >= 2 && num_dports == 1) {
		/* mix stereo signal on one speaker */

		std::fill(dports + num_dports, dports + audio_format.channels,
			  dports[0]);
	} else if (num_dports > audio_format.channels) {
		if (audio_format.channels == 1 && num_dports >= 2) {
			/* mono input file: connect the one source
			   channel to the both destination channels */
			duplicate_port = dports[1];
			num_dports = 1;
		} else
			/* connect only as many ports as we need */
			num_dports = audio_format.channels;
	}

	assert(num_dports <= num_source_ports);

	for (unsigned i = 0; i < num_dports; ++i) {
		int ret = jack_connect(client, jack_port_name(ports[i]),
				       dports[i]);
		if (ret != 0) {
			Stop();
			throw FormatRuntimeError("Not a valid JACK port: %s",
						 dports[i]);
		}
	}

	if (duplicate_port != nullptr) {
		/* mono input file: connect the one source channel to
		   the both destination channels */
		int ret;

		ret = jack_connect(client, jack_port_name(ports[0]),
				   duplicate_port);
		if (ret != 0) {
			Stop();
			throw FormatRuntimeError("Not a valid JACK port: %s",
						 duplicate_port);
		}
	}
}

inline void
JackOutput::Open(AudioFormat &new_audio_format)
{
	pause = false;

	if (client != nullptr && LockWasShutdown())
		Disconnect();

	if (client == nullptr)
		Connect();

	new_audio_format.sample_rate = jack_get_sample_rate(client);

	if (num_source_ports == 1)
		new_audio_format.channels = 1;
	else if (new_audio_format.channels > num_source_ports)
		new_audio_format.channels = 2;

	/* JACK uses 32 bit float in the range [-1 .. 1] - just like
	   MPD's SampleFormat::FLOAT*/
	static_assert(jack_sample_size == sizeof(float), "Expected float32");
	new_audio_format.format = SampleFormat::FLOAT;
	audio_format = new_audio_format;

	interrupted = false;

	Start();
}

void
JackOutput::Interrupt() noexcept
{
	const std::unique_lock<Mutex> lock(mutex);

	/* the "interrupted" flag will prevent Play() from waiting,
	   and will instead throw AudioOutputInterrupted */
	interrupted = true;
}

inline size_t
JackOutput::WriteSamples(const float *src, size_t n_frames)
{
	assert(n_frames > 0);

	const unsigned n_channels = audio_format.channels;

	float *dest[MAX_CHANNELS];
	size_t space = SIZE_MAX;
	for (unsigned i = 0; i < n_channels; ++i) {
		jack_ringbuffer_data_t d[2];
		jack_ringbuffer_get_write_vector(ringbuffer[i], d);

		/* choose the first non-empty writable area */
		const jack_ringbuffer_data_t &e = d[d[0].len == 0];

		if (e.len < space)
			/* send data symmetrically */
			space = e.len;

		dest[i] = reinterpret_cast<float *>(e.buf);
	}

	space /= jack_sample_size;
	if (space == 0)
		return 0;

	const size_t result = n_frames = std::min(space, n_frames);

	while (n_frames-- > 0)
		for (unsigned i = 0; i < n_channels; ++i)
			*dest[i]++ = *src++;

	const size_t per_channel_advance = result * jack_sample_size;
	for (unsigned i = 0; i < n_channels; ++i)
		jack_ringbuffer_write_advance(ringbuffer[i],
					      per_channel_advance);

	return result;
}

inline size_t
JackOutput::Play(const void *chunk, size_t size)
{
	pause = false;

	const size_t frame_size = audio_format.GetFrameSize();
	assert(size % frame_size == 0);
	size /= frame_size;

	while (true) {
		{
			const std::lock_guard<Mutex> lock(mutex);
			if (error)
				std::rethrow_exception(error);

			if (interrupted)
				throw AudioOutputInterrupted{};
		}

		size_t frames_written =
			WriteSamples(static_cast<const float *>(chunk), size);
		if (frames_written > 0)
			return frames_written * frame_size;

		/* XXX do something more intelligent to
		   synchronize */
		usleep(1000);
	}
}

void
JackOutput::Cancel() noexcept
{
	const std::unique_lock<Mutex> lock(mutex);
	interrupted = false;
}

inline bool
JackOutput::Pause()
{
	{
		const std::lock_guard<Mutex> lock(mutex);
		interrupted = false;
		if (error)
			std::rethrow_exception(error);
	}

	pause = true;

	return true;
}

const struct AudioOutputPlugin jack_output_plugin = {
	"jack",
	mpd_jack_test_default_device,
	mpd_jack_init,
	nullptr,
};
