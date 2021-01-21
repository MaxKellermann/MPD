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
#include "AlsaOutputPlugin.hxx"
#include "lib/alsa/AllowedFormat.hxx"
#include "lib/alsa/HwSetup.hxx"
#include "lib/alsa/NonBlock.hxx"
#include "lib/alsa/PeriodBuffer.hxx"
#include "lib/alsa/Version.hxx"
#include "../OutputAPI.hxx"
#include "../Error.hxx"
#include "mixer/MixerList.hxx"
#include "pcm/Export.hxx"
#include "system/PeriodClock.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"
#include "util/Manual.hxx"
#include "util/RuntimeError.hxx"
#include "util/Domain.hxx"
#include "util/ConstBuffer.hxx"
#include "util/StringView.hxx"
#include "event/MultiSocketMonitor.hxx"
#include "event/InjectEvent.hxx"
#include "event/Call.hxx"
#include "Log.hxx"

#include <alsa/asoundlib.h>

#include <boost/lockfree/spsc_queue.hpp>

#include <string>
#include <forward_list>

static const char default_device[] = "default";

static constexpr unsigned MPD_ALSA_BUFFER_TIME_US = 500000;

class AlsaOutput final
	: AudioOutput, MultiSocketMonitor {

	InjectEvent defer_invalidate_sockets;

	/**
	 * This timer is used to re-schedule the #MultiSocketMonitor
	 * after it had been disabled to wait for the next Play() call
	 * to deliver more data.  This timer is necessary to start
	 * generating silence if Play() doesn't get called soon enough
	 * to avoid the xrun.
	 */
	TimerEvent silence_timer;

	PeriodClock throttle_silence_log;

	Manual<PcmExport> pcm_export;

	/**
	 * The configured name of the ALSA device; empty for the
	 * default device
	 */
	const std::string device;

#ifdef ENABLE_DSD
	/**
	 * Enable DSD over PCM according to the DoP standard?
	 *
	 * @see http://dsd-guide.com/dop-open-standard
	 */
	bool dop_setting;
#endif

	/** libasound's buffer_time setting (in microseconds) */
	const unsigned buffer_time;

	/** libasound's period_time setting (in microseconds) */
	const unsigned period_time;

	/** the mode flags passed to snd_pcm_open */
	int mode = 0;

	std::forward_list<Alsa::AllowedFormat> allowed_formats;

	/**
	 * Protects #dop_setting and #allowed_formats.
	 */
	mutable Mutex attributes_mutex;

	/** the libasound PCM device handle */
	snd_pcm_t *pcm;

	/**
	 * The size of one audio frame passed to method play().
	 */
	size_t in_frame_size;

	/**
	 * The size of one audio frame passed to libasound.
	 */
	size_t out_frame_size;

	/**
	 * The size of one period, in number of frames.
	 */
	snd_pcm_uframes_t period_frames;

	Event::Duration effective_period_duration;

	/**
	 * If snd_pcm_avail() goes above this value and no more data
	 * is available in the #ring_buffer, we need to play some
	 * silence.
	 */
	snd_pcm_sframes_t max_avail_frames;

	/**
	 * Is this a buggy alsa-lib version, which needs a workaround
	 * for the snd_pcm_drain() bug always returning -EAGAIN?  See
	 * alsa-lib commits fdc898d41135 and e4377b16454f for details.
	 * This bug was fixed in alsa-lib version 1.1.4.
	 *
	 * The workaround is to re-enable blocking mode for the
	 * snd_pcm_drain() call.
	 */
	bool work_around_drain_bug;

	/**
	 * After Open() or Cancel(), has this output been activated by
	 * a Play() command?
	 *
	 * Protected by #mutex.
	 */
	bool active;

	/**
	 * Is this output waiting for more data?
	 *
	 * Protected by #mutex.
	 */
	bool waiting;

	/**
	 * Do we need to call snd_pcm_prepare() before the next write?
	 * It means that we put the device to SND_PCM_STATE_SETUP by
	 * calling snd_pcm_drop().
	 *
	 * Without this flag, we could easily recover after a failed
	 * optimistic write (returning -EBADFD), but the Raspberry Pi
	 * audio driver is infamous for generating ugly artefacts from
	 * this.
	 */
	bool must_prepare;

	/**
	 * Has snd_pcm_writei() been called successfully at least once
	 * since the PCM was prepared?
	 *
	 * This is necessary to work around a kernel bug which causes
	 * snd_pcm_drain() to return -EAGAIN forever in non-blocking
	 * mode if snd_pcm_writei() was never called.
	 */
	bool written;

	bool drain;

	/**
	 * Was Interrupt() called?  This will unblock
	 * LockWaitWriteAvailable().  It will be reset by Cancel() and
	 * Pause(), as documented by the #AudioOutput interface.
	 *
	 * Only initialized while the output is open.
	 */
	bool interrupted;

	/**
	 * This buffer gets allocated after opening the ALSA device.
	 * It contains silence samples, enough to fill one period (see
	 * #period_frames).
	 */
	uint8_t *silence;

	AlsaNonBlockPcm non_block;

	/**
	 * For copying data from OutputThread to IOThread.
	 */
	boost::lockfree::spsc_queue<uint8_t> *ring_buffer;

	Alsa::PeriodBuffer period_buffer;

	/**
	 * Protects #cond, #error, #active, #waiting, #drain.
	 */
	mutable Mutex mutex;

	/**
	 * Used to wait when #ring_buffer is full.  It will be
	 * signalled each time data is popped from the #ring_buffer,
	 * making space for more data.
	 */
	Cond cond;

	std::exception_ptr error;

public:
	AlsaOutput(EventLoop &loop, const ConfigBlock &block);

	~AlsaOutput() noexcept override {
		/* free libasound's config cache */
		snd_config_update_free_global();
	}

	using MultiSocketMonitor::GetEventLoop;

	gcc_pure
	const char *GetDevice() const noexcept {
		return device.empty() ? default_device : device.c_str();
	}

	static AudioOutput *Create(EventLoop &event_loop,
				   const ConfigBlock &block) {
		return new AlsaOutput(event_loop, block);
	}

private:
	std::map<std::string, std::string> GetAttributes() const noexcept override;
	void SetAttribute(std::string &&name, std::string &&value) override;

	void Enable() override;
	void Disable() noexcept override;

	void Open(AudioFormat &audio_format) override;
	void Close() noexcept override;

	void Interrupt() noexcept override;

	size_t Play(const void *chunk, size_t size) override;
	void Drain() override;
	void Cancel() noexcept override;
	bool Pause() noexcept override;

	/**
	 * Set up the snd_pcm_t object which was opened by the caller.
	 * Set up the configured settings and the audio format.
	 *
	 * Throws on error.
	 */
	void Setup(AudioFormat &audio_format, PcmExport::Params &params);

#ifdef ENABLE_DSD
	void SetupDop(AudioFormat audio_format,
		      PcmExport::Params &params);
#endif

	void SetupOrDop(AudioFormat &audio_format, PcmExport::Params &params
#ifdef ENABLE_DSD
			, bool dop
#endif
			);

	gcc_pure
	bool LockIsActive() const noexcept {
		const std::lock_guard<Mutex> lock(mutex);
		return active;
	}

	gcc_pure
	bool LockIsActiveAndNotWaiting() const noexcept {
		const std::lock_guard<Mutex> lock(mutex);
		return active && !waiting;
	}

	/**
	 * Activate the output by registering the sockets in the
	 * #EventLoop.  Before calling this, filling the ring buffer
	 * has no effect; nothing will be played, and no code will be
	 * run on #EventLoop's thread.
	 *
	 * Caller must hold the mutex.
	 *
	 * @return true if Activate() was called, false if the mutex
	 * was never unlocked
	 */
	bool Activate() noexcept {
		if (active && !waiting)
			return false;

		active = true;
		waiting = false;

		const ScopeUnlock unlock(mutex);
		defer_invalidate_sockets.Schedule();
		return true;
	}

	/**
	 * Wait until there is some space available in the ring buffer.
	 *
	 * Caller must not lock the mutex.
	 *
	 * Throws on error.
	 *
	 * @return the number of frames available for writing
	 */
	size_t LockWaitWriteAvailable();

	int Recover(int err) noexcept;

	/**
	 * Drain all buffers.  To be run in #EventLoop's thread.
	 *
	 * Throws on error.
	 *
	 * @return true if draining is complete, false if this method
	 * needs to be called again later
	 */
	bool DrainInternal();

	/**
	 * Stop playback immediately, dropping all buffers.  To be run
	 * in #EventLoop's thread.
	 */
	void CancelInternal() noexcept;

	/**
	 * @return false if no data was moved
	 */
	bool CopyRingToPeriodBuffer() noexcept {
		if (period_buffer.IsFull())
			return false;

		size_t nbytes = ring_buffer->pop(period_buffer.GetTail(),
						 period_buffer.GetSpaceBytes());
		if (nbytes == 0)
			return false;

		period_buffer.AppendBytes(nbytes);

		const std::lock_guard<Mutex> lock(mutex);
		/* notify the OutputThread that there is now
		   room in ring_buffer */
		cond.notify_one();

		return true;
	}

	snd_pcm_sframes_t WriteFromPeriodBuffer() noexcept {
		assert(period_buffer.IsFull());
		assert(period_buffer.GetFrames(out_frame_size) > 0);

		auto frames_written = snd_pcm_writei(pcm, period_buffer.GetHead(),
						     period_buffer.GetFrames(out_frame_size));
		if (frames_written > 0) {
			written = true;
			period_buffer.ConsumeFrames(frames_written,
						    out_frame_size);
		}

		return frames_written;
	}

	void LockCaughtError() noexcept {
		period_buffer.Clear();

		const std::lock_guard<Mutex> lock(mutex);
		error = std::current_exception();
		active = false;
		waiting = false;
		cond.notify_one();
	}

	/**
	 * Callback for @silence_timer
	 */
	void OnSilenceTimer() noexcept {
		{
			const std::lock_guard<Mutex> lock(mutex);
			assert(active);
			waiting = false;
		}

		MultiSocketMonitor::InvalidateSockets();
	}

	/* virtual methods from class MultiSocketMonitor */
	Event::Duration PrepareSockets() noexcept override;
	void DispatchSockets() noexcept override;
};

