// SPDX-License-Identifier: Apache-2.0
// Frames of the bridge protocol (see protocol/protocol.md at the repository root).

#ifndef CYCLESRENDER_PROTOCOL_H__
#define CYCLESRENDER_PROTOCOL_H__

#include "c4d.h"
#include "maxon/datadictionary.h"

#include "../platform/socket.h"

namespace cyclesrender
{

static const maxon::Int32 PROTOCOL_VERSION = 0;

/// One blob of a frame: raw bytes whose meaning the header describes.
using Blob = maxon::BaseArray<maxon::Char>;

//----------------------------------------------------------------------------------------
/// A received frame: the JSON header and the binary blobs that came with it.
//----------------------------------------------------------------------------------------
struct BridgeMessage
{
	maxon::DataDictionary header;
	maxon::BaseArray<Blob> blobs;
};

/// The blobs of an outgoing frame: views of memory the caller keeps alive during the call.
using BlobViews = maxon::Block<const maxon::Block<const maxon::Char>>;

/// Sends one frame. Blocks until everything is written.
maxon::Result<void> WriteFrame(platform::SocketHandle socket, const maxon::DataDictionary& header, const BlobViews& blobs = BlobViews());

/// Sends one frame whose header is already JSON text (a complete JSON object).
maxon::Result<void> WriteFrameText(platform::SocketHandle socket, const maxon::String& headerJson, const BlobViews& blobs = BlobViews());

/// Blocks until one frame has arrived.
maxon::Result<BridgeMessage> ReadFrame(platform::SocketHandle socket);

} // namespace cyclesrender

#endif // CYCLESRENDER_PROTOCOL_H__
