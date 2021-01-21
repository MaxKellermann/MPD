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

#include "PulseOutputPlugin.hxx"
#include "lib/pulse/Error.hxx"
#include "lib/pulse/LogError.hxx"
#include "lib/pulse/LockGuard.hxx"
#include "../OutputAPI.hxx"
#include "../Error.hxx"
#include "mixer/MixerList.hxx"
#include "mixer/plugins/PulseMixerPlugin.hxx"
#include "util/ScopeExit.hxx"

#include <pulse/thread-mainloop.h>
#include <pulse/context.h>
#include <pulse/stream.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>
#include <pulse/version.h>

#include <cassert>
#include <cstddef>
#include <stdexcept>

#include <stdlib.h>

#define MPD_PULSE_NAME "Music Player Daemon"

class PulseOutput final : AudioOutput {
	const char *name;
	const char *server;
	const char *sink;
	const char *const media_role;

	PulseMixer *mixer = nullptr;

	struct pa_threaded_mainloop *mainloop = nullptr;
	struct pa_context *context;
	struct pa_stream *stream = nullptr;

	size_t writable;

	bool pause;

	/**
	 * Was Interrupt() called?  This will unblock Play().  It will
	 * be reset by Cancel() and Pause(), as documented by the
	 * #AudioOutput interface.
	 *
	 * Only initialized while the output is open.
	 */
	bool interrupted;

	explicit PulseOutput(const ConfigBlock &block);

public:
	void SetMixer(PulseMixer &_mixer);

	void ClearMixer([[maybe_unused]] PulseMixer &old_mixer) {
		assert(mixer == &old_mixer);

		mixer = nullptr;
	}

	void SetVolume(const pa_cvolume &volume);

	struct pa_threaded_mainloop *GetMainloop() {
		return mainloop;
	}

	void OnContextStateChanged(pa_context_state_t new_state);
	void OnServerLayoutChanged(pa_subscription_event_type_t t,
				   uint32_t idx);
	void OnStreamSuspended(pa_stream *_stream);
	void OnStreamStateChanged(pa_stream *_stream,
				  pa_stream_state_t new_state);
	void OnStreamWrite(size_t nbytes);

	void OnStreamSuccess() {
		Signal();
	}

	static bool TestDefaultDevice();

	static AudioOutput *Create(EventLoop &,
				   const ConfigBlock &block) {
		return new PulseOutput(block);
	}

	void Enable() override;
	void Disable() noexcept override;

	void Open(AudioFormat &audio_format) override;
	void Close() noexcept override;

	void Interrupt() noexcept override;

	[[nodiscard]] std::chrono::steady_clock::duration Delay() const noexcept override;
	size_t Play(const void *chunk, size_t size) override;
	void Cancel() noexcept override;
	bool Pause() override;

private:
	/**
	 * Attempt to connect asynchronously to the PulseAudio server.
	 *
	 * Throws on error.
	 */
	void Connect();

	/**
	 * Create, set up and connect a context.
	 *
	 * Caller must lock the main loop.
	 *
	 * Throws on error.
	 */
	void SetupContext();

	/**
	 * Frees and clears the context.
	 *
	 * Caller must lock the main loop.
	 */
	void DeleteContext();

	void Signal() {
		pa_threaded_mainloop_signal(mainloop, 0);
	}

	/**
	 * Check if the context is (already) connected, and waits if
	 * not.  If the context has been disconnected, retry to
	 * connect.
	 *
	 * Caller must lock the main loop.
	 *
	 * Throws on error.
	 */
	void WaitConnection();

	/**
	 * Create, set up and connect a context.
	 *
	 * Caller must lock the main loop.
	 *
	 * Throws on error.
	 */
	void SetupStream(const pa_sample_spec &ss);

	/**
	 * Frees and clears the stream.
	 */
	void DeleteStream();

