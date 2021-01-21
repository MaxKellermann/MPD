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

#include "Mix.hxx"
#include "Volume.hxx"
#include "Clamp.hxx"
#include "Traits.hxx"
#include "util/Clamp.hxx"
#include "util/Math.hxx"

#include "Dither.cxx" // including the .cxx file to get inlined templates

#include <cassert>
#include <cmath>

template<SampleFormat F, class Traits=SampleTraits<F>>
static typename Traits::value_type
PcmAddVolume(PcmDither &dither,
	     typename Traits::value_type _a, typename Traits::value_type _b,
	     int volume1, int volume2) noexcept
{
	typename Traits::long_type a(_a), b(_b);
	typename Traits::long_type c(a * volume1 + b * volume2);

	return dither.DitherShift<typename Traits::long_type,
				  Traits::BITS + PCM_VOLUME_BITS,
				  Traits::BITS>(c);
}

template<SampleFormat F, class Traits=SampleTraits<F>>
static void
PcmAddVolume(PcmDither &dither,
	     typename Traits::pointer a,
	     typename Traits::const_pointer b,
	     size_t n, int volume1, int volume2) noexcept
{
	for (size_t i = 0; i != n; ++i)
		a[i] = PcmAddVolume<F, Traits>(dither, a[i], b[i],
					       volume1, volume2);
}

template<SampleFormat F, class Traits=SampleTraits<F>>
static void
PcmAddVolumeVoid(PcmDither &dither,
		 void *a, const void *b, size_t size,
		 int volume1, int volume2) noexcept
{
	constexpr size_t sample_size = Traits::SAMPLE_SIZE;
	assert(size % sample_size == 0);

	PcmAddVolume<F, Traits>(dither,
				typename Traits::pointer(a),
				typename Traits::const_pointer(b),
				size / sample_size,
				volume1, volume2);
}

static void
pcm_add_vol_float(float *buffer1, const float *buffer2,
		  unsigned num_samples, float volume1, float volume2) noexcept
{
	while (num_samples > 0) {
		float sample1 = *buffer1;
		float sample2 = *buffer2++;

		sample1 = (sample1 * volume1 + sample2 * volume2);
		*buffer1++ = sample1;
		--num_samples;
	}
}

static bool
pcm_add_vol(PcmDither &dither, void *buffer1, const void *buffer2, size_t size,
	    int vol1, int vol2,
	    SampleFormat format) noexcept
{
	switch (format) {
	case SampleFormat::UNDEFINED:
	case SampleFormat::DSD:
		/* not implemented */
		return false;

	case SampleFormat::S8:
		PcmAddVolumeVoid<SampleFormat::S8>(dither,
						   buffer1, buffer2, size,
						   vol1, vol2);
		return true;

	case SampleFormat::S16:
		PcmAddVolumeVoid<SampleFormat::S16>(dither,
						    buffer1, buffer2, size,
						    vol1, vol2);
		return true;

	case SampleFormat::S24_P32:
		PcmAddVolumeVoid<SampleFormat::S24_P32>(dither,
							buffer1, buffer2, size,
							vol1, vol2);
		return true;

	case SampleFormat::S32:
		PcmAddVolumeVoid<SampleFormat::S32>(dither,
						    buffer1, buffer2, size,
						    vol1, vol2);
		return true;

	case SampleFormat::FLOAT:
		pcm_add_vol_float(static_cast<float *>(buffer1), static_cast<const float *>(buffer2),
				  size / 4,
				  pcm_volume_to_float(vol1),
				  pcm_volume_to_float(vol2));
		return true;
	}

	assert(false);
	gcc_unreachable();
}

template<SampleFormat F, class Traits=SampleTraits<F>>
static constexpr typename Traits::value_type
PcmAdd(typename Traits::value_type _a, typename Traits::value_type _b) noexcept
{
	typename Traits::sum_type a(_a), b(_b);

	return PcmClamp<F, Traits>(a + b);
}

template<SampleFormat F, class Traits=SampleTraits<F>>
static void
PcmAdd(typename Traits::pointer a,
       typename Traits::const_pointer b,
       size_t n) noexcept
{
	for (size_t i = 0; i != n; ++i)
		a[i] = PcmAdd<F, Traits>(a[i], b[i]);
}

template<SampleFormat F, class Traits=SampleTraits<F>>
static void
PcmAddVoid(void *a, const void *b, size_t size) noexcept
{
	constexpr size_t sample_size = Traits::SAMPLE_SIZE;
	assert(size % sample_size == 0);

	PcmAdd<F, Traits>(typename Traits::pointer(a),
			  typename Traits::const_pointer(b),
			  size / sample_size);
}

static void
pcm_add_float(float *buffer1, const float *buffer2,
	      unsigned num_samples) noexcept
{
	while (num_samples > 0) {
		float sample1 = *buffer1;
		float sample2 = *buffer2++;
		*buffer1++ = sample1 + sample2;
		--num_samples;
	}
}

static bool
pcm_add(void *buffer1, const void *buffer2, size_t size,
	SampleFormat format) noexcept
{
	switch (format) {
	case SampleFormat::UNDEFINED:
	case SampleFormat::DSD:
		/* not implemented */
		return false;

	case SampleFormat::S8:
		PcmAddVoid<SampleFormat::S8>(buffer1, buffer2, size);
		return true;

	case SampleFormat::S16:
		PcmAddVoid<SampleFormat::S16>(buffer1, buffer2, size);
		return true;

	case SampleFormat::S24_P32:
		PcmAddVoid<SampleFormat::S24_P32>(buffer1, buffer2, size);
		return true;

	case SampleFormat::S32:
		PcmAddVoid<SampleFormat::S32>(buffer1, buffer2, size);
		return true;

	case SampleFormat::FLOAT:
		pcm_add_float(static_cast<float *>(buffer1), static_cast<const float *>(buffer2),
			      size / 4);
		return true;
	}

	assert(false);
	gcc_unreachable();
}

bool
pcm_mix(PcmDither &dither, void *buffer1, const void *buffer2, size_t size,
	SampleFormat format, float portion1) noexcept
{
	float s;

	/* portion1 is between 0.0 and 1.0 for crossfading, MixRamp uses -1
	 * to signal mixing rather than fading */
	if (portion1 < 0)
		return pcm_add(buffer1, buffer2, size, format);

	s = std::sin(static_cast<float>(M_PI_2) * portion1);
	s *= s;

	int vol1 = lround(s * PCM_VOLUME_1S);
	vol1 = Clamp<int>(vol1, 0, PCM_VOLUME_1S);

	return pcm_add_vol(dither, buffer1, buffer2, size,
			   vol1, PCM_VOLUME_1S - vol1, format);
}
