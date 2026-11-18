// SPDX-License-Identifier: Apache-2.0
// The thread that fetches each live frame as soon as Blender has published it
// (frame_watcher.h).

#include "c4d.h"

#include "../ids.h"
#include "../ipr/ipr_trace.h"
#include "frame_watcher.h"

namespace cyclesrender
{

// Without a signal (or should one be missed) the block is looked at this often anyway.
static const int POLL_MILLISECONDS = 20;

static maxon::Float64 Now()
{
	return maxon::TimeValue::GetTime().GetSeconds();
}

// A hash of the frame's size and every pixel value, in four lanes so that it runs at memory speed
// (about 2 ms for 1920 x 1080 on the frame thread). Equal frames have equal hashes; a frame that
// differs in any value has another hash, short of a collision of 64-bit hashes.
static maxon::UInt64 HashFrame(const Frame& frame)
{
	const maxon::Int count = frame.pixels.GetCount() / 2;	// 64-bit words: two values each
	const maxon::UInt64* const words = reinterpret_cast<const maxon::UInt64*>(frame.pixels.GetFirst());
	maxon::UInt64 lane[4] = { 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
	maxon::Int i = 0;
	for (; i + 3 < count; i += 4)
	{
		lane[0] = (lane[0] ^ words[i + 0]) * 0x100000001B3ull;
		lane[1] = (lane[1] ^ words[i + 1]) * 0x100000001B3ull;
		lane[2] = (lane[2] ^ words[i + 2]) * 0x100000001B3ull;
		lane[3] = (lane[3] ^ words[i + 3]) * 0x100000001B3ull;
	}
	for (; i < count; ++i)
		lane[0] = (lane[0] ^ words[i]) * 0x100000001B3ull;
	maxon::UInt64 hash = maxon::UInt64(frame.width) * 0x9E3779B97F4A7C15ull ^ maxon::UInt64(frame.height);
	for (const maxon::UInt64 value : lane)
		hash = (hash ^ value ^ (value >> 29)) * 0xBF58476D1CE4E5B9ull;
	return hash | 1;	// never 0, which means "not known"
}

FrameWatcher::~FrameWatcher()
{
	End(true);
	_reader.Close();
	platform::SignalClose(_signal);
}

void FrameWatcher::Watch(const maxon::String& token)
{
	maxon::ScopedLock guard(_lock);
	if (token == _token)
		return;
	_token = token;
	_tokenChanged = true;
	_fresh = false;
}

maxon::Bool FrameWatcher::Take(Frame& frame, maxon::Float64& arrivalTime, maxon::Float64 minSeconds)
{
	const maxon::Float64 now = Now();
	maxon::ScopedLock guard(_lock);
	if (!_fresh)
		return false;
	if (minSeconds > 0.0 && now - _lastTake < minSeconds)
	{
		// Too soon after the last one: Main() announces it again when it may be shown.
		_held = true;
		_heldUntil = _lastTake + minSeconds;
		return false;
	}
	_fresh = false;
	_held = false;
	_lastTake = now;
	// Swapping hands the caller's old pixel buffer back for reuse.
	std::swap(frame, _latest);
	arrivalTime = _arrival;
	return true;
}

void FrameWatcher::Main()
{
	while (!TestBreak())
	{
		maxon::String token;
		maxon::Bool reopen = false;
		{
			maxon::ScopedLock guard(_lock);
			reopen = _tokenChanged;
			_tokenChanged = false;
			token = _token;
		}
		if (reopen)
		{
			_reader.Close();
			platform::SignalClose(_signal);
			_work.frameId = 0;
			if (!token.IsEmpty())
			{
				iferr (_reader.Open(token))
				{
					// The block appears with Blender's viewport; try again on the next round.
					maxon::ScopedLock guard(_lock);
					_tokenChanged = true;
				}
				else
				{
					iferr (maxon::BaseArray<maxon::Char> name = ("cyclesrender_"_s + token + "_frame"_s).GetCString())
						continue;
					iferr (name.Append('\0'))
						continue;
					platform::SignalCreate(name.GetFirst(), _signal);
				}
			}
		}

		// Returns at once when a frame was published, and after the poll time otherwise; sooner when a
		// frame the window held back may be shown.
		int wait = POLL_MILLISECONDS;
		{
			maxon::ScopedLock guard(_lock);
			if (_held)
				wait = maxon::ClampValue(int((_heldUntil - Now()) * 1000.0) + 1, 1, POLL_MILLISECONDS);
		}
		platform::SignalWait(_signal, wait);

		// The last frame of a burst must reach the screen even when nothing comes after it: a frame
		// Take() held back is announced again once its time has come (the trailing frame).
		maxon::Bool due = false;
		{
			maxon::ScopedLock guard(_lock);
			if (_held && Now() >= _heldUntil)
			{
				_held = false;
				due = _fresh;
			}
		}
		if (due)
		{
			TraceIpr("frame_due");
			cinema::SpecialEventAdd(ID_CORE_MESSAGE);
			if (_onArrival)
				_onArrival();
		}

		if (!_reader.IsOpen())
			continue;

		iferr (const maxon::Bool got = _reader.Read(_work))
			continue;
		if (!got)
			continue;
		_work.contentHash = HashFrame(_work);

		maxon::Bool notify = false;
		{
			maxon::ScopedLock guard(_lock);
			notify = !_fresh;
			std::swap(_latest, _work);
			_work.frameId = _latest.frameId;	// only frames newer than this one are fetched
			_fresh = true;
			_arrival = platform::UnixTime();
		}
		TraceIpr("frame", maxon::Float64(_latest.frameId));
		// One message per frame the main thread has not picked up yet is enough.
		if (notify)
			cinema::SpecialEventAdd(ID_CORE_MESSAGE);
		// The core message waits while Cinema 4D is in a modal loop; the callback can reach the main
		// thread there (ipr/ipr_scenehook.h). Called for every frame: it decides itself.
		if (_onArrival)
			_onArrival();
	}
}

} // namespace cyclesrender
