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
#include "OSXOutputPlugin.hxx"
#include "apple/AudioObject.hxx"
#include "apple/AudioUnit.hxx"
#include "apple/StringRef.hxx"
#include "apple/Throw.hxx"
#include "../OutputAPI.hxx"
#include "mixer/MixerList.hxx"
#include "util/RuntimeError.hxx"
#include "util/Domain.hxx"
#include "util/Manual.hxx"
#include "util/ConstBuffer.hxx"
#include "pcm/Export.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"
#include "util/ByteOrder.hxx"
#include "util/CharUtil.hxx"
#include "util/StringAPI.hxx"
#include "util/StringBuffer.hxx"
#include "util/StringFormat.hxx"
#include "Log.hxx"

#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreServices/CoreServices.h>
#include <boost/lockfree/spsc_queue.hpp>

#include <memory>

static constexpr unsigned MPD_OSX_BUFFER_TIME_MS = 100;

static StringBuffer<64>
StreamDescriptionToString(const AudioStreamBasicDescription desc)
{
	// Only convert the lpcm formats (nothing else supported / used by MPD)
	assert(desc.mFormatID == kAudioFormatLinearPCM);

	return StringFormat<64>("%u channel %s %sinterleaved %u-bit %s %s (%uHz)",
				desc.mChannelsPerFrame,
				(desc.mFormatFlags & kAudioFormatFlagIsNonMixable) ? "" : "mixable",
				(desc.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? "non-" : "",
				desc.mBitsPerChannel,
				(desc.mFormatFlags & kAudioFormatFlagIsFloat) ? "Float" : "SInt",
				(desc.mFormatFlags & kAudioFormatFlagIsBigEndian) ? "BE" : "LE",
				(UInt32)desc.mSampleRate);
}


struct OSXOutput final : AudioOutput {
	/* configuration settings */
	OSType component_subtype;
	/* only applicable with kAudioUnitSubType_HALOutput */
	const char *device_name;
	const char *const channel_map;
	const bool hog_device;

	bool pause;

	/**
	 * Is the audio unit "started", i.e. was AudioOutputUnitStart() called?
	 */
	bool started;

#ifdef ENABLE_DSD
	/**
	 * Enable DSD over PCM according to the DoP standard?
	 *
	 * @see http://dsd-guide.com/dop-open-standard
	 */
	const bool dop_setting;
	bool dop_enabled;
	Manual<PcmExport> pcm_export;
#endif

	AudioDeviceID dev_id;
	AudioComponentInstance au;
	AudioStreamBasicDescription asbd;

	boost::lockfree::spsc_queue<uint8_t> *ring_buffer;

	OSXOutput(const ConfigBlock &block);

	static AudioOutput *Create(EventLoop &, const ConfigBlock &block);
	int GetVolume();
	void SetVolume(unsigned new_volume);

private:
	void Enable() override;
	void Disable() noexcept override;

	void Open(AudioFormat &audio_format) override;
	void Close() noexcept override;

	std::chrono::steady_clock::duration Delay() const noexcept override;
	size_t Play(const void *chunk, size_t size) override;
	bool Pause() override;
	void Cancel() noexcept override;
};

static constexpr Domain osx_output_domain("osx_output");

static bool
osx_output_test_default_device()
{
	/* on a Mac, this is always the default plugin, if nothing
	   else is configured */
	return true;
}

OSXOutput::OSXOutput(const ConfigBlock &block)
	:AudioOutput(FLAG_ENABLE_DISABLE|FLAG_PAUSE),
	 channel_map(block.GetBlockValue("channel_map")),
	 hog_device(block.GetBlockValue("hog_device", false))
#ifdef ENABLE_DSD
	, dop_setting(block.GetBlockValue("dop", false))
#endif
{
	const char *device = block.GetBlockValue("device");

	if (device == nullptr || StringIsEqual(device, "default")) {
		component_subtype = kAudioUnitSubType_DefaultOutput;
		device_name = nullptr;
	}
	else if (StringIsEqual(device, "system")) {
		component_subtype = kAudioUnitSubType_SystemOutput;
		device_name = nullptr;
	}
	else {
		component_subtype = kAudioUnitSubType_HALOutput;
		/* XXX am I supposed to strdup() this? */
		device_name = device;
	}
}

AudioOutput *
OSXOutput::Create(EventLoop &, const ConfigBlock &block)
{
	OSXOutput *oo = new OSXOutput(block);

	static constexpr AudioObjectPropertyAddress default_system_output_device{
		kAudioHardwarePropertyDefaultSystemOutputDevice,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster,
	};

	static constexpr AudioObjectPropertyAddress default_output_device{
		kAudioHardwarePropertyDefaultOutputDevice,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};

	const auto &aopa =
		oo->component_subtype == kAudioUnitSubType_SystemOutput
		// get system output dev_id if configured
		? default_system_output_device
		/* fallback to default device initially (can still be
		   changed by osx_output_set_device) */
		: default_output_device;

	AudioDeviceID dev_id = kAudioDeviceUnknown;
	UInt32 dev_id_size = sizeof(dev_id);
	AudioObjectGetPropertyData(kAudioObjectSystemObject,
				   &aopa,
				   0,
				   NULL,
				   &dev_id_size,
				   &dev_id);
	oo->dev_id = dev_id;

	return oo;
}


int
OSXOutput::GetVolume()
{
	static constexpr AudioObjectPropertyAddress aopa = {
		kAudioHardwareServiceDeviceProperty_VirtualMasterVolume,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster,
	};

	const auto vol = AudioObjectGetPropertyDataT<Float32>(dev_id,
							      aopa);

	return int(vol * 100.0f);
}

void
OSXOutput::SetVolume(unsigned new_volume)
{
	Float32 vol = new_volume / 100.0;
	static constexpr AudioObjectPropertyAddress aopa = {
		kAudioHardwareServiceDeviceProperty_VirtualMasterVolume,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};
	UInt32 size = sizeof(vol);
	OSStatus status = AudioObjectSetPropertyData(dev_id,
						     &aopa,
						     0,
						     NULL,
						     size,
						     &vol);

	if (status != noErr)
		Apple::ThrowOSStatus(status);
}

static void
osx_output_parse_channel_map(const char *device_name,
			     const char *channel_map_str,
			     SInt32 channel_map[],
			     UInt32 num_channels)
{
	unsigned int inserted_channels = 0;
	bool want_number = true;

	while (*channel_map_str) {
		if (inserted_channels >= num_channels)
			throw FormatRuntimeError("%s: channel map contains more than %u entries or trailing garbage",
						 device_name, num_channels);

		if (!want_number && *channel_map_str == ',') {
			++channel_map_str;
			want_number = true;
			continue;
		}

		if (want_number &&
			(IsDigitASCII(*channel_map_str) || *channel_map_str == '-')
		) {
			char *endptr;
			channel_map[inserted_channels] = strtol(channel_map_str, &endptr, 10);
			if (channel_map[inserted_channels] < -1)
				throw FormatRuntimeError("%s: channel map value %d not allowed (must be -1 or greater)",
							 device_name, channel_map[inserted_channels]);

			channel_map_str = endptr;
			want_number = false;
			FormatDebug(osx_output_domain,
				"%s: channel_map[%u] = %d",
				device_name, inserted_channels, channel_map[inserted_channels]);
			++inserted_channels;
			continue;
		}

		throw FormatRuntimeError("%s: invalid character '%c' in channel map",
					 device_name, *channel_map_str);
	}

	if (inserted_channels < num_channels)
		throw FormatRuntimeError("%s: channel map contains less than %u entries",
					 device_name, num_channels);
}

static UInt32
AudioUnitGetChannelsPerFrame(AudioUnit inUnit)
{
	const auto desc = AudioUnitGetPropertyT<AudioStreamBasicDescription>(inUnit,
									     kAudioUnitProperty_StreamFormat,
									     kAudioUnitScope_Output,
									     0);
	return desc.mChannelsPerFrame;
}

static void
osx_output_set_channel_map(OSXOutput *oo)
{
	OSStatus status;

	const UInt32 num_channels = AudioUnitGetChannelsPerFrame(oo->au);
	std::unique_ptr<SInt32[]> channel_map(new SInt32[num_channels]);
	osx_output_parse_channel_map(oo->device_name,
				     oo->channel_map,
				     channel_map.get(),
				     num_channels);

	UInt32 size = num_channels * sizeof(SInt32);
	status = AudioUnitSetProperty(oo->au,
		kAudioOutputUnitProperty_ChannelMap,
		kAudioUnitScope_Input,
		0,
		channel_map.get(),
		size);
	if (status != noErr)
		Apple::ThrowOSStatus(status, "unable to set channel map");
}


static float
osx_output_score_sample_rate(Float64 destination_rate, unsigned source_rate)
{
	float score = 0;
	double int_portion;
	double frac_portion = modf(source_rate / destination_rate, &int_portion);
	// prefer sample rates that are multiples of the source sample rate
	if (frac_portion < 0.01 || frac_portion >= 0.99)
		score += 1000;
	// prefer exact matches over other multiples
	score += (int_portion == 1.0) ? 500 : 0;
	if (source_rate == destination_rate)
		score += 1000;
	else if (source_rate > destination_rate)
		score += (int_portion > 1 && int_portion < 100) ? (100 - int_portion) / 100 * 100 : 0;
	else
		score += (int_portion > 1 && int_portion < 100) ? (100 + int_portion) / 100 * 100 : 0;

	return score;
}

static float
osx_output_score_format(const AudioStreamBasicDescription &format_desc,
			const AudioStreamBasicDescription &target_format)
{
	float score = 0;
	// Score only linear PCM formats (everything else MPD cannot use)
	if (format_desc.mFormatID == kAudioFormatLinearPCM) {
		score += osx_output_score_sample_rate(format_desc.mSampleRate,
						      target_format.mSampleRate);

		// Just choose the stream / format with the highest number of output channels
		score += format_desc.mChannelsPerFrame * 5;

		if (target_format.mFormatFlags == kLinearPCMFormatFlagIsFloat) {
			// for float, prefer the highest bitdepth we have
			if (format_desc.mBitsPerChannel >= 16)
				score += (format_desc.mBitsPerChannel / 8);
		} else {
			if (format_desc.mBitsPerChannel == target_format.mBitsPerChannel)
				score += 5;
			else if (format_desc.mBitsPerChannel > target_format.mBitsPerChannel)
				score += 1;

		}
	}

	return score;
}

static Float64
osx_output_set_device_format(AudioDeviceID dev_id,
			     const AudioStreamBasicDescription &target_format)
{
	static constexpr AudioObjectPropertyAddress aopa_device_streams = {
		kAudioDevicePropertyStreams,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};

	static constexpr AudioObjectPropertyAddress aopa_stream_direction = {
		kAudioStreamPropertyDirection,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};

	static constexpr AudioObjectPropertyAddress aopa_stream_phys_formats = {
		kAudioStreamPropertyAvailablePhysicalFormats,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};

	static constexpr AudioObjectPropertyAddress aopa_stream_phys_format = {
		kAudioStreamPropertyPhysicalFormat,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};

	OSStatus err;

	const auto streams =
		AudioObjectGetPropertyDataArray<AudioStreamID>(dev_id,
							       aopa_device_streams);

	bool format_found = false;
	int output_stream;
	AudioStreamBasicDescription output_format;

	for (const auto stream : streams) {
		const auto direction =
			AudioObjectGetPropertyDataT<UInt32>(stream,
							    aopa_stream_direction);
		if (direction != 0)
			continue;

		const auto format_list =
			AudioObjectGetPropertyDataArray<AudioStreamRangedDescription>(stream,
										      aopa_stream_phys_formats);

		float output_score = 0;

		for (const auto &format : format_list) {
			AudioStreamBasicDescription format_desc = format.mFormat;
			std::string format_string;

			// for devices with kAudioStreamAnyRate
			// we use the requested samplerate here
			if (format_desc.mSampleRate == kAudioStreamAnyRate)
				format_desc.mSampleRate = target_format.mSampleRate;
			float score = osx_output_score_format(format_desc, target_format);

			// print all (linear pcm) formats and their rating
			if (score > 0.0f)
				FormatDebug(osx_output_domain,
					    "Format: %s rated %f",
					    StreamDescriptionToString(format_desc).c_str(),
					    (double)score);

			if (score > output_score) {
				output_score  = score;
				output_format = format_desc;
				output_stream = stream; // set the idx of the stream in the device
				format_found = true;
			}
		}
	}

	if (format_found) {
		err = AudioObjectSetPropertyData(output_stream,
						 &aopa_stream_phys_format,
						 0,
						 NULL,
						 sizeof(output_format),
						 &output_format);
		if (err != noErr)
			throw FormatRuntimeError("Failed to change the stream format: %d",
						 err);
	}

	return output_format.mSampleRate;
}

static UInt32
osx_output_set_buffer_size(AudioUnit au, AudioStreamBasicDescription desc)
{
	const auto value_range = AudioUnitGetPropertyT<AudioValueRange>(au,
									kAudioDevicePropertyBufferFrameSizeRange,
									kAudioUnitScope_Global,
									0);

	try {
		AudioUnitSetBufferFrameSize(au, value_range.mMaximum);
	} catch (...) {
		LogError(std::current_exception(),
			 "Failed to set maximum buffer size");
	}

	auto buffer_frame_size = AudioUnitGetBufferFrameSize(au);
	buffer_frame_size *= desc.mBytesPerFrame;

	// We set the frame size to a power of two integer that
	// is larger than buffer_frame_size.
	UInt32 frame_size = 1;
	while (frame_size < buffer_frame_size + 1)
		frame_size <<= 1;

	return frame_size;
}

static void
osx_output_hog_device(AudioDeviceID dev_id, bool hog) noexcept
{
	static constexpr AudioObjectPropertyAddress aopa = {
		kAudioDevicePropertyHogMode,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMaster
	};

	pid_t hog_pid;

	try {
		hog_pid = AudioObjectGetPropertyDataT<pid_t>(dev_id, aopa);
	} catch (...) {
		Log(LogLevel::DEBUG, std::current_exception(),
		    "Failed to query HogMode");
		return;
	}

	if (hog) {
		if (hog_pid != -1) {
			FormatDebug(osx_output_domain,
				    "Device is already hogged.");
			return;
		}
	} else {
		if (hog_pid != getpid()) {
			FormatDebug(osx_output_domain,
				    "Device is not owned by this process.");
			return;
		}
	}

	hog_pid = hog ? getpid() : -1;
	UInt32 size = sizeof(hog_pid);
	OSStatus err;
	err = AudioObjectSetPropertyData(dev_id,
					 &aopa,
					 0,
					 NULL,
					 size,
					 &hog_pid);
	if (err != noErr) {
		FormatDebug(osx_output_domain,
			    "Cannot hog the device: %d",
			    err);
	} else {
		LogDebug(osx_output_domain,
			 hog_pid == -1
			 ? "Device is unhogged"
			 : "Device is hogged");
	}
}

gcc_pure
static bool
IsAudioDeviceName(AudioDeviceID id, const char *expected_name) noexcept
{
	static constexpr AudioObjectPropertyAddress aopa_name{
		kAudioObjectPropertyName,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster,
	};

	char actual_name[256];

	try {
		auto cfname = AudioObjectGetStringProperty(id, aopa_name);
		if (!cfname.GetCString(actual_name, sizeof(actual_name)))
			return false;
	} catch (...) {
		return false;
	}

	return StringIsEqual(actual_name, expected_name);
}

static AudioDeviceID
FindAudioDeviceByName(const char *name)
{
	/* what are the available audio device IDs? */
	static constexpr AudioObjectPropertyAddress aopa_hw_devices{
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster,
	};

	const auto ids =
		AudioObjectGetPropertyDataArray<AudioDeviceID>(kAudioObjectSystemObject,
							       aopa_hw_devices);

	for (const auto id : ids) {
		if (IsAudioDeviceName(id, name))
			return id;
	}

	throw FormatRuntimeError("Found no audio device with name '%s' ",
				 name);
}

static void
osx_output_set_device(OSXOutput *oo)
{
	if (oo->component_subtype != kAudioUnitSubType_HALOutput)
		return;

	const auto id = FindAudioDeviceByName(oo->device_name);

	FormatDebug(osx_output_domain,
		    "found matching device: ID=%u, name=%s",
		    (unsigned)id, oo->device_name);

	AudioUnitSetCurrentDevice(oo->au, id);

	oo->dev_id = id;
	FormatDebug(osx_output_domain,
		    "set OS X audio output device ID=%u, name=%s",
		    (unsigned)id, oo->device_name);

	if (oo->channel_map)
		osx_output_set_channel_map(oo);
}


/**
 * This function (the 'render callback' osx_render) is called by the
 * OS X audio subsystem (CoreAudio) to request audio data that will be
 * played by the audio hardware. This function has hard time
 * constraints so it cannot do IO (debug statements) or memory
 * allocations.
 */
static OSStatus
osx_render(void *vdata,
	   [[maybe_unused]] AudioUnitRenderActionFlags *io_action_flags,
	   [[maybe_unused]] const AudioTimeStamp *in_timestamp,
	   [[maybe_unused]] UInt32 in_bus_number,
	   UInt32 in_number_frames,
	   AudioBufferList *buffer_list)
{
	OSXOutput *od = (OSXOutput *) vdata;

	int count = in_number_frames * od->asbd.mBytesPerFrame;
	buffer_list->mBuffers[0].mDataByteSize =
		od->ring_buffer->pop((uint8_t *)buffer_list->mBuffers[0].mData,
				     count);
	return noErr;
}

void
OSXOutput::Enable()
{
	AudioComponentDescription desc;
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = component_subtype;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;

	AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
	if (comp == 0)
		throw std::runtime_error("Error finding OS X component");

	OSStatus status = AudioComponentInstanceNew(comp, &au);
	if (status != noErr)
		Apple::ThrowOSStatus(status, "Unable to open OS X component");

#ifdef ENABLE_DSD
	pcm_export.Construct();
#endif

	try {
		osx_output_set_device(this);
	} catch (...) {
		AudioComponentInstanceDispose(au);
#ifdef ENABLE_DSD
		pcm_export.Destruct();
#endif
		throw;
	}

	if (hog_device)
		osx_output_hog_device(dev_id, true);
}

void
OSXOutput::Disable() noexcept
{
	AudioComponentInstanceDispose(au);
#ifdef ENABLE_DSD
	pcm_export.Destruct();
#endif

	if (hog_device)
		osx_output_hog_device(dev_id, false);
}

void
OSXOutput::Close() noexcept
{
	if (started)
		AudioOutputUnitStop(au);
	AudioUnitUninitialize(au);
	delete ring_buffer;
}

void
OSXOutput::Open(AudioFormat &audio_format)
{
#ifdef ENABLE_DSD
	PcmExport::Params params;
	params.alsa_channel_order = true;
	bool dop = dop_setting;
#endif

	memset(&asbd, 0, sizeof(asbd));
	asbd.mFormatID = kAudioFormatLinearPCM;
	if (audio_format.format == SampleFormat::FLOAT) {
		asbd.mFormatFlags = kLinearPCMFormatFlagIsFloat;
	} else {
		asbd.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger;
	}

	if (IsBigEndian())
		asbd.mFormatFlags |= kLinearPCMFormatFlagIsBigEndian;

	if (audio_format.format == SampleFormat::S24_P32) {
		asbd.mBitsPerChannel = 24;
	} else {
		asbd.mBitsPerChannel = audio_format.GetSampleSize() * 8;
	}
	asbd.mBytesPerPacket = audio_format.GetFrameSize();
	asbd.mSampleRate = audio_format.sample_rate;

#ifdef ENABLE_DSD
	if (dop && audio_format.format == SampleFormat::DSD) {
		asbd.mBitsPerChannel = 24;
		params.dsd_mode = PcmExport::DsdMode::DOP;
		asbd.mSampleRate = params.CalcOutputSampleRate(audio_format.sample_rate);
		asbd.mBytesPerPacket = 4 * audio_format.channels;

	}
#endif

	asbd.mFramesPerPacket = 1;
	asbd.mBytesPerFrame = asbd.mBytesPerPacket;
	asbd.mChannelsPerFrame = audio_format.channels;

	Float64 sample_rate = osx_output_set_device_format(dev_id, asbd);

#ifdef ENABLE_DSD
	if (audio_format.format == SampleFormat::DSD &&
	    sample_rate != asbd.mSampleRate) {
		// fall back to PCM in case sample_rate cannot be synchronized
		params.dsd_mode = PcmExport::DsdMode::NONE;
		audio_format.format = SampleFormat::S32;
		asbd.mBitsPerChannel = 32;
		asbd.mBytesPerPacket = audio_format.GetFrameSize();
		asbd.mSampleRate = params.CalcOutputSampleRate(audio_format.sample_rate);
		asbd.mBytesPerFrame = asbd.mBytesPerPacket;
	}
	dop_enabled = params.dsd_mode == PcmExport::DsdMode::DOP;
#endif

	AudioUnitSetInputStreamFormat(au, asbd);

	AURenderCallbackStruct callback;
	callback.inputProc = osx_render;
	callback.inputProcRefCon = this;

	AudioUnitSetInputRenderCallback(au, callback);

	OSStatus status = AudioUnitInitialize(au);
	if (status != noErr)
		Apple::ThrowOSStatus(status, "Unable to initialize OS X audio unit");

	UInt32 buffer_frame_size = osx_output_set_buffer_size(au, asbd);

	size_t ring_buffer_size = std::max<size_t>(buffer_frame_size,
						   MPD_OSX_BUFFER_TIME_MS * audio_format.GetFrameSize() * audio_format.sample_rate / 1000);

#ifdef ENABLE_DSD
	if (dop_enabled) {
		pcm_export->Open(audio_format.format, audio_format.channels, params);
		ring_buffer_size = std::max<size_t>(buffer_frame_size,
						   MPD_OSX_BUFFER_TIME_MS * pcm_export->GetOutputFrameSize() * asbd.mSampleRate / 1000);
	}
#endif
	ring_buffer = new boost::lockfree::spsc_queue<uint8_t>(ring_buffer_size);

	pause = false;
	started = false;
}

size_t
OSXOutput::Play(const void *chunk, size_t size)
{
	assert(size > 0);

	pause = false;

	ConstBuffer<uint8_t> input((const uint8_t *)chunk, size);

#ifdef ENABLE_DSD
	if (dop_enabled) {
		input = ConstBuffer<uint8_t>::FromVoid(pcm_export->Export(input.ToVoid()));
		if (input.empty())
			return size;
	}
#endif

	size_t bytes_written = ring_buffer->push(input.data, input.size);

	if (!started) {
		OSStatus status = AudioOutputUnitStart(au);
		if (status != noErr)
			throw std::runtime_error("Unable to restart audio output after pause");

		started = true;
	}

#ifdef ENABLE_DSD
	if (dop_enabled)
		bytes_written = pcm_export->CalcInputSize(bytes_written);
#endif

	return bytes_written;
}

std::chrono::steady_clock::duration
OSXOutput::Delay() const noexcept
{
	return ring_buffer->write_available() && !pause
		? std::chrono::steady_clock::duration::zero()
		: std::chrono::milliseconds(MPD_OSX_BUFFER_TIME_MS / 4);
}

bool OSXOutput::Pause()
{
	pause = true;

	if (started) {
		AudioOutputUnitStop(au);
		started = false;
	}

	return true;
}

void
OSXOutput::Cancel() noexcept
{
	if (started) {
		AudioOutputUnitStop(au);
		started = false;
	}

	ring_buffer->reset();
#ifdef ENABLE_DSD
	pcm_export->Reset();
#endif

	/* the AudioUnit will be restarted by the next Play() call */
}

int
osx_output_get_volume(OSXOutput &output)
{
	return output.GetVolume();
}

void
osx_output_set_volume(OSXOutput &output, unsigned new_volume)
{
	return output.SetVolume(new_volume);
}

const struct AudioOutputPlugin osx_output_plugin = {
	"osx",
	osx_output_test_default_device,
	&OSXOutput::Create,
	&osx_mixer_plugin,
};
