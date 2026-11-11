// SPDX-License-Identifier: Apache-2.0
// Blocking TCP client sockets for talking to the bridge on this machine.
//
// The implementation is per operating system and includes no Cinema 4D headers, because
// the system headers clash with them. For the same reason this header uses only plain C++ types.

#ifndef CYCLESRENDER_PLATFORM_SOCKET_H__
#define CYCLESRENDER_PLATFORM_SOCKET_H__

namespace cyclesrender
{
namespace platform
{

using SocketHandle = unsigned long long;
static const SocketHandle NO_SOCKET = ~0ull;

/// Connects to a TCP port on this machine. Returns NO_SOCKET on failure.
SocketHandle SocketConnectLocal(unsigned short port);

/// Sends all bytes. Returns false when the connection failed.
bool SocketSend(SocketHandle socket, const void* data, long long size);

/// Receives exactly @p size bytes. Returns false when the connection closed or failed first.
bool SocketReceive(SocketHandle socket, void* data, long long size);

/// Waits until there is something to receive. Returns true when SocketReceive will not block
/// (data arrived or the connection closed) and false when the time ran out first.
bool SocketWaitReadable(SocketHandle socket, int timeoutMilliseconds);

/// Closes the socket. Safe to call with NO_SOCKET.
void SocketClose(SocketHandle socket);

} // namespace platform
} // namespace cyclesrender

#endif // CYCLESRENDER_PLATFORM_SOCKET_H__
