// SPDX-License-Identifier: Apache-2.0
// A thread that fetches each live frame the moment Blender publishes it.

#ifndef CYCLESRENDER_FRAME_WATCHER_H__
#define CYCLESRENDER_FRAME_WATCHER_H__

#include "c4d.h"
#include "maxon/spinlock.h"

#include "frame_reader.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// Waits for the signal the bridge raises after every frame (protocol/protocol.md, "Frame
/// block"), copies the frame out of shared memory, and tells the main thread with the core
/// message ID_CORE_MESSAGE. The main thread then calls Take(). So a frame is on its way to
/// the screen without anyone polling, and copying it does not hold up Cinema 4D's interface.
//----------------------------------------------------------------------------------------
class FrameWatcher : public cinema::C4DThread
{
public:
	FrameWatcher() = default;
	virtual ~FrameWatcher();

	virtual void Main() override;
	virtual const cinema::Char* GetThreadName() override { return "CyclesRenderFrames"; }

	/// Watch the frame block of the Blender launched with @p token. Empty: watch nothing.
	void Watch(const maxon::String& token);

	/// Moves the newest frame into @p frame if Take() has not returned it yet.
	/// @param[out] arrivalTime  When the frame was fetched from shared memory (Unix seconds).
	maxon::Bool Take(Frame& frame, maxon::Float64& arrivalTime);

private:
	maxon::Spinlock _lock;					///< guards the members up to _arrival
	maxon::String _token;
	maxon::Bool _tokenChanged = false;
	Frame _latest;
	maxon::Bool _fresh = false;			///< _latest has not been taken yet
	maxon::Float64 _arrival = 0.0;

	FrameReader _reader;						///< used by the watcher thread only, like the rest
	platform::Signal _signal;
	Frame _work;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_FRAME_WATCHER_H__