	/**
	 * Check if the stream is (already) connected, and waits if
	 * not.  The mainloop must be locked before calling this
	 * function.
	 *
	 * Throws on error.
	 */
	void WaitStream();

	/**
	 * Sets cork mode on the stream.
	 *
	 * Throws on error.
	 */
	void StreamPause(bool pause);
};

PulseOutput::PulseOutput(const ConfigBlock &block)
	:AudioOutput(FLAG_ENABLE_DISABLE|FLAG_PAUSE),
	 name(block.GetBlockValue("name", "mpd_pulse")),
	 server(block.GetBlockValue("server")),
	 sink(block.GetBlockValue("sink")),
	 media_role(block.GetBlockValue("media_role"))
{
	setenv("PULSE_PROP_media.role", "music", true);
	setenv("PULSE_PROP_application.icon_name", "mpd", true);
}

struct pa_threaded_mainloop *
pulse_output_get_mainloop(PulseOutput &po)
{
	return po.GetMainloop();
}

inline void
PulseOutput::SetMixer(PulseMixer &_mixer)
{
	assert(mixer == nullptr);

	mixer = &_mixer;

	if (mainloop == nullptr)
		return;

	Pulse::LockGuard lock(mainloop);

	if (context != nullptr &&
	    pa_context_get_state(context) == PA_CONTEXT_READY) {
		pulse_mixer_on_connect(_mixer, context);

		if (stream != nullptr &&
		    pa_stream_get_state(stream) == PA_STREAM_READY)
			pulse_mixer_on_change(_mixer, context, stream);
	}
}

void
pulse_output_set_mixer(PulseOutput &po, PulseMixer &pm)
{
	po.SetMixer(pm);
}

void
pulse_output_clear_mixer(PulseOutput &po, PulseMixer &pm)
{
	po.ClearMixer(pm);
}

inline void
PulseOutput::SetVolume(const pa_cvolume &volume)
{
	if (context == nullptr || stream == nullptr ||
	    pa_stream_get_state(stream) != PA_STREAM_READY)
		throw std::runtime_error("disconnected");

	pa_operation *o =
		pa_context_set_sink_input_volume(context,
						 pa_stream_get_index(stream),
						 &volume, nullptr, nullptr);
	if (o == nullptr)
		throw std::runtime_error("failed to set PulseAudio volume");

	pa_operation_unref(o);
}

void
pulse_output_set_volume(PulseOutput &po, const pa_cvolume *volume)
{
	return po.SetVolume(*volume);
}

/**
 * \brief waits for a pulseaudio operation to finish, frees it and
 *     unlocks the mainloop
 * \param operation the operation to wait for
 * \return true if operation has finished normally (DONE state),
 *     false otherwise
 */
static bool
pulse_wait_for_operation(struct pa_threaded_mainloop *mainloop,
			 struct pa_operation *operation)
{
	assert(mainloop != nullptr);
	assert(operation != nullptr);

	pa_operation_state_t state;
	while ((state = pa_operation_get_state(operation))
	       == PA_OPERATION_RUNNING)
		pa_threaded_mainloop_wait(mainloop);

	pa_operation_unref(operation);

	return state == PA_OPERATION_DONE;
}

/**
 * Callback function for stream operation.  It just sends a signal to
 * the caller thread, to wake pulse_wait_for_operation() up.
 */
static void
pulse_output_stream_success_cb([[maybe_unused]] pa_stream *s,
			       [[maybe_unused]] int success, void *userdata)
{
	PulseOutput &po = *static_cast<PulseOutput *>(userdata);

	po.OnStreamSuccess();
}

inline void
PulseOutput::OnContextStateChanged(pa_context_state_t new_state)
{
	switch (new_state) {
	case PA_CONTEXT_READY:
		if (mixer != nullptr)
			pulse_mixer_on_connect(*mixer, context);

		Signal();
		break;

	case PA_CONTEXT_TERMINATED:
	case PA_CONTEXT_FAILED:
		if (mixer != nullptr)
			pulse_mixer_on_disconnect(*mixer);

		/* the caller thread might be waiting for these
		   states */
		Signal();
		break;

	case PA_CONTEXT_UNCONNECTED:
	case PA_CONTEXT_CONNECTING:
	case PA_CONTEXT_AUTHORIZING:
	case PA_CONTEXT_SETTING_NAME:
		break;
	}
}

