// SPDX-License-Identifier: Apache-2.0
// The plugin's connection to one Blender: launch, authenticate, request/reply, shut down.

#ifndef CYCLESRENDER_BRIDGE_CLIENT_H__
#define CYCLESRENDER_BRIDGE_CLIENT_H__

#include "c4d.h"

#include "blender_process.h"
#include "json_writer.h"
#include "protocol.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// Owns one Blender process and the socket to its bridge. Requests are synchronous: the
/// calling thread blocks until the reply has arrived, so use it from a worker or render
/// thread. Destroying the client kills Blender if Shutdown() was not called, which is also
/// how a running render is cancelled.
//----------------------------------------------------------------------------------------
class BridgeClient
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(BridgeClient);

public:
	BridgeClient() = default;
	~BridgeClient();

	/// Starts Blender, connects and says hello.
	maxon::Result<void> Open(BRIDGEMODE mode);

	/// Sends one request and waits for its reply. An error reply from the bridge is returned
	/// as an error carrying the bridge's message.
	maxon::Result<BridgeMessage> Request(maxon::DataDictionary header, const BlobViews& blobs = BlobViews());

	/// The same for a header written with JsonWriter. Pass the writer with its top-level
	/// object still open: the request id is added and the object closed here.
	/// When @p cancel is given it is tested while waiting, and a cancelled wait returns
	/// maxon::OperationCancelledError; the connection is unusable afterwards.
	maxon::Result<BridgeMessage> Request(JsonWriter& header, const BlobViews& blobs = BlobViews(), cinema::BaseThread* cancel = nullptr);

	/// Asks the bridge to quit, waits for Blender to exit and returns its exit code.
	maxon::Result<maxon::Int> Shutdown();

	/// What the bridge reported about itself in its hello reply (version, mode, devices).
	const maxon::DataDictionary& GetHello() const { return _hello; }

	/// The secret of this launch; it also names the shared-memory frame block.
	const maxon::String& GetToken() const { return _process.GetToken(); }

	/// The process id of Blender, 0 before Open().
	maxon::Int GetProcessId() const { return _process.GetProcessId(); }

	/// False once Blender has exited, whatever the reason.
	maxon::Bool IsAlive() const { return _process.IsRunning(); }

	/// Ends Blender at once without closing the session, as a crash would. For tests.
	void KillBlender() { _process.Kill(); }

private:
	maxon::Result<BridgeMessage> AwaitReply(maxon::Int64 id, cinema::BaseThread* cancel);

	BlenderProcess _process;
	platform::SocketHandle _socket = platform::NO_SOCKET;
	maxon::Int64 _nextId = 1;
	maxon::DataDictionary _hello;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_BRIDGE_CLIENT_H__
