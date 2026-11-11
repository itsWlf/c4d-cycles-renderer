// SPDX-License-Identifier: Apache-2.0
// The "Cycles Native Material": a Blender material kept as Blender's own data and edited in
// Blender's own Shader Editor.
//
// Nothing of it is translated. The material holds a .blend file's bytes that contain one
// material (written by the bridge: bridge/c4d_bridge/scene/native.py); rendering hands the bytes
// to Blender, and the button opens a window that is only a Shader Editor on it, in a Blender
// kept for editing (not the one the IPR renders in). The bridge writes every change into a
// folder, with a small picture of a ball wearing the material; a timer here takes both into
// the material (native_material.cpp, "Editing"). So the scene file carries the material, and
// closing the editor loses nothing.

#ifndef CYCLESRENDER_NATIVE_MATERIAL_H__
#define CYCLESRENDER_NATIVE_MATERIAL_H__

#include "c4d.h"
#include "c4d_materialdata.h"

namespace cyclesrender
{

class NativeMaterial : public cinema::MaterialData
{
	INSTANCEOF(NativeMaterial, cinema::MaterialData)

public:
	static cinema::NodeData* Alloc();
	/// The data of @p material, or nullptr when it is not a Cycles Native Material.
	static NativeMaterial* Get(cinema::BaseMaterial* material);
	static const NativeMaterial* Get(const cinema::BaseMaterial* material);

	/// The .blend bytes; empty until the material has been edited (Blender's default material then).
	const maxon::BaseArray<maxon::Char>& GetBlend() const { return _blend; }
	/// The id of the material's item in a scene request, and the name of its files while it is edited.
	maxon::String GetItemId() const;
	maxon::UInt64 GetKey() const { return _key; }
	/// For tests: the change Blender is to make after the next "open" (see CYCLESNATIVE_TEST_CHANGE); asked once.
	maxon::String TakeTestChange() { maxon::String change = _testChange; _testChange = maxon::String(); return change; }
	/// Gives the material a key of its own; a copy starts with the key of its original.
	void NewKey();

	maxon::Result<void> SetBlend(cinema::BaseMaterial* material, const maxon::Char* data, maxon::Int count);
	/// The picture of a ball wearing the material: 8-bit RGB as displayed, top row first.
	maxon::Result<void> SetPreview(cinema::BaseMaterial* material, maxon::Int32 width, maxon::Int32 height, const maxon::UChar* rgb);

	virtual cinema::Bool Init(cinema::GeListNode* node, cinema::Bool isCloneInit) override;
	virtual cinema::Bool Read(cinema::GeListNode* node, cinema::HyperFile* hf, cinema::Int32 level) override;
	virtual cinema::Bool Write(const cinema::GeListNode* node, cinema::HyperFile* hf) const override;
	virtual cinema::Bool CopyTo(cinema::NodeData* dest, const cinema::GeListNode* snode, cinema::GeListNode* dnode, cinema::COPYFLAGS flags,
		cinema::AliasTrans* trn) const override;
	virtual cinema::Bool Message(cinema::GeListNode* node, cinema::Int32 type, void* data) override;
	virtual cinema::Bool GetDParameter(const cinema::GeListNode* node, const cinema::DescID& id, cinema::GeData& t_data, cinema::DESCFLAGS_GET& flags) const override;
	virtual cinema::Bool SetDParameter(cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& t_data, cinema::DESCFLAGS_SET& flags) override;

	virtual cinema::VOLUMEINFO GetRenderInfo(cinema::BaseMaterial* mat) override;
	virtual cinema::INITRENDERRESULT InitRender(cinema::BaseMaterial* mat, const cinema::InitRenderStruct& irs) override;
	virtual void FreeRender(cinema::BaseMaterial* mat) override;
	virtual void CalcSurface(cinema::BaseMaterial* mat, cinema::VolumeData* vd) override;

private:
	maxon::BaseArray<maxon::Char> _blend;
	maxon::BaseArray<maxon::UChar> _preview;        ///< _previewWidth x _previewHeight x 3
	maxon::Int32 _previewWidth = 0;
	maxon::Int32 _previewHeight = 0;
	maxon::UInt64 _key = 0;
	maxon::String _testChange;                      ///< for tests: handed to the bridge with the next "open"

	// A copy for the renderer: the preview may be replaced while Cinema 4D renders with it.
	maxon::BaseArray<maxon::Vector32> _renderPreview;
	maxon::Int32 _renderWidth = 0;
	maxon::Int32 _renderHeight = 0;
};

cinema::Bool RegisterNativeMaterial();

/// Opens Blender's Shader Editor on @p material, in a Blender kept for editing (started if it is not running).
maxon::Result<void> OpenNativeMaterialEditor(cinema::BaseMaterial* material);
/// Takes in what Blender wrote for the materials that are open there. Called by a timer four
/// times a second; tests without a message loop call it themselves.
void NativeMaterialEditTick();
/// Forgets the materials being edited and ends the editor's Blender. Called at shutdown.
void FreeNativeMaterialEditing();
/// For tests: "free" does the above, "kill" ends the editor's Blender as a crash would.
void NativeMaterialEditorTestCommand(const maxon::String& command);

} // namespace cyclesrender

#endif // CYCLESRENDER_NATIVE_MATERIAL_H__