static void
pulse_output_context_state_cb(struct pa_context *context, void *userdata)
{
	PulseOutput &po = *static_cast<PulseOutput *>(userdata);

	po.OnContextStateChanged(pa_context_get_state(context));
}

inline void
PulseOutput::OnServerLayoutChanged(pa_subscription_event_type_t t,
				   uint32_t idx)
{
	auto facility =
		pa_subscription_event_type_t(t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK);
	auto type =
		pa_subscription_event_type_t(t & PA_SUBSCRIPTION_EVENT_TYPE_MASK);

	if (mixer != nullptr &&
	    facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT &&
	    stream != nullptr &&
	    pa_stream_get_state(stream) == PA_STREAM_READY &&
	    idx == pa_stream_get_index(stream) &&
	    (type == PA_SUBSCRIPTION_EVENT_NEW ||
	     type == PA_SUBSCRIPTION_EVENT_CHANGE))
		pulse_mixer_on_change(*mixer, context, stream);
}

static void
pulse_output_subscribe_cb([[maybe_unused]] pa_context *context,
			  pa_subscription_event_type_t t,
			  uint32_t idx, void *userdata)
{
	PulseOutput &po = *static_cast<PulseOutput *>(userdata);

	po.OnServerLayoutChanged(t, idx);
}

inline void
PulseOutput::Connect()
{
	assert(context != nullptr);

	if (pa_context_connect(context, server,
			       pa_context_flags_t(0), nullptr) < 0)
		throw MakePulseError(context,
				     "pa_context_connect() has failed");
}

void
PulseOutput::DeleteStream()
{
	assert(stream != nullptr);

	pa_stream_set_suspended_callback(stream, nullptr, nullptr);

	pa_stream_set_state_callback(stream, nullptr, nullptr);
	pa_stream_set_write_callback(stream, nullptr, nullptr);

	pa_stream_disconnect(stream);
	pa_stream_unref(stream);
	stream = nullptr;
}

void
PulseOutput::DeleteContext()
{
	assert(context != nullptr);

	pa_context_set_state_callback(context, nullptr, nullptr);
	pa_context_set_subscribe_callback(context, nullptr, nullptr);

	pa_context_disconnect(context);
	pa_context_unref(context);
	context = nullptr;
}

void
PulseOutput::SetupContext()
{
	assert(mainloop != nullptr);

	pa_proplist *proplist = pa_proplist_new();
	if (media_role)
		pa_proplist_sets(proplist, PA_PROP_MEDIA_ROLE, media_role);

	context = pa_context_new_with_proplist(pa_threaded_mainloop_get_api(mainloop),
					       MPD_PULSE_NAME,
					       proplist);

	pa_proplist_free(proplist);

	if (context == nullptr)
		throw std::runtime_error("pa_context_new() has failed");

	pa_context_set_state_callback(context,
				      pulse_output_context_state_cb, this);
	pa_context_set_subscribe_callback(context,
					  pulse_output_subscribe_cb, this);

	try {
		Connect();
	} catch (...) {
		DeleteContext();
		throw;
	}
}

