// SPDX-License-Identifier: Apache-2.0
// The Cycles Render engine as Cinema 4D sees it: a VideoPostData registered as a renderer.

#ifndef CYCLESRENDER_CYCLES_VIDEOPOST_H__
#define CYCLESRENDER_CYCLES_VIDEOPOST_H__

#include "c4d.h"
#include "c4d_videopostdata.h"

#include "../scene/scene_writer.h"
#include "../session/bridge_client.h"
#include "render_settings_map.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// The renderer the user selects in the Render Settings. Cinema 4D creates one instance per
/// render job (it renders a clone of the document) and calls Execute() for every stage of
/// every frame.
///
/// One render job uses one Blender: it is started for the job's first frame and shut down
/// when the frame sequence closes, so an animation does not start Blender per frame. From
/// the second frame on only the scene items that changed are sent (SyncState).
///
/// Render passes: every pass switched on in the settings is asked from Cinema 4D as a named
/// "post effect" buffer (AllocateBuffers). Cinema 4D creates those only when Multi-Pass is on
/// and has a "Post Effects" entry; the passes then show as layers in the Picture Viewer and
/// are saved with the multi-pass file.
//----------------------------------------------------------------------------------------
class CyclesVideoPost : public cinema::VideoPostData
{
	INSTANCEOF(CyclesVideoPost, cinema::VideoPostData)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(CyclesVideoPost); }

	virtual cinema::Bool Init(cinema::GeListNode* node, cinema::Bool isCloneInit) override;
	virtual void Free(cinema::GeListNode* node) override;
	virtual cinema::Bool GetDDescription(const cinema::GeListNode* node, cinema::Description* description, cinema::DESCFLAGS_DESC& flags) const override;
	virtual cinema::Bool SetDParameter(cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data, cinema::DESCFLAGS_SET& flags) override;
	virtual cinema::Bool GetDEnabling(const cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data,
		cinema::DESCFLAGS_ENABLE flags, const cinema::BaseContainer* itemdesc) const override;
	virtual cinema::VIDEOPOSTINFO GetRenderInfo(cinema::BaseVideoPost* node) override;
	virtual void AllocateBuffers(cinema::BaseVideoPost* node, cinema::Render* render, cinema::BaseDocument* doc) override;
	virtual cinema::Bool RenderEngineCheck(const cinema::BaseVideoPost* node, cinema::Int32 id) const override;
	virtual cinema::RENDERRESULT Execute(cinema::BaseVideoPost* node, cinema::VideoPostStruct* vps) override;

private:
	/// Renders the current frame of vps->doc in Blender and writes it into @p rgba.
	maxon::Result<void> RenderFrame(const cinema::BaseContainer& settings, cinema::VideoPostStruct* vps, cinema::VPBuffer* rgba);

	/// Starts Blender for this render job and sends the render settings.
	maxon::Result<void> OpenSession(const cinema::BaseContainer& settings, cinema::VideoPostStruct* vps);

	/// Ends the job's Blender: asks it to quit when @p cleanly, otherwise stops it at once.
	void CloseSession(maxon::Bool cleanly);

	/// Prints each warning once per render job.
	void Report(const maxon::BaseArray<maxon::String>& warnings);

	/// A render pass switched on in the settings and the Cinema 4D buffer asked for it.
	struct PassBuffer
	{
		const RenderPass* pass = nullptr;
		maxon::Int32 bufferId = 0;
	};

	maxon::BaseArray<PassBuffer> _passes;
	maxon::Bool _passesMissingReported = false;
	BridgeClient* _client = nullptr;
	SyncState _sync;                        ///< what the job's Blender holds
	maxon::Int _framesRendered = 0;
	maxon::BaseArray<maxon::String> _reported;
};

/// Registers the renderer with Cinema 4D. Called once from PluginStart().
cinema::Bool RegisterCyclesVideoPost();

} // namespace cyclesrender

#endif // CYCLESRENDER_CYCLES_VIDEOPOST_H__
