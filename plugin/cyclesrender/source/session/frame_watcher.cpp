// SPDX-License-Identifier: Apache-2.0
// The thread that fetches each live frame as soon as Blender has published it
// (frame_watcher.h).

#include "c4d.h"

#include "../ids.h"
#include "frame_watcher.h"

namespace cyclesrender
{

// Without a signal (or should one be missed) the block is looked at this often anyway.
static const int POLL_MILLISECONDS = 20;

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

maxon::Bool FrameWatcher::Take(Frame& frame, maxon::Float64& arrivalTime)
{
	maxon::ScopedLock guard(_lock);
	if (!_fresh)
		return false;
	_fresh = false;
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

		// Returns at once when a frame was published, and after the poll time otherwise.
		platform::SignalWait(_signal, POLL_MILLISECONDS);
		if (!_reader.IsOpen())
			continue;

		iferr (const maxon::Bool got = _reader.Read(_work))
			continue;
		if (!got)
			continue;

		maxon::Bool notify = false;
		{
			maxon::ScopedLock guard(_lock);
			notify = !_fresh;
			std::swap(_latest, _work);
			_work.frameId = _latest.frameId;	// only frames newer than this one are fetched
			_fresh = true;
			_arrival = platform::UnixTime();
		}
		// One message per frame the main thread has not picked up yet is enough.
		if (notify)
			cinema::SpecialEventAdd(ID_CORE_MESSAGE);
	}
}

} // namespace cyclesrender