void
PulseOutput::Enable()
{
	assert(mainloop == nullptr);

	/* create the libpulse mainloop and start the thread */

	mainloop = pa_threaded_mainloop_new();
	if (mainloop == nullptr)
		throw std::runtime_error("pa_threaded_mainloop_new() has failed");

	pa_threaded_mainloop_lock(mainloop);

	if (pa_threaded_mainloop_start(mainloop) < 0) {
		pa_threaded_mainloop_unlock(mainloop);
		pa_threaded_mainloop_free(mainloop);
		mainloop = nullptr;

		throw std::runtime_error("pa_threaded_mainloop_start() has failed");
	}

	/* create the libpulse context and connect it */

	try {
		SetupContext();
	} catch (...) {
		pa_threaded_mainloop_unlock(mainloop);
		pa_threaded_mainloop_stop(mainloop);
		pa_threaded_mainloop_free(mainloop);
		mainloop = nullptr;
		throw;
	}

	pa_threaded_mainloop_unlock(mainloop);
}

void
PulseOutput::Disable() noexcept
{
	assert(mainloop != nullptr);

	pa_threaded_mainloop_stop(mainloop);
	if (context != nullptr)
		DeleteContext();
	pa_threaded_mainloop_free(mainloop);
	mainloop = nullptr;
}

void
PulseOutput::WaitConnection()
{
	assert(mainloop != nullptr);

	pa_context_state_t state;

	if (context == nullptr)
		SetupContext();

	while (true) {
		state = pa_context_get_state(context);
		switch (state) {
		case PA_CONTEXT_READY:
			/* nothing to do */
			return;

		case PA_CONTEXT_UNCONNECTED:
		case PA_CONTEXT_TERMINATED:
		case PA_CONTEXT_FAILED:
			/* failure */
			{
				auto e = MakePulseError(context,
							"failed to connect");
				DeleteContext();
				throw e;
			}

		case PA_CONTEXT_CONNECTING:
		case PA_CONTEXT_AUTHORIZING:
		case PA_CONTEXT_SETTING_NAME:
			/* wait some more */
			pa_threaded_mainloop_wait(mainloop);
			break;
		}
	}
}

inline void
PulseOutput::OnStreamSuspended([[maybe_unused]] pa_stream *_stream)
{
	assert(_stream == stream || stream == nullptr);
	assert(mainloop != nullptr);

	/* wake up the main loop to break out of the loop in
	   pulse_output_play() */
	Signal();
}

static void
pulse_output_stream_suspended_cb(pa_stream *stream, void *userdata)
{
	PulseOutput &po = *static_cast<PulseOutput *>(userdata);

	po.OnStreamSuspended(stream);
}

inline void
PulseOutput::OnStreamStateChanged(pa_stream *_stream,
				  pa_stream_state_t new_state)
{
	assert(_stream == stream || stream == nullptr);
	assert(mainloop != nullptr);
	assert(context != nullptr);

	switch (new_state) {
	case PA_STREAM_READY:
		if (mixer != nullptr)
			pulse_mixer_on_change(*mixer, context, _stream);

		Signal();
		break;

	case PA_STREAM_FAILED:
	case PA_STREAM_TERMINATED:
		if (mixer != nullptr)
			pulse_mixer_on_disconnect(*mixer);

		Signal();
		break;

	case PA_STREAM_UNCONNECTED:
	case PA_STREAM_CREATING:
		break;
	}
}

static void
pulse_output_stream_state_cb(pa_stream *stream, void *userdata)
{
	PulseOutput &po = *static_cast<PulseOutput *>(userdata);

	return po.OnStreamStateChanged(stream, pa_stream_get_state(stream));
}

inline void
PulseOutput::OnStreamWrite(size_t nbytes)
{
	assert(mainloop != nullptr);

	writable = nbytes;
	Signal();
}

static void
pulse_output_stream_write_cb([[maybe_unused]] pa_stream *stream, size_t nbytes,
			     void *userdata)
{
	PulseOutput &po = *static_cast<PulseOutput *>(userdata);

	return po.OnStreamWrite(nbytes);
}

