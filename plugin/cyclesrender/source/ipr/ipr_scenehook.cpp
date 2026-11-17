// SPDX-License-Identifier: Apache-2.0
// The IPR in Cinema 4D's modal loops (ipr_scenehook.h).
//
// Execute() runs for every evaluation of every document, on whatever thread evaluates it, so it
// does as little as possible: unless the IPR window renders and Cinema 4D is in a modal loop
// (IsIprInModalLoop), it returns at once. Then, on the main thread, it calls the window directly for
// the active document; on another thread, and for a frame that arrived, it hands one call to the main
// thread's queue. Calls are not queued up: while one waits, no other is added.

#include "c4d.h"
#include "c4d_scenehookdata.h"
#include "maxon/atomictypes.h"
#include "maxon/thread.h"

#include "../ids.h"
#include "ipr_dialog.h"
#include "ipr_scenehook.h"
#include "ipr_trace.h"

namespace cyclesrender
{

// A call handed to the main thread that has not run yet.
static maxon::AtomicBool g_callWaiting;

// Hands FollowIprEvaluation() to the main thread's queue, unless a call is waiting already.
// @param[in] source  For the trace: 1 an evaluation on another thread, 2 a frame.
static void CallOnMainThread(maxon::Float64 source)
{
	if (!g_callWaiting.TryCompareAndSwap(true, false))
		return;
	iferr (maxon::JobRef job = maxon::JobRef::Create([source]()
		{
			g_callWaiting.Set(false);
			TraceIpr("hook_call", source);
			FollowIprEvaluation();
		}))
	{
		g_callWaiting.Set(false);
		return;
	}
	job.Enqueue(maxon::JobQueueRef::GetMainThreadQueue());
}

void NoteIprFrameArrived()
{
	if (IsIprInModalLoop())
		CallOnMainThread(2.0);
}

class IprSceneHook : public cinema::SceneHookData
{
public:
	static cinema::NodeData* Alloc() { return NewObjClear(IprSceneHook); }

	virtual cinema::Bool AddToExecution(cinema::BaseSceneHook* node, cinema::PriorityList* list) override
	{
		// After the expressions and generators, when the document's values are what the step set.
		list->Add(node, EXECUTIONPRIORITY_GENERATOR + 1, cinema::EXECUTIONFLAGS::NONE);
		return true;
	}

	virtual cinema::EXECUTIONRESULT Execute(cinema::BaseSceneHook* node, cinema::BaseDocument* doc, cinema::BaseThread* bt, cinema::Int32 priority,
		cinema::EXECUTIONFLAGS flags) override
	{
		if (!IsIprInModalLoop())
			return cinema::EXECUTIONRESULT::OK;
		if (cinema::GeIsMainThread())
		{
			// Rendered copies and other documents are evaluated as well; only the active one counts.
			// Reading the lights here is safe: this is the end of the evaluation, on the thread that runs it.
			TraceIpr("hook_execute", 1.0);
			if (doc == cinema::GetActiveDocument())
				FollowIprEvaluation();
		}
		else
		{
			// Which document this was is decided on the main thread, where the active one can be asked.
			TraceIpr("hook_execute", 0.0);
			CallOnMainThread(1.0);
		}
		return cinema::EXECUTIONRESULT::OK;
	}
};

cinema::Bool RegisterIprSceneHook()
{
	return cinema::RegisterSceneHookPlugin(ID_IPR_SCENE_HOOK, "Cycles Render IPR"_s, 0, IprSceneHook::Alloc, EXECUTIONPRIORITY_GENERATOR, 0);
}

} // namespace cyclesrender
