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
/// It also hashes every frame (Frame::contentHash), so that the window can leave out a frame that
/// shows nothing new, and announces a frame Take() held back once it may be shown (0.2.0, B11).
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

	//----------------------------------------------------------------------------------------
	/// Moves the newest frame into @p frame if Take() has not returned it yet.
	/// @param[out] arrivalTime  When the frame was fetched from shared memory (Unix seconds).
	/// @param[in] minSeconds    At most one frame per this many seconds (0.2.0, B11): a frame asked for
	///                          sooner is held back (false), and the watcher announces it again, with the
	///                          core message and the arrival callback, as soon as its time has come, so the
	///                          newest frame is always shown in the end. 0: no limit (tests reading frames).
	//----------------------------------------------------------------------------------------
	maxon::Bool Take(Frame& frame, maxon::Float64& arrivalTime, maxon::Float64 minSeconds = 0.0);

	/// A function the watcher thread calls after each frame it fetched, besides the core message.
	/// Set it before Start(); it must be safe on any thread and return at once.
	void SetArrivalCallback(void (*callback)()) { _onArrival = callback; }

private:
	void (*_onArrival)() = nullptr;
	maxon::Spinlock _lock;					///< guards the members up to _arrival
	maxon::String _token;
	maxon::Bool _tokenChanged = false;
	Frame _latest;
	maxon::Bool _fresh = false;			///< _latest has not been taken yet
	maxon::Float64 _arrival = 0.0;
	maxon::Float64 _lastTake = 0.0;		///< when Take() last returned a frame (TimeValue seconds)
	maxon::Bool _held = false;				///< Take() held _latest back; announce it again at _heldUntil
	maxon::Float64 _heldUntil = 0.0;

	FrameReader _reader;						///< used by the watcher thread only, like the rest
	platform::Signal _signal;
	Frame _work;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_FRAME_WATCHER_H__