inline void
PulseOutput::SetupStream(const pa_sample_spec &ss)
{
	assert(context != nullptr);

	/* WAVE-EX is been adopted as the speaker map for most media files */
	pa_channel_map chan_map;
	pa_channel_map_init_extend(&chan_map, ss.channels,
				   PA_CHANNEL_MAP_WAVEEX);
	stream = pa_stream_new(context, name, &ss, &chan_map);
	if (stream == nullptr)
		throw MakePulseError(context,
				     "pa_stream_new() has failed");

	pa_stream_set_suspended_callback(stream,
					 pulse_output_stream_suspended_cb,
					 this);

	pa_stream_set_state_callback(stream,
				     pulse_output_stream_state_cb, this);
	pa_stream_set_write_callback(stream,
				     pulse_output_stream_write_cb, this);
}

void
PulseOutput::Open(AudioFormat &audio_format)
{
	assert(mainloop != nullptr);

	Pulse::LockGuard lock(mainloop);

	if (context != nullptr) {
		switch (pa_context_get_state(context)) {
		case PA_CONTEXT_UNCONNECTED:
		case PA_CONTEXT_TERMINATED:
		case PA_CONTEXT_FAILED:
			/* the connection was closed meanwhile; delete
			   it, and pulse_output_wait_connection() will
			   reopen it */
			DeleteContext();
			break;

		case PA_CONTEXT_READY:
		case PA_CONTEXT_CONNECTING:
		case PA_CONTEXT_AUTHORIZING:
		case PA_CONTEXT_SETTING_NAME:
			break;
		}
	}

	WaitConnection();

	/* Use the sample formats that our version of PulseAudio and MPD
	   have in common, otherwise force MPD to send 16 bit */

	pa_sample_spec ss;

	switch (audio_format.format) {
	case SampleFormat::FLOAT:
		ss.format = PA_SAMPLE_FLOAT32NE;
		break;
	case SampleFormat::S32:
		ss.format = PA_SAMPLE_S32NE;
		break;
	case SampleFormat::S24_P32:
		ss.format = PA_SAMPLE_S24_32NE;
		break;
	case SampleFormat::S16:
		ss.format = PA_SAMPLE_S16NE;
		break;
	default:
		audio_format.format = SampleFormat::S16;
		ss.format = PA_SAMPLE_S16NE;
		break;
	}

	ss.rate = std::min(audio_format.sample_rate, PA_RATE_MAX);
	ss.channels = audio_format.channels;

	/* create a stream .. */

	SetupStream(ss);

	/* .. and connect it (asynchronously) */

	if (pa_stream_connect_playback(stream, sink,
				       nullptr, pa_stream_flags_t(0),
				       nullptr, nullptr) < 0) {
		DeleteStream();

		throw MakePulseError(context,
				     "pa_stream_connect_playback() has failed");
	}

	pause = false;
	interrupted = false;
}

void
PulseOutput::Close() noexcept
{
	assert(mainloop != nullptr);

	Pulse::LockGuard lock(mainloop);

	if (pa_stream_get_state(stream) == PA_STREAM_READY) {
		pa_operation *o =
			pa_stream_drain(stream,
					pulse_output_stream_success_cb, this);
		if (o == nullptr) {
			LogPulseError(context,
				      "pa_stream_drain() has failed");
		} else
			pulse_wait_for_operation(mainloop, o);
	}

	DeleteStream();

	if (context != nullptr &&
	    pa_context_get_state(context) != PA_CONTEXT_READY)
		DeleteContext();
}

void
PulseOutput::Interrupt() noexcept
{
	if (mainloop == nullptr)
		return;

	const Pulse::LockGuard lock(mainloop);

	/* the "interrupted" flag will prevent Play() from blocking,
	   and will instead throw AudioOutputInterrupted */
	interrupted = true;

	Signal();
}

void
PulseOutput::WaitStream()
{
	while (true) {
		switch (pa_stream_get_state(stream)) {
		case PA_STREAM_READY:
			return;

		case PA_STREAM_FAILED:
		case PA_STREAM_TERMINATED:
		case PA_STREAM_UNCONNECTED:
			throw MakePulseError(context,
					     "failed to connect the stream");

		case PA_STREAM_CREATING:
			if (interrupted)
				throw AudioOutputInterrupted{};

			pa_threaded_mainloop_wait(mainloop);
			break;
		}
	}
}

