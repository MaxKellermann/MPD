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

#include "Converter.hxx"
#include "util/AllocatedString.hxx"
#include "util/AllocatedArray.hxx"
#include "util/FormatString.hxx"
#include "config.h"

#include <iterator>
#include <stdexcept>

#include <string.h>

#ifdef HAVE_ICU
#include "Util.hxx"
#include <unicode/ucnv.h>
#elif defined(HAVE_ICONV)
#include "system/Error.hxx"
#endif

#ifdef HAVE_ICU

IcuConverter::~IcuConverter()
{
	ucnv_close(converter);
}

#endif

#ifdef HAVE_ICU_CONVERTER

std::unique_ptr<IcuConverter>
IcuConverter::Create(const char *charset)
{
#ifdef HAVE_ICU
	UErrorCode code = U_ZERO_ERROR;
	UConverter *converter = ucnv_open(charset, &code);
	if (converter == nullptr)
		throw std::runtime_error(FormatString("Failed to initialize charset '%s': %s",
						      charset, u_errorName(code)).c_str());

	return std::unique_ptr<IcuConverter>(new IcuConverter(converter));
#elif defined(HAVE_ICONV)
	iconv_t to = iconv_open("utf-8", charset);
	iconv_t from = iconv_open(charset, "utf-8");
	if (to == (iconv_t)-1 || from == (iconv_t)-1) {
		int e = errno;
		if (to != (iconv_t)-1)
			iconv_close(to);
		if (from != (iconv_t)-1)
			iconv_close(from);
		throw FormatErrno(e, "Failed to initialize charset '%s'",
				  charset);
	}

	return std::unique_ptr<IcuConverter>(new IcuConverter(to, from));
#endif
}

#ifdef HAVE_ICU
#elif defined(HAVE_ICONV)

static AllocatedString<char>
DoConvert(iconv_t conv, std::string_view src)
{
	// TODO: dynamic buffer?
	char buffer[4096];
	char *in = const_cast<char *>(src.data());
	char *out = buffer;
	size_t in_left = src.size();
	size_t out_left = sizeof(buffer);

	size_t n = iconv(conv, &in, &in_left, &out, &out_left);

	if (n == size_t(-1))
		throw MakeErrno("Charset conversion failed");

	if (in_left > 0)
		throw std::runtime_error("Charset conversion failed");

	return AllocatedString<>::Duplicate({buffer, sizeof(buffer) - out_left});
}

#endif

AllocatedString<char>
IcuConverter::ToUTF8(std::string_view s) const
{
#ifdef HAVE_ICU
	const std::lock_guard<Mutex> protect(mutex);

	ucnv_resetToUnicode(converter);

	// TODO: dynamic buffer?
	UChar buffer[4096], *target = buffer;
	const char *source = s.data();

	UErrorCode code = U_ZERO_ERROR;

	ucnv_toUnicode(converter, &target, buffer + std::size(buffer),
		       &source, source + s.size(),
		       nullptr, true, &code);
	if (code != U_ZERO_ERROR)
		throw std::runtime_error(FormatString("Failed to convert to Unicode: %s",
						      u_errorName(code)).c_str());

	const size_t target_length = target - buffer;
	return UCharToUTF8({buffer, target_length});
#elif defined(HAVE_ICONV)
	return DoConvert(to_utf8, s);
#endif
}

AllocatedString<char>
IcuConverter::FromUTF8(std::string_view s) const
{
#ifdef HAVE_ICU
	const std::lock_guard<Mutex> protect(mutex);

	const auto u = UCharFromUTF8(s);

	ucnv_resetFromUnicode(converter);

	// TODO: dynamic buffer?
	char buffer[4096], *target = buffer;
	const UChar *source = u.begin();
	UErrorCode code = U_ZERO_ERROR;

	ucnv_fromUnicode(converter, &target, buffer + std::size(buffer),
			 &source, u.end(),
			 nullptr, true, &code);

	if (code != U_ZERO_ERROR)
		throw std::runtime_error(FormatString("Failed to convert from Unicode: %s",
						      u_errorName(code)).c_str());

	return AllocatedString<>::Duplicate({buffer, size_t(target - buffer)});

#elif defined(HAVE_ICONV)
	return DoConvert(from_utf8, s);
#endif
}

#endif