static constexpr Domain alsa_output_domain("alsa_output");

AlsaOutput::AlsaOutput(EventLoop &_loop, const ConfigBlock &block)
	:AudioOutput(FLAG_ENABLE_DISABLE),
	 MultiSocketMonitor(_loop),
	 defer_invalidate_sockets(_loop, BIND_THIS_METHOD(InvalidateSockets)),
	 silence_timer(_loop, BIND_THIS_METHOD(OnSilenceTimer)),
	 device(block.GetBlockValue("device", "")),
#ifdef ENABLE_DSD
	 dop_setting(block.GetBlockValue("dop", false) ||
		     /* legacy name from MPD 0.18 and older: */
		     block.GetBlockValue("dsd_usb", false)),
#endif
	 buffer_time(block.GetPositiveValue("buffer_time",
					    MPD_ALSA_BUFFER_TIME_US)),
	 period_time(block.GetPositiveValue("period_time", 0U))
{
#ifdef SND_PCM_NO_AUTO_RESAMPLE
	if (!block.GetBlockValue("auto_resample", true))
		mode |= SND_PCM_NO_AUTO_RESAMPLE;
#endif

#ifdef SND_PCM_NO_AUTO_CHANNELS
	if (!block.GetBlockValue("auto_channels", true))
		mode |= SND_PCM_NO_AUTO_CHANNELS;
#endif

#ifdef SND_PCM_NO_AUTO_FORMAT
	if (!block.GetBlockValue("auto_format", true))
		mode |= SND_PCM_NO_AUTO_FORMAT;
#endif

	const char *allowed_formats_string =
		block.GetBlockValue("allowed_formats", nullptr);
	if (allowed_formats_string != nullptr)
		allowed_formats = Alsa::AllowedFormat::ParseList(allowed_formats_string);
}

