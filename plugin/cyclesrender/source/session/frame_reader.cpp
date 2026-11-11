// SPDX-License-Identifier: Apache-2.0
// Reader of the shared-memory frame block the bridge writes (frame_reader.h). The layout is
// defined in protocol/protocol.md, "Frame block"; the writer is
// bridge/c4d_bridge/viewport/frame_block.py.

#include "c4d.h"

#include "frame_reader.h"

namespace cyclesrender
{

// Layout of the state block; see protocol/protocol.md.
static const maxon::UInt32 STATE_MAGIC = 0x54535243;	// "CRST"
static const maxon::UInt32 STATE_LAYOUT = 1;
static const maxon::Int STATE_SIZE = 256;
static const maxon::Int OFFSET_HEADER_SEQUENCE = 8;
static const maxon::Int OFFSET_GENERATION = 16;
static const maxon::Int OFFSET_WIDTH = 20;
static const maxon::Int OFFSET_HEIGHT = 24;
static const maxon::Int OFFSET_LATEST = 28;
static const maxon::Int OFFSET_BUFFER_SEQUENCE = 32;
static const maxon::Int OFFSET_FRAME_ID = 48;
static const maxon::Int OFFSET_STATE = 56;
static const maxon::Int OFFSET_SAMPLE = 60;
static const maxon::Int OFFSET_SAMPLES = 64;
static const maxon::Int OFFSET_START_TIME = 72;
static const maxon::Int OFFSET_END_TIME = 80;
static const maxon::Int OFFSET_TEXT = 128;
static const maxon::Int TEXT_SIZE = 128;

static const maxon::Int32 MAX_FRAME_SIDE = 16384;

template <typename T> static T ReadValue(const platform::SharedMemory& memory, maxon::Int offset)
{
	T value;
	maxon::MemCopyType(reinterpret_cast<maxon::UChar*>(&value), memory.data + offset, SIZEOF(T));
	return value;
}

static maxon::Result<void> OpenBlock(const maxon::String& name, platform::SharedMemory& memory)
{
	iferr_scope;
	maxon::BaseArray<maxon::Char> text = name.GetCString() iferr_return;
	text.Append('\0') iferr_return;
	if (!platform::SharedMemoryOpen(text.GetFirst(), memory))
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, FormatString("The shared memory block @ does not exist.", name));
	return maxon::OK;
}

FrameReader::~FrameReader()
{
	Close();
}

maxon::Result<void> FrameReader::Open(const maxon::String& token)
{
	iferr_scope;

	Close();
	_prefix = "cyclesrender_"_s + token;
	OpenBlock(_prefix + "_state"_s, _state) iferr_return;
	if (_state.size < STATE_SIZE || ReadValue<maxon::UInt32>(_state, 0) != STATE_MAGIC || ReadValue<maxon::UInt32>(_state, 4) != STATE_LAYOUT)
	{
		Close();
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "Blender's frame block has an unknown layout."_s);
	}
	return maxon::OK;
}

void FrameReader::Close()
{
	platform::SharedMemoryClose(_pixels);
	platform::SharedMemoryClose(_state);
	_generation = 0;
}

FrameStatus FrameReader::GetStatus() const
{
	FrameStatus status;
	if (!_state.data)
		return status;

	status.frameId = ReadValue<maxon::UInt64>(_state, OFFSET_FRAME_ID);
	const maxon::UInt32 state = ReadValue<maxon::UInt32>(_state, OFFSET_STATE);
	status.state = state <= 3 ? VIEWSTATE(state) : VIEWSTATE::STOPPED;
	status.sample = maxon::Int32(ReadValue<maxon::UInt32>(_state, OFFSET_SAMPLE));
	status.samples = maxon::Int32(ReadValue<maxon::UInt32>(_state, OFFSET_SAMPLES));
	status.startTime = ReadValue<maxon::Float64>(_state, OFFSET_START_TIME);
	status.endTime = ReadValue<maxon::Float64>(_state, OFFSET_END_TIME);
	status.width = maxon::Int32(ReadValue<maxon::UInt32>(_state, OFFSET_WIDTH));
	status.height = maxon::Int32(ReadValue<maxon::UInt32>(_state, OFFSET_HEIGHT));

	maxon::Char text[TEXT_SIZE];
	maxon::MemCopyType(reinterpret_cast<maxon::UChar*>(text), _state.data + OFFSET_TEXT, TEXT_SIZE);
	text[TEXT_SIZE - 1] = 0;
	status.text = maxon::String(text);
	return status;
}

maxon::Result<maxon::Bool> FrameReader::Read(Frame& frame)
{
	iferr_scope;

	if (!_state.data)
		return false;

	// The writer makes a sequence number odd while it changes what the number guards, so a
	// copy is valid when the number was even before and is unchanged after.
	const maxon::UInt64 headerBefore = ReadValue<maxon::UInt64>(_state, OFFSET_HEADER_SEQUENCE);
	const maxon::UInt32 generation = ReadValue<maxon::UInt32>(_state, OFFSET_GENERATION);
	const maxon::Int32 width = maxon::Int32(ReadValue<maxon::UInt32>(_state, OFFSET_WIDTH));
	const maxon::Int32 height = maxon::Int32(ReadValue<maxon::UInt32>(_state, OFFSET_HEIGHT));
	const maxon::UInt32 latest = ReadValue<maxon::UInt32>(_state, OFFSET_LATEST);
	const maxon::UInt64 frameId = ReadValue<maxon::UInt64>(_state, OFFSET_FRAME_ID);

	if ((headerBefore & 1) || generation == 0 || frameId == 0 || frameId == frame.frameId || latest > 1)
		return false;
	if (width < 1 || height < 1 || width > MAX_FRAME_SIDE || height > MAX_FRAME_SIDE)
		return false;

	const maxon::Int rowValues = maxon::Int(width) * 4;
	const maxon::Int bufferBytes = rowValues * height * SIZEOF(maxon::Float32);
	if (generation != _generation || !_pixels.data)
	{
		platform::SharedMemoryClose(_pixels);
		_generation = 0;
		// The block can be gone again already when sizes change in quick succession.
		iferr (OpenBlock(_prefix + FormatString("_px@", generation), _pixels))
			return false;
		_generation = generation;
	}
	if (_pixels.size < bufferBytes * 2)
		return false;

	const maxon::UInt64 sequenceBefore = ReadValue<maxon::UInt64>(_state, OFFSET_BUFFER_SEQUENCE + 8 * maxon::Int(latest));
	if (sequenceBefore & 1)
		return false;

	frame.pixels.Resize(rowValues * height, maxon::COLLECTION_RESIZE_FLAGS::POD_UNINITIALIZED) iferr_return;

	// Stored bottom row first; Cinema 4D wants the top row first.
	const maxon::UChar* const source = _pixels.data + bufferBytes * maxon::Int(latest);
	const maxon::Int rowBytes = rowValues * SIZEOF(maxon::Float32);
	for (maxon::Int32 y = 0; y < height; ++y)
	{
		maxon::MemCopyType(reinterpret_cast<maxon::UChar*>(frame.pixels.GetFirst() + rowValues * y),
			source + rowBytes * maxon::Int(height - 1 - y), rowBytes);
	}

	if (ReadValue<maxon::UInt64>(_state, OFFSET_BUFFER_SEQUENCE + 8 * maxon::Int(latest)) != sequenceBefore ||
		ReadValue<maxon::UInt64>(_state, OFFSET_HEADER_SEQUENCE) != headerBefore)
		return false;

	frame.width = width;
	frame.height = height;
	frame.frameId = frameId;
	return true;
}

} // namespace cyclesrender
