// SPDX-License-Identifier: Apache-2.0
// Reading and writing frames of the bridge protocol (protocol.h). The format is defined in
// protocol/protocol.md; the bridge's side of it is bridge/c4d_bridge/protocol.py.

#include "c4d.h"

#include "json.h"
#include "protocol.h"

namespace cyclesrender
{

// The frame prefix as it travels: all fields little-endian. Cinema 4D only runs on
// little-endian machines, so the struct is sent as it lies in memory.
#pragma pack(push, 1)
struct FramePrefix
{
	maxon::Char magic[4];
	maxon::UInt16 version;
	maxon::UInt16 flags;
	maxon::UInt32 headerLength;
	maxon::UInt32 blobCount;
};
#pragma pack(pop)

static const maxon::UInt32 MAX_HEADER_BYTES = 16 * 1024 * 1024;
static const maxon::UInt32 MAX_BLOBS = 4096;

static maxon::Result<void> Send(platform::SocketHandle socket, const void* data, maxon::Int size)
{
	if (size > 0 && !platform::SocketSend(socket, data, size))
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "The connection to Blender was lost while sending."_s);
	return maxon::OK;
}

static maxon::Result<void> Receive(platform::SocketHandle socket, void* data, maxon::Int size)
{
	if (size > 0 && !platform::SocketReceive(socket, data, size))
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "The connection to Blender was lost while receiving."_s);
	return maxon::OK;
}

maxon::Result<void> WriteFrame(platform::SocketHandle socket, const maxon::DataDictionary& header, const BlobViews& blobs)
{
	iferr_scope;

	const maxon::String text = WriteJson(header) iferr_return;
	return WriteFrameText(socket, text, blobs);
}

maxon::Result<void> WriteFrameText(platform::SocketHandle socket, const maxon::String& headerJson, const BlobViews& blobs)
{
	iferr_scope;

	const maxon::BaseArray<maxon::Char> headerBytes = headerJson.GetCString() iferr_return;

	FramePrefix prefix;
	prefix.magic[0] = 'C';
	prefix.magic[1] = '4';
	prefix.magic[2] = 'D';
	prefix.magic[3] = 'B';
	prefix.version = maxon::UInt16(PROTOCOL_VERSION);
	prefix.flags = 0;
	prefix.headerLength = maxon::UInt32(headerBytes.GetCount());
	prefix.blobCount = maxon::UInt32(blobs.GetCount());

	maxon::BaseArray<maxon::UInt64> lengths;
	for (const maxon::Block<const maxon::Char>& blob : blobs)
	{
		lengths.Append(maxon::UInt64(blob.GetCount())) iferr_return;
	}

	Send(socket, &prefix, SIZEOF(prefix)) iferr_return;
	Send(socket, lengths.GetFirst(), lengths.GetCount() * SIZEOF(maxon::UInt64)) iferr_return;
	Send(socket, headerBytes.GetFirst(), headerBytes.GetCount()) iferr_return;
	for (const maxon::Block<const maxon::Char>& blob : blobs)
	{
		Send(socket, blob.GetFirst(), blob.GetCount()) iferr_return;
	}
	return maxon::OK;
}

maxon::Result<BridgeMessage> ReadFrame(platform::SocketHandle socket)
{
	iferr_scope;

	FramePrefix prefix;
	Receive(socket, &prefix, SIZEOF(prefix)) iferr_return;
	if (prefix.magic[0] != 'C' || prefix.magic[1] != '4' || prefix.magic[2] != 'D' || prefix.magic[3] != 'B')
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "Blender sent data that is not a bridge frame."_s);
	if (prefix.version != PROTOCOL_VERSION)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "Blender speaks a different bridge protocol version."_s);
	if (prefix.headerLength > MAX_HEADER_BYTES || prefix.blobCount > MAX_BLOBS)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "Blender sent a frame that exceeds the protocol limits."_s);

	maxon::BaseArray<maxon::UInt64> lengths;
	lengths.Resize(prefix.blobCount) iferr_return;
	Receive(socket, lengths.GetFirst(), lengths.GetCount() * SIZEOF(maxon::UInt64)) iferr_return;

	maxon::BaseArray<maxon::Char> headerBytes;
	headerBytes.Resize(prefix.headerLength) iferr_return;
	Receive(socket, headerBytes.GetFirst(), headerBytes.GetCount()) iferr_return;

	BridgeMessage message;
	message.header = ParseJson(maxon::String(headerBytes.GetFirst(), headerBytes.GetCount())) iferr_return;
	for (const maxon::UInt64 length : lengths)
	{
		Blob blob;
		blob.Resize(maxon::Int(length)) iferr_return;
		Receive(socket, blob.GetFirst(), blob.GetCount()) iferr_return;
		message.blobs.Append(std::move(blob)) iferr_return;
	}
	return std::move(message);
}

} // namespace cyclesrender