std::map<std::string, std::string>
AlsaOutput::GetAttributes() const noexcept
{
	const std::lock_guard<Mutex> lock(attributes_mutex);

	return {
		std::make_pair("allowed_formats",
			       Alsa::ToString(allowed_formats)),
#ifdef ENABLE_DSD
		std::make_pair("dop", dop_setting ? "1" : "0"),
#endif
	};
}

void
AlsaOutput::SetAttribute(std::string &&name, std::string &&value)
{
	if (name == "allowed_formats") {
		const std::lock_guard<Mutex> lock(attributes_mutex);
		allowed_formats = Alsa::AllowedFormat::ParseList({value.data(), value.length()});
#ifdef ENABLE_DSD
	} else if (name == "dop") {
		const std::lock_guard<Mutex> lock(attributes_mutex);
		if (value == "0")
			dop_setting = false;
		else if (value == "1")
			dop_setting = true;
		else
			throw std::invalid_argument("Bad 'dop' value");
#endif
	} else
		AudioOutput::SetAttribute(std::move(name), std::move(value));
}

void
AlsaOutput::Enable()
{
	pcm_export.Construct();
}

void
AlsaOutput::Disable() noexcept
{
	pcm_export.Destruct();
}

static bool
alsa_test_default_device()
{
	snd_pcm_t *handle;

	int ret = snd_pcm_open(&handle, default_device,
			       SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (ret) {
		FormatError(alsa_output_domain,
			    "Error opening default ALSA device: %s",
			    snd_strerror(-ret));
		return false;
	} else
		snd_pcm_close(handle);

	return true;
}

/**
 * Wrapper for snd_pcm_sw_params().
 */
static void
AlsaSetupSw(snd_pcm_t *pcm, snd_pcm_uframes_t start_threshold,
	    snd_pcm_uframes_t avail_min)
{
	snd_pcm_sw_params_t *swparams;
	snd_pcm_sw_params_alloca(&swparams);

	int err = snd_pcm_sw_params_current(pcm, swparams);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_sw_params_current() failed: %s",
					 snd_strerror(-err));

	err = snd_pcm_sw_params_set_start_threshold(pcm, swparams,
						    start_threshold);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_sw_params_set_start_threshold() failed: %s",
					 snd_strerror(-err));

	err = snd_pcm_sw_params_set_avail_min(pcm, swparams, avail_min);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_sw_params_set_avail_min() failed: %s",
					 snd_strerror(-err));

	err = snd_pcm_sw_params(pcm, swparams);
	if (err < 0)
		throw FormatRuntimeError("snd_pcm_sw_params() failed: %s",
					 snd_strerror(-err));
}

