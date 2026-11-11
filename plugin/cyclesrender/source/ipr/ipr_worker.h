// SPDX-License-Identifier: Apache-2.0
// The thread that talks to the IPR's Blender, so that Cinema 4D's interface never waits for it.

#ifndef CYCLESRENDER_IPR_WORKER_H__
#define CYCLESRENDER_IPR_WORKER_H__

#include "c4d.h"
#include "maxon/atomictypes.h"
#include "maxon/spinlock.h"

#include "../platform/shared_memory.h"
#include "../scene/scene_writer.h"
#include "../session/bridge_client.h"

namespace cyclesrender
{

/// One piece of work for the worker, done in the order posted.
struct IprTask
{
	enum class KIND
	{
		OPEN,			///< start Blender if it is not running
		REQUEST,	///< send `request`
		SCENE,		///< send `scene`
		CLOSE			///< ask Blender to quit
	} MAXON_ENUM_LIST_CLASS(KIND);

	KIND kind = KIND::REQUEST;
	JsonWriter request;						///< for REQUEST: a request with its top-level object still open
	SceneWriter* scene = nullptr;	///< for SCENE; owned by the task

	~IprTask() { DeleteObj(scene); }
};

//----------------------------------------------------------------------------------------
/// Owns the windowed Blender of the IPR. The main thread posts tasks and polls the results;
/// every request to Blender is sent from this thread. When a task fails, or Blender exits by
/// itself, the session is dropped, waiting tasks are discarded and HasFailed() reports why.
//----------------------------------------------------------------------------------------
class IprWorker : public cinema::C4DThread
{
public:
	IprWorker();
	virtual ~IprWorker();

	virtual void Main() override;
	virtual const cinema::Char* GetThreadName() override { return "CyclesRenderIPR"; }

	/// Queues a task; the worker takes ownership.
	void Post(IprTask* task);
	void PostRequest(const JsonWriter& request);

	/// Tasks posted and not finished yet. 0 means Blender has everything that was sent.
	maxon::Int32 GetPendingCount() const { return _pending.LoadRelaxed(); }

	/// True after a failure, until ClearFailure(). @p message says what went wrong, for the user.
	maxon::Bool HasFailed(maxon::String& message);
	void ClearFailure();

	/// The running session: the token names its frame block. False while no Blender is open.
	maxon::Bool GetSession(maxon::String& token, maxon::Int& processId);

	/// Moves the warnings collected since the last call into @p warnings.
	void TakeWarnings(maxon::BaseArray<maxon::String>& warnings);

	/// When the last `scene` request was answered by Blender (Unix seconds), for latency measurements.
	maxon::Float64 GetLastSceneAnswerTime();

	/// Totals since the worker started, for tests.
	maxon::Int32 GetSentSceneCount() const { return _scenesSent.LoadRelaxed(); }
	maxon::Int32 GetSentRequestCount() const { return _requestsSent.LoadRelaxed(); }

	/// Ends the Blender process at once, as a crash would. For tests.
	void KillBlenderForTest();

private:
	IprTask* Pop();
	maxon::Result<void> Run(IprTask& task);
	void Fail(const maxon::String& message);
	void DropSession();

	maxon::Spinlock _lock;								///< guards everything below except the atomics
	maxon::BaseArray<IprTask*> _queue;
	maxon::String _failure;
	maxon::Bool _failed = false;
	maxon::String _token;
	maxon::Int _processId = 0;
	maxon::BaseArray<maxon::String> _warnings;
	maxon::Float64 _sceneAnswerTime = 0.0;

	maxon::AtomicInt32 _pending;
	maxon::AtomicInt32 _scenesSent;
	maxon::AtomicInt32 _requestsSent;
	maxon::AtomicInt32 _killRequested;
	platform::Signal _wake;								///< raised when there is something to do
	BridgeClient* _client = nullptr;			///< used by the worker thread only
};

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_WORKER_H__
