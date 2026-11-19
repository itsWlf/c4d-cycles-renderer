// SPDX-License-Identifier: Apache-2.0
// The IPR in Cinema 4D's modal loops (ipr_scenehook.h).
//
// Execute() runs for every evaluation of every document, on whatever thread evaluates it, so it
// does as little as possible: unless the IPR window renders and Cinema 4D is in a modal loop
// (IsIprInModalLoop), it returns at once. Then, on the main thread, it calls the window directly for
// the active document. On another thread it has the controller follow the evaluation right there
// (IprController::FollowEvaluation), and, if that was the document the IPR shows, hands the window one
// call through the main thread's queue for what only the main thread can do: showing the newest frame,
// and asking which view and camera count. A frame that arrived is handed over the same way. Calls are
// not queued up: while one waits, no other is added.
//
// The hook itself keeps nothing and reads only atomics (ipr_dialog.h). On a thread other than the main
// thread it touches neither the window nor the active document; the controller is entered only through
// FollowEvaluation(), which counts the thread, so that IprController::Free() can wait for it.

#include "c4d.h"
#include "c4d_scenehookdata.h"
#include "maxon/atomictypes.h"
#include "maxon/thread.h"

#include "../ids.h"
#include "ipr_controller.h"
#include "ipr_dialog.h"
#include "ipr_scenehook.h"
#include "ipr_trace.h"

namespace cyclesrender
{

// A call handed to the main thread that has not run yet.
static maxon::AtomicBool g_callWaiting;

// Hands FollowIprEvaluation() to the main thread's queue, unless a call is waiting already. The call
// reads no cache of the document: by the time the main thread makes it, another evaluation may be at
// work (in the Attribute Manager's loop the queue runs with the next mouse message, which also starts
// the next evaluation).
// @param[in] source  For the trace: 1 an evaluation on another thread, 2 a frame.
static void CallOnMainThread(maxon::Float64 source)
{
	if (!g_callWaiting.TryCompareAndSwap(true, false))
		return;
	iferr (maxon::JobRef job = maxon::JobRef::Create([source]()
		{
			g_callWaiting.Set(false);
			TraceIpr("hook_call", source);
			FollowIprEvaluation(false);
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
		// After the expressions, the generators and the scene nodes: the document's values are what the
		// step set, and its caches are built (measured for generators and deformers at "Generators + 1":
		// docs/reference/c4d-sdk.md, "What the IPR sends while a change is in progress").
		list->Add(node, EXECUTIONPRIORITY_SCENENODE + 1, cinema::EXECUTIONFLAGS::NONE);
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
			// Reading the document here is safe: this is the end of the evaluation, on the thread that runs it.
			TraceIpr("hook_execute", 1.0);
			if (doc == cinema::GetActiveDocument())
				FollowIprEvaluation(true);
		}
		else
		{
			// The same on this thread, for the document the IPR shows (the controller knows which; rendered
			// copies and other documents are evaluated as well, and for those nothing is handed on). Then the
			// window's part on the main thread.
			TraceIpr("hook_execute", 0.0);
			if (IprController::FollowEvaluation(doc, IsIprOptimizedInLoop()))
				CallOnMainThread(1.0);
		}
		return cinema::EXECUTIONRESULT::OK;
	}
};

cinema::Bool RegisterIprSceneHook()
{
	// The priority given here is the one of drawing and input, which the hook does not take part in;
	// when Execute() runs is set in AddToExecution().
	return cinema::RegisterSceneHookPlugin(ID_IPR_SCENE_HOOK, "Cycles Render IPR"_s, 0, IprSceneHook::Alloc, EXECUTIONPRIORITY_GENERATOR, 0);
}

} // namespace cyclesrender
