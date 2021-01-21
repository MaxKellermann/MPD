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

#include "lib/alsa/NonBlock.hxx"
#include "mixer/MixerInternal.hxx"
#include "mixer/Listener.hxx"
#include "output/OutputAPI.hxx"
#include "event/MultiSocketMonitor.hxx"
#include "event/InjectEvent.hxx"
#include "event/Call.hxx"
#include "util/ASCII.hxx"
#include "util/Domain.hxx"
#include "util/Math.hxx"
#include "util/RuntimeError.hxx"
#include "Log.hxx"

extern "C" {
#include "volume_mapping.h"
}

#include <alsa/asoundlib.h>

#define VOLUME_MIXER_ALSA_DEFAULT		"default"
#define VOLUME_MIXER_ALSA_CONTROL_DEFAULT	"PCM"
static constexpr unsigned VOLUME_MIXER_ALSA_INDEX_DEFAULT = 0;

class AlsaMixerMonitor final : MultiSocketMonitor {
	InjectEvent defer_invalidate_sockets;

	snd_mixer_t *mixer;

	AlsaNonBlockMixer non_block;

public:
	AlsaMixerMonitor(EventLoop &_loop, snd_mixer_t *_mixer)
		:MultiSocketMonitor(_loop),
		 defer_invalidate_sockets(_loop,
					  BIND_THIS_METHOD(InvalidateSockets)),
		 mixer(_mixer) {
		defer_invalidate_sockets.Schedule();
	}

	~AlsaMixerMonitor() {
		BlockingCall(MultiSocketMonitor::GetEventLoop(), [this](){
				MultiSocketMonitor::Reset();
				defer_invalidate_sockets.Cancel();
			});
	}

private:
	Event::Duration PrepareSockets() noexcept override;
	void DispatchSockets() noexcept override;
};

class AlsaMixer final : public Mixer {
	EventLoop &event_loop;

	const char *device;
	const char *control;
	unsigned int index;

	snd_mixer_t *handle;
	snd_mixer_elem_t *elem;

	AlsaMixerMonitor *monitor;

public:
	AlsaMixer(EventLoop &_event_loop, MixerListener &_listener)
		:Mixer(alsa_mixer_plugin, _listener),
		 event_loop(_event_loop) {}

	~AlsaMixer() override;

	void Configure(const ConfigBlock &block);
	void Setup();

	/* virtual methods from class Mixer */
	void Open() override;
	void Close() noexcept override;
	int GetVolume() override;
	void SetVolume(unsigned volume) override;
};

static constexpr Domain alsa_mixer_domain("alsa_mixer");

Event::Duration
AlsaMixerMonitor::PrepareSockets() noexcept
{
	if (mixer == nullptr) {
		ClearSocketList();
		return Event::Duration(-1);
	}

	return non_block.PrepareSockets(*this, mixer);
}

void
AlsaMixerMonitor::DispatchSockets() noexcept
{
	assert(mixer != nullptr);

	non_block.DispatchSockets(*this, mixer);

	int err = snd_mixer_handle_events(mixer);
	if (err < 0) {
		FormatError(alsa_mixer_domain,
			    "snd_mixer_handle_events() failed: %s",
			    snd_strerror(err));

		if (err == -ENODEV) {
			/* the sound device was unplugged; disable
			   this GSource */
			mixer = nullptr;
			InvalidateSockets();
			return;
		}
	}
}

/*
 * libasound callbacks
 *
 */

static int
alsa_mixer_elem_callback(snd_mixer_elem_t *elem, unsigned mask)
{
	AlsaMixer &mixer = *static_cast<AlsaMixer *>(snd_mixer_elem_get_callback_private(elem));

	if (mask & SND_CTL_EVENT_MASK_VALUE) {
		try {
			int volume = mixer.GetVolume();
			mixer.listener.OnMixerVolumeChanged(mixer, volume);
		} catch (...) {
		}
	}

	return 0;
}

