// SPDX-License-Identifier: Apache-2.0
// Reads live frames and render progress from the bridge's shared memory
// (protocol/protocol.md, "Frame block").

#ifndef CYCLESRENDER_FRAME_READER_H__
#define CYCLESRENDER_FRAME_READER_H__

#include "c4d.h"

#include "../platform/shared_memory.h"

namespace cyclesrender
{

/// What the viewport in Blender is doing, as the frame block reports it.
enum class VIEWSTATE
{
	STOPPED = 0,
	RENDERING = 1,
	PAUSED = 2,
	DONE = 3
} MAXON_ENUM_LIST(VIEWSTATE);

struct FrameStatus
{
	maxon::UInt64 frameId = 0;        ///< counts published frames; 0 while there is none
	VIEWSTATE state = VIEWSTATE::STOPPED;
	maxon::Int32 sample = 0;          ///< samples rendered so far
	maxon::Int32 samples = 0;         ///< samples wanted; 0 when unlimited
	maxon::Float64 startTime = 0.0;   ///< when the current render began (Unix seconds)
	maxon::Float64 endTime = 0.0;     ///< when it finished or was paused; 0 while it runs
	maxon::Int32 width = 0;           ///< size of the frames being published
	maxon::Int32 height = 0;
	maxon::String text;               ///< the engine's own progress line
};

/// One frame copied out of shared memory: RGBA float32, scene-linear, top row first.
struct Frame
{
	maxon::Int32 width = 0;
	maxon::Int32 height = 0;
	maxon::UInt64 frameId = 0;
	maxon::BaseArray<maxon::Float32> pixels;
};

//----------------------------------------------------------------------------------------
/// Reader side of the frame block. Polling it costs a few memory reads; nothing travels
/// over the socket. Use from one thread at a time.
//----------------------------------------------------------------------------------------
class FrameReader
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(FrameReader);

public:
	FrameReader() = default;
	~FrameReader();

	/// Opens the state block of the Blender that was launched with @p token.
	maxon::Result<void> Open(const maxon::String& token);
	void Close();
	maxon::Bool IsOpen() const { return _state.data != nullptr; }

	/// The current progress. All zero while closed.
	FrameStatus GetStatus() const;

	/// Copies the newest frame into @p frame if it is newer than frame.frameId.
	/// Returns false when there is no newer frame (or it changed while being copied; the next
	/// call gets it).
	maxon::Result<maxon::Bool> Read(Frame& frame);

private:
	maxon::String _prefix;
	platform::SharedMemory _state;
	platform::SharedMemory _pixels;
	maxon::UInt32 _generation = 0;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_FRAME_READER_H__