inline void
AlsaOutput::Setup(AudioFormat &audio_format,
		  PcmExport::Params &params)
{
	const auto hw_result = Alsa::SetupHw(pcm,
					     buffer_time, period_time,
					     audio_format, params);

	FormatDebug(alsa_output_domain, "format=%s (%s)",
		    snd_pcm_format_name(hw_result.format),
		    snd_pcm_format_description(hw_result.format));

	FormatDebug(alsa_output_domain, "buffer_size=%u period_size=%u",
		    unsigned(hw_result.buffer_size),
		    unsigned(hw_result.period_size));

	AlsaSetupSw(pcm, hw_result.buffer_size - hw_result.period_size,
		    hw_result.period_size);

	auto alsa_period_size = hw_result.period_size;
	if (alsa_period_size == 0)
		/* this works around a SIGFPE bug that occurred when
		   an ALSA driver indicated period_size==0; this
		   caused a division by zero in alsa_play().  By using
		   the fallback "1", we make sure that this won't
		   happen again. */
		alsa_period_size = 1;

	period_frames = alsa_period_size;
	effective_period_duration = audio_format.FramesToTime<decltype(effective_period_duration)>(period_frames);

	/* generate silence if there's less than one period of data
	   in the ALSA-PCM buffer */
	max_avail_frames = hw_result.buffer_size - hw_result.period_size;

	silence = new uint8_t[snd_pcm_frames_to_bytes(pcm, alsa_period_size)];
	snd_pcm_format_set_silence(hw_result.format, silence,
				   alsa_period_size * audio_format.channels);

}