void
PulseOutput::StreamPause(bool _pause)
{
	assert(mainloop != nullptr);
	assert(context != nullptr);
	assert(stream != nullptr);

	pa_operation *o = pa_stream_cork(stream, _pause,
					 pulse_output_stream_success_cb, this);
	if (o == nullptr)
		throw MakePulseError(context,
				     "pa_stream_cork() has failed");

	if (!pulse_wait_for_operation(mainloop, o))
		throw MakePulseError(context,
				     "pa_stream_cork() has failed");
}

std::chrono::steady_clock::duration
PulseOutput::Delay() const noexcept
{
	Pulse::LockGuard lock(mainloop);

	auto result = std::chrono::steady_clock::duration::zero();
	if (pause && pa_stream_is_corked(stream) &&
	    pa_stream_get_state(stream) == PA_STREAM_READY)
		/* idle while paused */
		result = std::chrono::seconds(1);

	return result;
}

size_t
PulseOutput::Play(const void *chunk, size_t size)
{
	assert(mainloop != nullptr);
	assert(stream != nullptr);

	Pulse::LockGuard lock(mainloop);

	pause = false;

	/* check if the stream is (already) connected */

	WaitStream();

	assert(context != nullptr);

	/* unpause if previously paused */

	if (pa_stream_is_corked(stream))
		StreamPause(false);

	/* wait until the server allows us to write */

	while (writable == 0) {
		if (pa_stream_is_suspended(stream))
			throw std::runtime_error("suspended");

		if (interrupted)
			throw AudioOutputInterrupted{};

		pa_threaded_mainloop_wait(mainloop);

		if (pa_stream_get_state(stream) != PA_STREAM_READY)
			throw std::runtime_error("disconnected");
	}

	/* now write */

	if (size > writable)
		/* don't send more than possible */
		size = writable;

	writable -= size;

	int result = pa_stream_write(stream, chunk, size, nullptr,
				     0, PA_SEEK_RELATIVE);
	if (result < 0)
		throw MakePulseError(context, "pa_stream_write() failed");

	return size;
}

void
PulseOutput::Cancel() noexcept
{
	assert(mainloop != nullptr);
	assert(stream != nullptr);

	Pulse::LockGuard lock(mainloop);
	interrupted = false;

	if (pa_stream_get_state(stream) != PA_STREAM_READY) {
		/* no need to flush when the stream isn't connected
		   yet */
		return;
	}

	assert(context != nullptr);

	pa_operation *o = pa_stream_flush(stream,
					  pulse_output_stream_success_cb,
					  this);
	if (o == nullptr) {
		LogPulseError(context, "pa_stream_flush() has failed");
		return;
	}

	pulse_wait_for_operation(mainloop, o);
}

bool
PulseOutput::Pause()
{
	assert(mainloop != nullptr);
	assert(stream != nullptr);

	Pulse::LockGuard lock(mainloop);

	pause = true;
	interrupted = false;

	/* check if the stream is (already/still) connected */

	WaitStream();

	assert(context != nullptr);

	/* cork the stream */

	if (!pa_stream_is_corked(stream))
		StreamPause(true);

	return true;
}

inline bool
PulseOutput::TestDefaultDevice()
try {
	const ConfigBlock empty;
	PulseOutput po(empty);
	po.Enable();
	AtScopeExit(&po) { po.Disable(); };
	po.WaitConnection();

	return true;
} catch (...) {
	return false;
}

static bool
pulse_output_test_default_device()
{
	return PulseOutput::TestDefaultDevice();
}

constexpr struct AudioOutputPlugin pulse_output_plugin = {
	"pulse",
	pulse_output_test_default_device,
	PulseOutput::Create,
	&pulse_mixer_plugin,
};
