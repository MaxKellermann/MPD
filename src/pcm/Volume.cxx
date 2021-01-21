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

#include "Volume.hxx"
#include "Silence.hxx"
#include "Traits.hxx"
#include "util/ConstBuffer.hxx"
#include "util/WritableBuffer.hxx"
#include "util/RuntimeError.hxx"
#include "util/TransformN.hxx"

#include "Dither.cxx" // including the .cxx file to get inlined templates

#include <cassert>
#include <cstdint>

#include <string.h>

#if GCC_OLDER_THAN(8,0)
/* GCC 6.3 emits this bogus warning in PcmVolumeConvert() because it
   checks an unreachable branch */
#pragma GCC diagnostic ignored "-Wshift-count-overflow"
#endif

/**
 * Apply software volume, converting to a different sample type.
 */
template<SampleFormat SF, SampleFormat DF,
	 class STraits=SampleTraits<SF>,
	 class DTraits=SampleTraits<DF>>
#if !GCC_OLDER_THAN(8,0)
constexpr
#endif
static typename DTraits::value_type
PcmVolumeConvert(typename STraits::value_type _sample, int volume) noexcept
{
	typename STraits::long_type sample(_sample);
	sample *= volume;

	static_assert(DTraits::BITS > STraits::BITS,
		      "Destination sample must be larger than source sample");

	/* after multiplying with the volume value, the "sample"
	   variable contains this number of precision bits: source
	   bits plus the volume bits */
	constexpr unsigned BITS = STraits::BITS + PCM_VOLUME_BITS;

	/* .. and now we need to scale to the requested destination
	   bits */

	typename DTraits::value_type result;
	if (BITS > DTraits::BITS)
		result = sample >> (BITS - DTraits::BITS);
	else if (BITS < DTraits::BITS)
		result = sample << (DTraits::BITS - BITS);
	else
		result = sample;

	return result;
}

template<SampleFormat F, class Traits=SampleTraits<F>>
static inline typename Traits::value_type
pcm_volume_sample(PcmDither &dither,
		  typename Traits::value_type _sample,
		  int volume) noexcept
{
	typename Traits::long_type sample(_sample);

	return dither.DitherShift<typename Traits::long_type,
				  Traits::BITS + PCM_VOLUME_BITS,
				  Traits::BITS>(sample * volume);
}

template<SampleFormat F, class Traits=SampleTraits<F>>
static void
pcm_volume_change(PcmDither &dither,
		  typename Traits::pointer dest,
		  typename Traits::const_pointer src,
		  size_t n,
		  int volume) noexcept
{
	transform_n(src, n, dest,
		    [&dither, volume](auto x){
			    return pcm_volume_sample<F, Traits>(dither, x,
								volume);
		    });
}

static void
pcm_volume_change_8(PcmDither &dither,
		    int8_t *dest, const int8_t *src, size_t n,
		    int volume) noexcept
{
	pcm_volume_change<SampleFormat::S8>(dither, dest, src, n, volume);
}

static void
pcm_volume_change_16(PcmDither &dither,
		     int16_t *dest, const int16_t *src, size_t n,
		     int volume) noexcept
{
	pcm_volume_change<SampleFormat::S16>(dither, dest, src, n, volume);
}

static void
PcmVolumeChange16to32(int32_t *dest, const int16_t *src, size_t n,
		      int volume) noexcept
{
	transform_n(src, n, dest,
		    [volume](auto x){
			    return PcmVolumeConvert<SampleFormat::S16,
						    SampleFormat::S24_P32>(x,
									   volume);
		    });
}

static void
pcm_volume_change_24(PcmDither &dither,
		     int32_t *dest, const int32_t *src, size_t n,
		     int volume) noexcept
{
	pcm_volume_change<SampleFormat::S24_P32>(dither, dest, src, n,
						 volume);
}

static void
pcm_volume_change_32(PcmDither &dither,
		     int32_t *dest, const int32_t *src, size_t n,
		     int volume) noexcept
{
	pcm_volume_change<SampleFormat::S32>(dither, dest, src, n, volume);
}

static void
pcm_volume_change_float(float *dest, const float *src, size_t n,
			float volume) noexcept
{
	transform_n(src, n, dest,
		    [volume](float x){ return x * volume; });
}

SampleFormat
PcmVolume::Open(SampleFormat _format, bool allow_convert)
{
	assert(format == SampleFormat::UNDEFINED);

	convert = false;

	switch (_format) {
	case SampleFormat::UNDEFINED:
		throw FormatRuntimeError("Software volume for %s is not implemented",
					 sample_format_to_string(_format));

	case SampleFormat::S8:
		break;

	case SampleFormat::S16:
		if (allow_convert) {
			/* convert S16 to S24 to avoid discarding too
			   many bits of precision in this stage */
			format = _format;
			convert = true;
			return SampleFormat::S24_P32;
		}

		break;

	case SampleFormat::S24_P32:
	case SampleFormat::S32:
	case SampleFormat::FLOAT:
		break;

	case SampleFormat::DSD:
		// TODO: implement this; currently, it's a no-op
		break;
	}

	return format = _format;
}

ConstBuffer<void>
PcmVolume::Apply(ConstBuffer<void> src) noexcept
{
	if (volume == PCM_VOLUME_1 && !convert)
		return src;

	size_t dest_size = src.size;
	if (convert) {
		assert(format == SampleFormat::S16);

		/* converting to S24_P32 */
		dest_size *= 2;
	}

	void *data = buffer.Get(dest_size);

	if (volume == 0) {
		/* optimized special case: 0% volume = memset(0) */
		PcmSilence({data, dest_size}, format);
		return { data, dest_size };
	}

	switch (format) {
	case SampleFormat::UNDEFINED:
		assert(false);
		gcc_unreachable();

	case SampleFormat::S8:
		pcm_volume_change_8(dither, static_cast<int8_t *>(data),
				    static_cast<const int8_t *>(src.data),
				    src.size / sizeof(int8_t),
				    volume);
		break;

	case SampleFormat::S16:
		if (convert)
			PcmVolumeChange16to32(static_cast<int32_t *>(data),
					      static_cast<const int16_t *>(src.data),
					      src.size / sizeof(int16_t),
					      volume);
		else
			pcm_volume_change_16(dither, static_cast<int16_t *>(data),
					     static_cast<const int16_t *>(src.data),
					     src.size / sizeof(int16_t),
					     volume);
		break;

	case SampleFormat::S24_P32:
		pcm_volume_change_24(dither, static_cast<int32_t *>(data),
				     static_cast<const int32_t *>(src.data),
				     src.size / sizeof(int32_t),
				     volume);
		break;

	case SampleFormat::S32:
		pcm_volume_change_32(dither, static_cast<int32_t *>(data),
				     static_cast<const int32_t *>(src.data),
				     src.size / sizeof(int32_t),
				     volume);
		break;

	case SampleFormat::FLOAT:
		pcm_volume_change_float(static_cast<float *>(data),
					static_cast<const float *>(src.data),
					src.size / sizeof(float),
					pcm_volume_to_float(volume));
		break;

	case SampleFormat::DSD:
		// TODO: implement this; currently, it's a no-op
		return src;
	}

	return { data, dest_size };
}