#ifdef ENABLE_DSD

inline void
AlsaOutput::SetupDop(const AudioFormat audio_format,
		     PcmExport::Params &params)
{
	assert(audio_format.format == SampleFormat::DSD);

	/* pass 24 bit to AlsaSetup() */

	AudioFormat dop_format = audio_format;
	dop_format.format = SampleFormat::S24_P32;

	const AudioFormat check = dop_format;

	Setup(dop_format, params);

	/* if the device allows only 32 bit, shift all DoP
	   samples left by 8 bit and leave the lower 8 bit cleared;
	   the DSD-over-USB documentation does not specify whether
	   this is legal, but there is anecdotical evidence that this
	   is possible (and the only option for some devices) */
	params.shift8 = dop_format.format == SampleFormat::S32;
	if (dop_format.format == SampleFormat::S32)
		dop_format.format = SampleFormat::S24_P32;

	if (dop_format != check) {
		/* no bit-perfect playback, which is required
		   for DSD over USB */
		delete[] silence;
		throw std::runtime_error("Failed to configure DSD-over-PCM");
	}
}

#endif

inline void
AlsaOutput::SetupOrDop(AudioFormat &audio_format, PcmExport::Params &params
#ifdef ENABLE_DSD
		       , bool dop
#endif
		       )
{
#ifdef ENABLE_DSD
	std::exception_ptr dop_error;
	if (dop && audio_format.format == SampleFormat::DSD) {
		try {
			params.dsd_mode = PcmExport::DsdMode::DOP;
			SetupDop(audio_format, params);
			return;
		} catch (...) {
			dop_error = std::current_exception();
			params.dsd_mode = PcmExport::DsdMode::NONE;
		}
	}

	try {
#endif
		Setup(audio_format, params);
#ifdef ENABLE_DSD
	} catch (...) {
		if (dop_error)
			/* if DoP was attempted, prefer returning the
			   original DoP error instead of the fallback
			   error */
			std::rethrow_exception(dop_error);
		else
			throw;
	}
#endif
}

static constexpr bool
MaybeDmix(snd_pcm_type_t type)
{
	return type == SND_PCM_TYPE_DMIX || type == SND_PCM_TYPE_PLUG;
}

gcc_pure
static bool
MaybeDmix(snd_pcm_t *pcm) noexcept
{
	return MaybeDmix(snd_pcm_type(pcm));
}

static const Alsa::AllowedFormat &
BestMatch(const std::forward_list<Alsa::AllowedFormat> &haystack,
	  const AudioFormat &needle)
{
	assert(!haystack.empty());

	for (const auto &i : haystack)
		if (needle.MatchMask(i.format))
			return i;

	return haystack.front();
}

void
AlsaOutput::Open(AudioFormat &audio_format)
{
#ifdef ENABLE_DSD
	bool dop;
#endif

	{
		const std::lock_guard<Mutex> lock(attributes_mutex);
#ifdef ENABLE_DSD
		dop = dop_setting;
#endif

		if (!allowed_formats.empty()) {
			const auto &a = BestMatch(allowed_formats,
						  audio_format);
			audio_format.ApplyMask(a.format);
#ifdef ENABLE_DSD
			dop = a.dop;
#endif
		}
	}

	int err = snd_pcm_open(&pcm, GetDevice(),
			       SND_PCM_STREAM_PLAYBACK, mode);
	if (err < 0)
		throw FormatRuntimeError("Failed to open ALSA device \"%s\": %s",
					 GetDevice(), snd_strerror(err));

	FormatDebug(alsa_output_domain, "opened %s type=%s",
		    snd_pcm_name(pcm),
		    snd_pcm_type_name(snd_pcm_type(pcm)));

	PcmExport::Params params;
	params.alsa_channel_order = true;

	try {
		SetupOrDop(audio_format, params
#ifdef ENABLE_DSD
			   , dop
#endif
			   );
	} catch (...) {
		snd_pcm_close(pcm);
		std::throw_with_nested(FormatRuntimeError("Error opening ALSA device \"%s\"",
							  GetDevice()));
	}

	work_around_drain_bug = MaybeDmix(pcm) &&
		GetRuntimeAlsaVersion() < MakeAlsaVersion(1, 1, 4);

	snd_pcm_nonblock(pcm, 1);

#ifdef ENABLE_DSD
	if (params.dsd_mode == PcmExport::DsdMode::DOP)
		FormatDebug(alsa_output_domain, "DoP (DSD over PCM) enabled");
#endif

	pcm_export->Open(audio_format.format,
			 audio_format.channels,
			 params);

	in_frame_size = audio_format.GetFrameSize();
	out_frame_size = pcm_export->GetOutputFrameSize();

	drain = false;
	interrupted = false;

	size_t period_size = period_frames * out_frame_size;
	ring_buffer = new boost::lockfree::spsc_queue<uint8_t>(period_size * 4);

	period_buffer.Allocate(period_frames, out_frame_size);

	active = false;
	waiting = false;
	must_prepare = false;
	written = false;
	error = {};
}

