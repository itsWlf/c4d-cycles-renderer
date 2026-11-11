// SPDX-License-Identifier: Apache-2.0
// Windows implementation of platform/socket.h. Includes no Cinema 4D headers on purpose.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include "socket.h"

#pragma comment(lib, "ws2_32.lib")

namespace cyclesrender
{
namespace platform
{

static bool EnsureWinsock()
{
	static bool started = false;
	if (!started)
	{
		WSADATA data;
		started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}
	return started;
}

SocketHandle SocketConnectLocal(unsigned short port)
{
	if (!EnsureWinsock())
		return NO_SOCKET;

	const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (handle == INVALID_SOCKET)
		return NO_SOCKET;

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (::connect(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
	{
		::closesocket(handle);
		return NO_SOCKET;
	}

	// Requests are small and latency matters more than packet count.
	const BOOL noDelay = TRUE;
	::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
	return static_cast<SocketHandle>(handle);
}

bool SocketSend(SocketHandle socket, const void* data, long long size)
{
	const char* position = static_cast<const char*>(data);
	while (size > 0)
	{
		const int chunk = size > (1 << 24) ? (1 << 24) : static_cast<int>(size);
		const int sent = ::send(static_cast<SOCKET>(socket), position, chunk, 0);
		if (sent <= 0)
			return false;
		position += sent;
		size -= sent;
	}
	return true;
}

bool SocketReceive(SocketHandle socket, void* data, long long size)
{
	char* position = static_cast<char*>(data);
	while (size > 0)
	{
		const int chunk = size > (1 << 24) ? (1 << 24) : static_cast<int>(size);
		const int received = ::recv(static_cast<SOCKET>(socket), position, chunk, 0);
		if (received <= 0)
			return false;
		position += received;
		size -= received;
	}
	return true;
}

bool SocketWaitReadable(SocketHandle socket, int timeoutMilliseconds)
{
	fd_set readable;
	FD_ZERO(&readable);
	FD_SET(static_cast<SOCKET>(socket), &readable);
	timeval timeout;
	timeout.tv_sec = timeoutMilliseconds / 1000;
	timeout.tv_usec = (timeoutMilliseconds % 1000) * 1000;
	// An error also counts as "will not block": the following receive reports it.
	return ::select(0, &readable, nullptr, nullptr, &timeout) != 0;
}

void SocketClose(SocketHandle socket)
{
	if (socket == NO_SOCKET)
		return;
	::shutdown(static_cast<SOCKET>(socket), SD_BOTH);
	::closesocket(static_cast<SOCKET>(socket));
}

} // namespace platform
} // namespace cyclesrender

#endif // _WIN32
