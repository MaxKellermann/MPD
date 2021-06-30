/*
 * Copyright 2012-2021 Max Kellermann <max.kellermann@gmail.com>
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

#ifndef SOCKET_DESCRIPTOR_HXX
#define SOCKET_DESCRIPTOR_HXX

#include "Features.hxx"
#include "io/FileDescriptor.hxx"

#include <type_traits>

class SocketAddress;
class StaticSocketAddress;
class IPv4Address;
class IPv6Address;

/**
 * An OO wrapper for a UNIX socket descriptor.
 */
class SocketDescriptor : protected FileDescriptor {
protected:
	explicit constexpr SocketDescriptor(FileDescriptor _fd) noexcept
		:FileDescriptor(_fd) {}

public:
	SocketDescriptor() = default;

	explicit constexpr SocketDescriptor(int _fd) noexcept
		:FileDescriptor(_fd) {}

	constexpr bool operator==(SocketDescriptor other) const noexcept {
		return fd == other.fd;
	}

#ifndef _WIN32
	/**
	 * Convert a #FileDescriptor to a #SocketDescriptor.  This is only
	 * possible on operating systems where socket descriptors are the
	 * same as file descriptors (i.e. not on Windows).  Use this only
	 * when you know what you're doing.
	 */
	static constexpr SocketDescriptor FromFileDescriptor(FileDescriptor fd) noexcept {
		return SocketDescriptor(fd);
	}

	/**
	 * Convert this object to a #FileDescriptor instance.  This is only
	 * possible on operating systems where socket descriptors are the
	 * same as file descriptors (i.e. not on Windows).  Use this only
	 * when you know what you're doing.
	 */
	constexpr const FileDescriptor &ToFileDescriptor() const noexcept {
		return *this;
	}
#endif

	using FileDescriptor::IsDefined;
#ifndef _WIN32
	using FileDescriptor::IsValid;
	using FileDescriptor::IsSocket;
#endif

	/**
	 * Determine the socket type, i.e. SOCK_STREAM, SOCK_DGRAM or
	 * SOCK_SEQPACKET.  Returns -1 on error.
	 */
	[[gnu::pure]]
	int GetType() const noexcept;

	/**
	 * Is this a stream socket?
	 */
	[[gnu::pure]]
	bool IsStream() const noexcept;

	using FileDescriptor::Get;
	using FileDescriptor::Set;
	using FileDescriptor::Steal;
	using FileDescriptor::SetUndefined;

	static constexpr SocketDescriptor Undefined() noexcept {
		return SocketDescriptor(FileDescriptor::Undefined());
	}

	using FileDescriptor::EnableCloseOnExec;
	using FileDescriptor::DisableCloseOnExec;

#ifndef _WIN32
	using FileDescriptor::SetNonBlocking;
	using FileDescriptor::SetBlocking;
	using FileDescriptor::Duplicate;
	using FileDescriptor::CheckDuplicate;
	using FileDescriptor::Close;
#else
	bool SetNonBlocking() noexcept;

	/**
	 * This method replaces FileDescriptor::Close(), using closesocket()
	 * on Windows.  FileDescriptor::Close() is not virtual, so be
	 * careful when dealing with a FileDescriptor reference that is
	 * really a SocketDescriptor.
	 */
	void Close() noexcept;
#endif

	/**
	 * Create a socket.
	 *
	 * @param domain is the address domain
	 * @param type is the sochet type
	 * @param protocol is the protocol
	 * @return True on success, False on failure
	 * See man 2 socket for detailed information
	 */
	bool Create(int domain, int type, int protocol) noexcept;

	/**
	 * Like Create(), but enable non-blocking mode.
	 */
	bool CreateNonBlock(int domain, int type, int protocol) noexcept;

#ifndef _WIN32
	static bool CreateSocketPair(int domain, int type, int protocol,
				     SocketDescriptor &a,
				     SocketDescriptor &b) noexcept;
	static bool CreateSocketPairNonBlock(int domain, int type, int protocol,
					     SocketDescriptor &a,
					     SocketDescriptor &b) noexcept;
#endif

	int GetError() noexcept;

	/**
	 * @return the value size or 0 on error
	 */
	std::size_t GetOption(int level, int name,
			      void *value, std::size_t size) const noexcept;

#ifdef HAVE_STRUCT_UCRED
	/**
	 * Receive peer credentials (SO_PEERCRED).  On error, the pid
	 * is -1.
	 */
	[[gnu::pure]]
	struct ucred GetPeerCredentials() const noexcept;
#endif

	bool SetOption(int level, int name,
		       const void *value, std::size_t size) noexcept;

	bool SetBoolOption(int level, int name, bool _value) noexcept {
		const int value = _value;
		return SetOption(level, name, &value, sizeof(value));
	}

	bool SetKeepAlive(bool value=true) noexcept;
	bool SetReuseAddress(bool value=true) noexcept;

#ifdef __linux__
	bool SetReusePort(bool value=true) noexcept;
	bool SetFreeBind(bool value=true) noexcept;
	bool SetNoDelay(bool value=true) noexcept;
	bool SetCork(bool value=true) noexcept;

	bool SetTcpDeferAccept(const int &seconds) noexcept;

	/**
	 * Setter for TCP_USER_TIMEOUT.
	 */
	bool SetTcpUserTimeout(const unsigned &milliseconds) noexcept;

	/**
	 * Setter for SO_BINDTODEVICE.
	 */
	bool SetBindToDevice(const char *name) noexcept;

	bool SetTcpFastOpen(int qlen=16) noexcept;

	bool AddMembership(const IPv4Address &address) noexcept;
	bool AddMembership(const IPv6Address &address) noexcept;
	bool AddMembership(SocketAddress address) noexcept;
#endif

	bool SetV6Only(bool value) noexcept;

	bool Bind(SocketAddress address) noexcept;

#ifdef __linux__
	/**
	 * Binds the socket to a unique abstract address.
	 */
	bool AutoBind() noexcept;
#endif

	bool Listen(int backlog) noexcept;

	SocketDescriptor Accept() noexcept;
	SocketDescriptor AcceptNonBlock() const noexcept;
	SocketDescriptor AcceptNonBlock(StaticSocketAddress &address) const noexcept;

	bool Connect(SocketAddress address) noexcept;

	[[gnu::pure]]
	StaticSocketAddress GetLocalAddress() const noexcept;

	[[gnu::pure]]
	StaticSocketAddress GetPeerAddress() const noexcept;

	ssize_t Read(void *buffer, std::size_t length) noexcept;
	ssize_t Write(const void *buffer, std::size_t length) noexcept;

#ifdef _WIN32
	int WaitReadable(int timeout_ms) const noexcept;
	int WaitWritable(int timeout_ms) const noexcept;
#else
	using FileDescriptor::WaitReadable;
	using FileDescriptor::WaitWritable;
	using FileDescriptor::IsReadyForWriting;
#endif

	/**
	 * Receive a datagram and return the source address.
	 */
	ssize_t Read(void *buffer, std::size_t length,
		     StaticSocketAddress &address) noexcept;

	/**
	 * Send a datagram to the specified address.
	 */
	ssize_t Write(const void *buffer, std::size_t length,
		      SocketAddress address) noexcept;

#ifndef _WIN32
	void Shutdown() noexcept;
	void ShutdownRead() noexcept;
	void ShutdownWrite() noexcept;
#endif
};

static_assert(std::is_trivial<SocketDescriptor>::value, "type is not trivial");

#endif