void
AlsaOutput::Interrupt() noexcept
{
	std::unique_lock<Mutex> lock(mutex);

	/* the "interrupted" flag will prevent
	   LockWaitWriteAvailable() from actually waiting, and will
	   instead throw AudioOutputInterrupted */
	interrupted = true;
	cond.notify_one();
}

inline int
AlsaOutput::Recover(int err) noexcept
{
	if (err == -EPIPE) {
		FormatDebug(alsa_output_domain,
			    "Underrun on ALSA device \"%s\"",
			    GetDevice());
	} else if (err == -ESTRPIPE) {
		FormatDebug(alsa_output_domain,
			    "ALSA device \"%s\" was suspended",
			    GetDevice());
	}

	switch (snd_pcm_state(pcm)) {
	case SND_PCM_STATE_PAUSED:
		err = snd_pcm_pause(pcm, /* disable */ 0);
		break;
	case SND_PCM_STATE_SUSPENDED:
		err = snd_pcm_resume(pcm);
		if (err == -EAGAIN)
			return 0;
		/* fall-through to snd_pcm_prepare: */
#if CLANG_OR_GCC_VERSION(7,0)
		[[fallthrough]];
#endif
	case SND_PCM_STATE_OPEN:
	case SND_PCM_STATE_SETUP:
	case SND_PCM_STATE_XRUN:
		period_buffer.Rewind();
		written = false;
		err = snd_pcm_prepare(pcm);
		break;

	case SND_PCM_STATE_DISCONNECTED:
	case SND_PCM_STATE_DRAINING:
		/* can't play in this state; throw the error */
		break;

	case SND_PCM_STATE_PREPARED:
	case SND_PCM_STATE_RUNNING:
		/* the state is ok, but the error was unexpected;
		   throw it */
		break;

	default:
		/* this default case is just here to work around
		   -Wswitch due to SND_PCM_STATE_PRIVATE1 (libasound
		   1.1.6) */
		break;
	}

	return err;
}

inline bool
AlsaOutput::DrainInternal()
{
	/* drain ring_buffer */
	CopyRingToPeriodBuffer();

	/* drain period_buffer */
	if (!period_buffer.IsCleared()) {
		if (!period_buffer.IsFull())
			/* generate some silence to finish the partial
			   period */
			period_buffer.FillWithSilence(silence, out_frame_size);

		/* drain period_buffer */
		if (!period_buffer.IsDrained()) {
			auto frames_written = WriteFromPeriodBuffer();
			if (frames_written < 0) {
				if (frames_written == -EAGAIN)
					return false;

				throw FormatRuntimeError("snd_pcm_writei() failed: %s",
							 snd_strerror(-frames_written));
			}

			/* need to call CopyRingToPeriodBuffer() and
			   WriteFromPeriodBuffer() again in the next
			   iteration, so don't finish the drain just
			   yet */
			return false;
		}
	}

	if (!written)
		/* if nothing has ever been written to the PCM, we
		   don't need to drain it */
		return true;

	switch (snd_pcm_state(pcm)) {
	case SND_PCM_STATE_PREPARED:
	case SND_PCM_STATE_RUNNING:
		/* these states require a call to snd_pcm_drain() */
		break;

	case SND_PCM_STATE_DRAINING:
		/* already draining, but not yet finished; this is
		   probably a spurious epoll event, and we should wait
		   for the next one */
		return false;

	default:
		/* all other states cannot be drained, and we're
		   done */
		return true;
	}

	/* .. and finally drain the ALSA hardware buffer */

	int result;
	if (work_around_drain_bug) {
		snd_pcm_nonblock(pcm, 0);
		result = snd_pcm_drain(pcm);
		snd_pcm_nonblock(pcm, 1);
	} else
		result = snd_pcm_drain(pcm);

	if (result == 0)
		return true;
	else if (result == -EAGAIN)
		return false;
	else
		throw FormatRuntimeError("snd_pcm_drain() failed: %s",
					 snd_strerror(-result));
}

