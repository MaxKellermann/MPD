/*
 * Copyright (C) 2011-2015 Max Kellermann <max.kellermann@gmail.com>,
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef BYTE_ORDER_HXX
#define BYTE_ORDER_HXX

#include "Compiler.h"

#include <cstdint>

#if defined(__i386__) || defined(__x86_64__) || defined(__ARMEL__)
/* well-known little-endian */
#  define IS_LITTLE_ENDIAN true
#  define IS_BIG_ENDIAN false
#elif defined(__MIPSEB__)
/* well-known big-endian */
#  define IS_LITTLE_ENDIAN false
#  define IS_BIG_ENDIAN true
#elif defined(__APPLE__) || defined(__NetBSD__)
/* compile-time check for MacOS */
#  include <machine/endian.h>
#  if BYTE_ORDER == LITTLE_ENDIAN
#    define IS_LITTLE_ENDIAN true
#    define IS_BIG_ENDIAN false
#  else
#    define IS_LITTLE_ENDIAN false
#    define IS_BIG_ENDIAN true
#  endif
#elif defined(__BYTE_ORDER__)
/* GCC-specific macros */
#  if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#    define IS_LITTLE_ENDIAN true
#    define IS_BIG_ENDIAN false
#  else
#    define IS_LITTLE_ENDIAN false
#    define IS_BIG_ENDIAN true
#  endif
#else
/* generic compile-time check */
#  include <endian.h>
#  if __BYTE_ORDER == __LITTLE_ENDIAN
#    define IS_LITTLE_ENDIAN true
#    define IS_BIG_ENDIAN false
#  else
#    define IS_LITTLE_ENDIAN false
#    define IS_BIG_ENDIAN true
#  endif
#endif

constexpr bool
IsLittleEndian() noexcept
{
	return IS_LITTLE_ENDIAN;
}

constexpr bool
IsBigEndian() noexcept
{
	return IS_BIG_ENDIAN;
}

constexpr uint16_t
GenericByteSwap16(uint16_t value) noexcept
{
	return (value >> 8) | (value << 8);
}

constexpr uint32_t
GenericByteSwap32(uint32_t value) noexcept
{
	return (value >> 24) | ((value >> 8) & 0x0000ff00) |
		((value << 8) & 0x00ff0000) | (value << 24);
}

constexpr uint64_t
GenericByteSwap64(uint64_t value) noexcept
{
	return uint64_t(GenericByteSwap32(uint32_t(value >> 32)))
		| (uint64_t(GenericByteSwap32(value)) << 32);
}

constexpr uint16_t
ByteSwap16(uint16_t value) noexcept
{
#if CLANG_OR_GCC_VERSION(4,8)
	return __builtin_bswap16(value);
#else
	return GenericByteSwap16(value);
#endif
}

constexpr uint32_t
ByteSwap32(uint32_t value) noexcept
{
#if CLANG_OR_GCC_VERSION(4,3)
	return __builtin_bswap32(value);
#else
	return GenericByteSwap32(value);
#endif
}

constexpr uint64_t
ByteSwap64(uint64_t value) noexcept
{
#if CLANG_OR_GCC_VERSION(4,3)
	return __builtin_bswap64(value);
#else
	return GenericByteSwap64(value);
#endif
}

/**
 * Converts a 16bit value from big endian to the system's byte order
 */
constexpr uint16_t
FromBE16(uint16_t value) noexcept
{
	return IsBigEndian() ? value : ByteSwap16(value);
}

/**
 * Converts a 32bit value from big endian to the system's byte order
 */
constexpr uint32_t
FromBE32(uint32_t value) noexcept
{
	return IsBigEndian() ? value : ByteSwap32(value);
}

/**
 * Converts a 64bit value from big endian to the system's byte order
 */
constexpr uint64_t
FromBE64(uint64_t value) noexcept
{
	return IsBigEndian() ? value : ByteSwap64(value);
}

/**
 * Converts a 16bit value from little endian to the system's byte order
 */
constexpr uint16_t
FromLE16(uint16_t value) noexcept
{
	return IsLittleEndian() ? value : ByteSwap16(value);
}

/**
 * Converts a 32bit value from little endian to the system's byte order
 */
constexpr uint32_t
FromLE32(uint32_t value) noexcept
{
	return IsLittleEndian() ? value : ByteSwap32(value);
}

/**
 * Converts a 64bit value from little endian to the system's byte order
 */
constexpr uint64_t
FromLE64(uint64_t value) noexcept
{
	return IsLittleEndian() ? value : ByteSwap64(value);
}

/**
 * Converts a 16bit value from the system's byte order to big endian
 */
constexpr uint16_t
ToBE16(uint16_t value) noexcept
{
	return IsBigEndian() ? value : ByteSwap16(value);
}

/**
 * Converts a 32bit value from the system's byte order to big endian
 */
constexpr uint32_t
ToBE32(uint32_t value) noexcept
{
	return IsBigEndian() ? value : ByteSwap32(value);
}

/**
 * Converts a 64bit value from the system's byte order to big endian
 */
constexpr uint64_t
ToBE64(uint64_t value) noexcept
{
	return IsBigEndian() ? value : ByteSwap64(value);
}

/**
 * Converts a 16bit value from the system's byte order to little endian
 */
constexpr uint16_t
ToLE16(uint16_t value) noexcept
{
	return IsLittleEndian() ? value : ByteSwap16(value);
}

/**
 * Converts a 32bit value from the system's byte order to little endian
 */
constexpr uint32_t
ToLE32(uint32_t value) noexcept
{
	return IsLittleEndian() ? value : ByteSwap32(value);
}

/**
 * Converts a 64bit value from the system's byte order to little endian
 */
constexpr uint64_t
ToLE64(uint64_t value) noexcept
{
	return IsLittleEndian() ? value : ByteSwap64(value);
}

/**
 * Converts a 16 bit integer from little endian to the host byte order
 * and returns it as a signed integer.
 */
constexpr int16_t
FromLE16S(uint16_t value) noexcept
{
	/* assuming two's complement representation */
	return int16_t(FromLE16(value));
}

#endif