/*
 * mixer_plugin methods
 *
 */

inline void
AlsaMixer::Configure(const ConfigBlock &block)
{
	device = block.GetBlockValue("mixer_device",
				     VOLUME_MIXER_ALSA_DEFAULT);
	control = block.GetBlockValue("mixer_control",
				      VOLUME_MIXER_ALSA_CONTROL_DEFAULT);
	index = block.GetBlockValue("mixer_index",
				    VOLUME_MIXER_ALSA_INDEX_DEFAULT);
}

static Mixer *
alsa_mixer_init(EventLoop &event_loop, [[maybe_unused]] AudioOutput &ao,
		MixerListener &listener,
		const ConfigBlock &block)
{
	auto *am = new AlsaMixer(event_loop, listener);
	am->Configure(block);

	return am;
}

AlsaMixer::~AlsaMixer()
{
	/* free libasound's config cache */
	snd_config_update_free_global();
}

gcc_pure
static snd_mixer_elem_t *
alsa_mixer_lookup_elem(snd_mixer_t *handle,
		       const char *name, unsigned idx) noexcept
{
	for (snd_mixer_elem_t *elem = snd_mixer_first_elem(handle);
	     elem != nullptr; elem = snd_mixer_elem_next(elem)) {
		if (snd_mixer_elem_get_type(elem) == SND_MIXER_ELEM_SIMPLE &&
		    StringEqualsCaseASCII(snd_mixer_selem_get_name(elem),
					  name) &&
		    snd_mixer_selem_get_index(elem) == idx)
			return elem;
	}

	return nullptr;
}

inline void
AlsaMixer::Setup()
{
	int err;

	if ((err = snd_mixer_attach(handle, device)) < 0)
		throw FormatRuntimeError("failed to attach to %s: %s",
					 device, snd_strerror(err));

	if ((err = snd_mixer_selem_register(handle, nullptr, nullptr)) < 0)
		throw FormatRuntimeError("snd_mixer_selem_register() failed: %s",
					 snd_strerror(err));

	if ((err = snd_mixer_load(handle)) < 0)
		throw FormatRuntimeError("snd_mixer_load() failed: %s\n",
					 snd_strerror(err));

	elem = alsa_mixer_lookup_elem(handle, control, index);
	if (elem == nullptr)
		throw FormatRuntimeError("no such mixer control: %s", control);

	snd_mixer_elem_set_callback_private(elem, this);
	snd_mixer_elem_set_callback(elem, alsa_mixer_elem_callback);

	monitor = new AlsaMixerMonitor(event_loop, handle);
}

void
AlsaMixer::Open()
{
	int err;

	err = snd_mixer_open(&handle, 0);
	if (err < 0)
		throw FormatRuntimeError("snd_mixer_open() failed: %s",
					 snd_strerror(err));

	try {
		Setup();
	} catch (...) {
		snd_mixer_close(handle);
		throw;
	}
}

void
AlsaMixer::Close() noexcept
{
	assert(handle != nullptr);

	delete monitor;

	snd_mixer_elem_set_callback(elem, nullptr);
	snd_mixer_close(handle);
}

int
AlsaMixer::GetVolume()
{
	int err;

	assert(handle != nullptr);

	err = snd_mixer_handle_events(handle);
	if (err < 0)
		throw FormatRuntimeError("snd_mixer_handle_events() failed: %s",
					 snd_strerror(err));

	return lround(100 * get_normalized_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT));
}

void
AlsaMixer::SetVolume(unsigned volume)
{
	assert(handle != nullptr);

	double cur = get_normalized_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT);
	int delta = volume - lround(100.*cur);
	int err = set_normalized_playback_volume(elem, cur + 0.01*delta, delta);
	if (err < 0)
		throw FormatRuntimeError("failed to set ALSA volume: %s",
					 snd_strerror(err));
}

const MixerPlugin alsa_mixer_plugin = {
	alsa_mixer_init,
	true,
};
