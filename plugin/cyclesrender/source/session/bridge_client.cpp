// SPDX-License-Identifier: Apache-2.0
// The connection to one Blender (bridge_client.h): open, request and reply, shut down.

#include "c4d.h"
#include "maxon/atomictypes.h"

#include "../version.h"
#include "bridge_client.h"
#include "capture.h"
#include "json.h"

namespace cyclesrender
{

// The plugin and the bridge are installed separately, so one can be updated without the
// other. That is harmless while both speak the same protocol version (the bridge refuses the
// hello otherwise), but worth a line in the console: a feature of the newer part may be missing.
static void NoteBridgeVersion(const maxon::DataDictionary& hello)
{
	// Clients are opened on render and worker threads. Should two of them get here at the same
	// moment the line appears twice, which does no harm.
	static maxon::AtomicBool noted;
	const maxon::String bridge = GetJsonString(hello, "bridge"_s);
	if (noted.Get() || bridge.IsEmpty() || bridge == maxon::String(CYCLESRENDER_VERSION))
		return;
	noted.Set(true);
	ApplicationOutput("Cycles Render: the bridge installed in Blender is version @, this plugin is version @. "
		"They work together, but installing both from the same release is recommended.", bridge, maxon::String(CYCLESRENDER_VERSION));
}

BridgeClient::~BridgeClient()
{
	platform::SocketClose(_socket);
	_socket = platform::NO_SOCKET;
}

maxon::Result<void> BridgeClient::Open(BRIDGEMODE mode)
{
	iferr_scope;

	_process.Start(mode) iferr_return;

	_socket = platform::SocketConnectLocal(static_cast<unsigned short>(_process.GetPort()));
	if (_socket == platform::NO_SOCKET)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "Could not connect to the bridge in Blender."_s);

	maxon::DataDictionary hello;
	hello.Set("op"_s, "hello"_s) iferr_return;
	hello.Set("token"_s, _process.GetToken()) iferr_return;
	hello.Set("protocol"_s, maxon::Int64(PROTOCOL_VERSION)) iferr_return;
	hello.Set("client"_s, "cyclesrender"_s) iferr_return;
	BridgeMessage reply = Request(std::move(hello)) iferr_return;
	_hello = reply.header;
	NoteBridgeVersion(_hello);
	return maxon::OK;
}

maxon::Result<BridgeMessage> BridgeClient::Request(maxon::DataDictionary header, const BlobViews& blobs)
{
	iferr_scope;

	if (_socket == platform::NO_SOCKET)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "Not connected to Blender."_s);

	const maxon::Int64 id = _nextId++;
	header.Set("id"_s, id) iferr_return;
	WriteFrame(_socket, header, blobs) iferr_return;
	return AwaitReply(id, nullptr);
}

maxon::Result<BridgeMessage> BridgeClient::Request(JsonWriter& header, const BlobViews& blobs, cinema::BaseThread* cancel)
{
	iferr_scope;

	if (_socket == platform::NO_SOCKET)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "Not connected to Blender."_s);

	const maxon::Int64 id = _nextId++;
	header.Int("id", id);
	header.EndObject();
	if (IsCapturing())
	{
		maxon::String lengths;
		for (const maxon::Block<const maxon::Char>& blob : blobs)
			lengths += (lengths.IsEmpty() ? ""_s : ","_s) + maxon::String::IntToString(blob.GetCount());
		Capture("request", header.GetText() + " blobs=["_s + lengths + "]"_s);
	}
	WriteFrameText(_socket, header.GetText(), blobs) iferr_return;
	BridgeMessage reply = AwaitReply(id, cancel) iferr_return;
	if (IsCapturing())
	{
		const maxon::Result<maxon::String> text = WriteJson(reply.header);
		Capture("reply", text == maxon::OK ? text.GetValue() : "(could not be written as text)"_s);
	}
	return std::move(reply);
}

maxon::Result<BridgeMessage> BridgeClient::AwaitReply(maxon::Int64 id, cinema::BaseThread* cancel)
{
	iferr_scope;

	while (true)
	{
		if (cancel)
		{
			// Wake up regularly so a cancelled render does not wait for Blender to finish.
			while (!platform::SocketWaitReadable(_socket, 100))
			{
				if (cancel->TestBreak())
					return maxon::OperationCancelledError(MAXON_SOURCE_LOCATION);
			}
		}

		BridgeMessage message = ReadFrame(_socket) iferr_return;
		if (GetJsonString(message.header, "op"_s) != "reply"_s || GetJsonInt(message.header, "id"_s, -1) != id)
			continue; // An event, or a reply to something else: not handled yet.

		if (!GetJsonBool(message.header, "ok"_s, false))
		{
			maxon::String code = "unknown"_s;
			maxon::String text;
			maxon::Result<maxon::DataDictionary> error = message.header.Get<maxon::DataDictionary>("error"_s);
			if (error == maxon::OK)
			{
				code = GetJsonString(error.GetValue(), "code"_s);
				text = GetJsonString(error.GetValue(), "message"_s);
			}
			return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, FormatString("Blender bridge error (@): @", code, text));
		}
		return std::move(message);
	}
}

maxon::Result<maxon::Int> BridgeClient::Shutdown()
{
	iferr_scope;

	maxon::DataDictionary request;
	request.Set("op"_s, "shutdown"_s) iferr_return;
	Request(std::move(request)) iferr_return;

	platform::SocketClose(_socket);
	_socket = platform::NO_SOCKET;
	return _process.WaitForExit(20.0);
}

} // namespace cyclesrender