void
AlsaOutput::Drain()
{
	std::unique_lock<Mutex> lock(mutex);

	if (error)
		std::rethrow_exception(error);

	drain = true;

	Activate();

	cond.wait(lock, [this]{ return !drain || !active; });

	if (error)
		std::rethrow_exception(error);
}

inline void
AlsaOutput::CancelInternal() noexcept
{
	/* this method doesn't need to lock the mutex because while it
	   runs, the calling thread is blocked inside Cancel() */

	must_prepare = true;

	snd_pcm_drop(pcm);

	pcm_export->Reset();
	period_buffer.Clear();
	ring_buffer->reset();

	active = false;
	waiting = false;

	MultiSocketMonitor::Reset();
	defer_invalidate_sockets.Cancel();
	silence_timer.Cancel();
}

void
AlsaOutput::Cancel() noexcept
{
	{
		std::unique_lock<Mutex> lock(mutex);
		interrupted = false;
	}

	if (!LockIsActive()) {
		/* early cancel, quick code path without thread
		   synchronization */

		pcm_export->Reset();
		assert(period_buffer.IsCleared());
		ring_buffer->reset();

		return;
	}

	BlockingCall(GetEventLoop(), [this](){
			CancelInternal();
		});
}

bool
AlsaOutput::Pause() noexcept
{
	std::unique_lock<Mutex> lock(mutex);
	interrupted = false;

	/* not implemented - this override exists only to reset the
	   "interrupted" flag */
	return false;
}

void
AlsaOutput::Close() noexcept
{
	/* make sure the I/O thread isn't inside DispatchSockets() */
	BlockingCall(GetEventLoop(), [this](){
			MultiSocketMonitor::Reset();
			defer_invalidate_sockets.Cancel();
			silence_timer.Cancel();
		});

	period_buffer.Free();
	delete ring_buffer;
	snd_pcm_close(pcm);
	delete[] silence;
}

size_t
AlsaOutput::LockWaitWriteAvailable()
{
	const size_t out_block_size = pcm_export->GetOutputBlockSize();
	const size_t min_available = 2 * out_block_size;

	std::unique_lock<Mutex> lock(mutex);

	while (true) {
		if (error)
			std::rethrow_exception(error);

		if (interrupted)
			/* a CANCEL command is in flight - don't block
			   here */
			throw AudioOutputInterrupted{};

		size_t write_available = ring_buffer->write_available();
		if (write_available >= min_available) {
			/* reserve room for one extra block, just in
			   case PcmExport::Export() has some partial
			   block data in its internal buffer */
			write_available -= out_block_size;

			return write_available / out_frame_size;
		}

		/* now that the ring_buffer is full, we can activate
		   the socket handlers to trigger the first
		   snd_pcm_writei() */
		if (Activate())
			/* since everything may have changed while the
			   mutex was unlocked, we need to skip the
			   cond.wait() call below and check the new
			   status */
			continue;

		/* wait for the DispatchSockets() to make room in the
		   ring_buffer */
		cond.wait(lock);
	}
}

