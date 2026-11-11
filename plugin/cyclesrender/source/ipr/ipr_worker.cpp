// SPDX-License-Identifier: Apache-2.0
// The worker thread that owns one windowed Blender and sends every request to it
// (ipr_worker.h).

#include "c4d.h"

#include "../platform/shared_memory.h"
#include "ipr_worker.h"

namespace cyclesrender
{

// How long the idle worker sleeps before it checks again that Blender is still alive.
static const int IDLE_MILLISECONDS = 20;

IprWorker::IprWorker()
{
	platform::SignalCreate(nullptr, _wake);
}

IprWorker::~IprWorker()
{
	// Ends the thread; a request in flight notices the break and returns.
	End(true);

	if (_client)
	{
		iferr (_client->Shutdown())
		{
			// Destroying the client below stops Blender in any case.
		}
		DeleteObj(_client);
	}
	for (IprTask*& task : _queue)
		DeleteObj(task);
	platform::SignalClose(_wake);
}

void IprWorker::Post(IprTask* task)
{
	if (!task)
		return;
	maxon::ScopedLock guard(_lock);
	iferr (_queue.Append(task))
	{
		DeleteObj(task);
		return;
	}
	_pending.SwapIncrement();
	platform::SignalSet(_wake);
}

void IprWorker::PostRequest(const JsonWriter& request)
{
	iferr (IprTask* const task = NewObj(IprTask))
		return;
	task->kind = IprTask::KIND::REQUEST;
	task->request = request;
	Post(task);
}

IprTask* IprWorker::Pop()
{
	maxon::ScopedLock guard(_lock);
	if (_queue.IsEmpty())
		return nullptr;
	IprTask* const task = _queue[0];
	_queue.Erase(0) iferr_ignore("Erasing the first of a non-empty array cannot fail.");
	return task;
}

maxon::Bool IprWorker::HasFailed(maxon::String& message)
{
	maxon::ScopedLock guard(_lock);
	if (_failed)
		message = _failure;
	return _failed;
}

void IprWorker::ClearFailure()
{
	maxon::ScopedLock guard(_lock);
	_failed = false;
	_failure = maxon::String();
}

maxon::Bool IprWorker::GetSession(maxon::String& token, maxon::Int& processId)
{
	maxon::ScopedLock guard(_lock);
	if (_token.IsEmpty())
		return false;
	token = _token;
	processId = _processId;
	return true;
}

void IprWorker::TakeWarnings(maxon::BaseArray<maxon::String>& warnings)
{
	maxon::ScopedLock guard(_lock);
	warnings = std::move(_warnings);
	_warnings.Reset();
}

maxon::Float64 IprWorker::GetLastSceneAnswerTime()
{
	maxon::ScopedLock guard(_lock);
	return _sceneAnswerTime;
}

void IprWorker::KillBlenderForTest()
{
	_killRequested.Set(1);
	platform::SignalSet(_wake);
}

void IprWorker::DropSession()
{
	DeleteObj(_client);	// kills Blender if it still runs
	maxon::ScopedLock guard(_lock);
	_token = maxon::String();
	_processId = 0;
}

void IprWorker::Fail(const maxon::String& message)
{
	DropSession();

	maxon::ScopedLock guard(_lock);
	_failed = true;
	_failure = message;
	for (IprTask*& task : _queue)
	{
		DeleteObj(task);
		_pending.SwapDecrement();
	}
	_queue.Reset();
}

maxon::Result<void> IprWorker::Run(IprTask& task)
{
	iferr_scope;

	switch (task.kind)
	{
		case IprTask::KIND::OPEN:
		{
			if (_client)
				return maxon::OK;
			_client = NewObj(BridgeClient) iferr_return;
			_client->Open(BRIDGEMODE::VIEWPORT) iferr_return;
			maxon::ScopedLock guard(_lock);
			_token = _client->GetToken();
			_processId = _client->GetProcessId();
			return maxon::OK;
		}
		case IprTask::KIND::CLOSE:
		{
			if (!_client)
				return maxon::OK;
			iferr (_client->Shutdown())
			{
				// Dropping the session below stops Blender in any case.
			}
			DropSession();
			return maxon::OK;
		}
		case IprTask::KIND::REQUEST:
		{
			// Without a session there is nobody to tell; the failure that ended it was reported.
			if (!_client)
				return maxon::OK;
			_client->Request(task.request, BlobViews(), Get()) iferr_return;
			_requestsSent.SwapIncrement();
			return maxon::OK;
		}
		case IprTask::KIND::SCENE:
		{
			if (!_client || !task.scene)
				return maxon::OK;
			task.scene->Send(*_client, Get()) iferr_return;
			_scenesSent.SwapIncrement();
			maxon::ScopedLock guard(_lock);
			_sceneAnswerTime = platform::UnixTime();
			for (const maxon::String& warning : task.scene->GetWarnings())
			{
				_warnings.Append(warning) iferr_return;
			}
			return maxon::OK;
		}
	}
	return maxon::OK;
}

void IprWorker::Main()
{
	while (!TestBreak())
	{
		if (_killRequested.Swap(0) != 0 && _client)
			_client->KillBlender();	// the lines below then find a session whose Blender is gone

		IprTask* task = Pop();
		if (!task)
		{
			if (_client && !_client->IsAlive())
				Fail("Blender has stopped unexpectedly."_s);
			platform::SignalWait(_wake, IDLE_MILLISECONDS);
			continue;
		}

		iferr (Run(*task))
		{
			if (!TestBreak())
			{
				const maxon::Bool blenderGone = _client && !_client->IsAlive();
				Fail(blenderGone ? "Blender has stopped unexpectedly."_s : err.GetMessage());
			}
		}
		DeleteObj(task);
		_pending.SwapDecrement();
	}
}

} // namespace cyclesrender