size_t
AlsaOutput::Play(const void *chunk, size_t size)
{
	assert(size > 0);
	assert(size % in_frame_size == 0);

	const size_t max_frames = LockWaitWriteAvailable();
	const size_t max_size = max_frames * in_frame_size;
	if (size > max_size)
		size = max_size;

	const auto e = pcm_export->Export({chunk, size});
	if (e.empty())
		return size;

	size_t bytes_written = ring_buffer->push(static_cast<const uint8_t *>(e.data),
						 e.size);
	assert(bytes_written == e.size);
	(void)bytes_written;

	return size;
}

Event::Duration
AlsaOutput::PrepareSockets() noexcept
{
	if (!LockIsActiveAndNotWaiting()) {
		ClearSocketList();
		return Event::Duration(-1);
	}

	try {
		return non_block.PrepareSockets(*this, pcm);
	} catch (...) {
		ClearSocketList();
		LockCaughtError();
		return Event::Duration(-1);
	}
}

void
AlsaOutput::DispatchSockets() noexcept
try {
	non_block.DispatchSockets(*this, pcm);

	if (must_prepare) {
		must_prepare = false;
		written = false;

		int err = snd_pcm_prepare(pcm);
		if (err < 0)
			throw FormatRuntimeError("snd_pcm_prepare() failed: %s",
						 snd_strerror(-err));
	}

	{
		const std::lock_guard<Mutex> lock(mutex);

		assert(active);

		if (drain) {
			{
				ScopeUnlock unlock(mutex);
				if (!DrainInternal())
					return;

				MultiSocketMonitor::InvalidateSockets();
			}

			drain = false;
			cond.notify_one();
			return;
		}
	}

	CopyRingToPeriodBuffer();

	if (!period_buffer.IsFull()) {
		if (snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED ||
		    snd_pcm_avail(pcm) <= max_avail_frames) {
			/* at SND_PCM_STATE_PREPARED (not yet switched
			   to SND_PCM_STATE_RUNNING), we have no
			   pressure to fill the ALSA buffer, because
			   no xrun can possibly occur; and if no data
			   is available right now, we can easily wait
			   until some is available; so we just stop
			   monitoring the ALSA file descriptor, and
			   let it be reactivated by Play()/Activate()
			   whenever more data arrives */
			/* the same applies when there is still enough
			   data in the ALSA-PCM buffer (determined by
			   snd_pcm_avail()); this can happen at the
			   start of playback, when our ring_buffer is
			   smaller than the ALSA-PCM buffer */

			{
				const std::lock_guard<Mutex> lock(mutex);
				waiting = true;
				cond.notify_one();
			}

			/* avoid race condition: see if data has
			   arrived meanwhile before disabling the
			   event (but after setting the "waiting"
			   flag) */
			if (!CopyRingToPeriodBuffer()) {
				MultiSocketMonitor::Reset();
				defer_invalidate_sockets.Cancel();

				/* just in case Play() doesn't get
				   called soon enough, schedule a
				   timer which generates silence
				   before the xrun occurs */
				/* the timer fires in half of a
				   period; this short duration may
				   produce a few more wakeups than
				   necessary, but should be small
				   enough to avoid the xrun */
				silence_timer.Schedule(effective_period_duration / 2);
			}

			return;
		}

		if (throttle_silence_log.CheckUpdate(std::chrono::seconds(5)))
			FormatWarning(alsa_output_domain, "Decoder is too slow; playing silence to avoid xrun");

		/* insert some silence if the buffer has not enough
		   data yet, to avoid ALSA xrun */
		period_buffer.FillWithSilence(silence, out_frame_size);
	}

	auto frames_written = WriteFromPeriodBuffer();
	if (frames_written < 0) {
		if (frames_written == -EAGAIN || frames_written == -EINTR)
			/* try again in the next DispatchSockets()
			   call which is still scheduled */
			return;

		if (Recover(frames_written) < 0)
			throw FormatRuntimeError("snd_pcm_writei() failed: %s",
						 snd_strerror(-frames_written));

		/* recovered; try again in the next DispatchSockets()
		   call */
		return;
	}
} catch (...) {
	MultiSocketMonitor::Reset();
	LockCaughtError();
}

constexpr struct AudioOutputPlugin alsa_output_plugin = {
	"alsa",
	alsa_test_default_device,
	&AlsaOutput::Create,
	&alsa_mixer_plugin,
};
