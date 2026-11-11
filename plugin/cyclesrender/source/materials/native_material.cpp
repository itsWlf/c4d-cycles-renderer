// SPDX-License-Identifier: Apache-2.0
// The Cycles Native Material (native_material.h).
//
// In this file, in order: the material type (its data, saving, loading and copying, the
// button, and how Cinema 4D's own renderers show it, which is by Blender's preview picture);
// "Editing" (the Blender kept for the Shader Editor, and the timer that takes in what was
// edited there); and the command that makes a new material.

#include "c4d.h"
#include "c4d_basematerial.h"
#include "c4d_commanddata.h"
#include "c4d_messagedata.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "maxon/configuration.h"
#include "mcyclesnative.h"

#include "../ids.h"
#include "../ipr/ipr_worker.h"
#include "../session/json_writer.h"
#include "../videopost/render_settings_map.h"
#include "native_material.h"

MAXON_CONFIGURATION_BOOL(g_cyclesRenderNativeMaterial, false, maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: offer the Cycles Native Material (a material edited in Blender's Shader Editor), which is switched off in this release.");

namespace cyclesrender
{

// Not in the description: for scripts and tests.
static const maxon::Int32 CYCLESNATIVE_IMPORT = 2001;       ///< set: path of a .blend whose bytes become the material's
static const maxon::Int32 CYCLESNATIVE_TEST_CHANGE = 2002;  ///< set: a colour as "[r, g, b]" Blender applies after the next "open"
static const maxon::Int32 CYCLESNATIVE_STATE = 2003;        ///< get: "bytes=<n> preview=<w>x<h> editing=<0|1> id=<item id>"

//----------------------------------------------------------------------------------------
// Files
//----------------------------------------------------------------------------------------
static maxon::Bool ReadFile(const cinema::Filename& name, maxon::BaseArray<maxon::Char>& data)
{
	data.Reset();
	cinema::AutoAlloc<cinema::BaseFile> file;
	if (!file || !cinema::GeFExist(name) || !file->Open(name, cinema::FILEOPEN::READ, cinema::FILEDIALOG::NONE))
		return false;
	const maxon::Int64 length = file->GetLength();
	if (length <= 0)
		return true;
	iferr (data.Resize(maxon::Int(length)))
		return false;
	return file->ReadBytes(data.GetFirst(), maxon::Int(length)) == maxon::Int(length);
}

static maxon::Bool WriteFile(const cinema::Filename& name, const maxon::BaseArray<maxon::Char>& data)
{
	cinema::AutoAlloc<cinema::BaseFile> file;
	if (!file || !file->Open(name, cinema::FILEOPEN::WRITE, cinema::FILEDIALOG::NONE))
		return false;
	return data.IsEmpty() || file->WriteBytes(data.GetFirst(), data.GetCount());
}

/// Where the bridge writes the materials that are being edited.
static cinema::Filename EditFolder()
{
	return cinema::GeGetC4DPath(C4D_PATH_PREFS) + cinema::Filename("cyclesrender"_s) + cinema::Filename("edit"_s);
}

//----------------------------------------------------------------------------------------
// The material
//----------------------------------------------------------------------------------------
cinema::NodeData* NativeMaterial::Alloc()
{
	iferr (NativeMaterial* const data = NewObj(NativeMaterial))
		return nullptr;
	return data;
}

NativeMaterial* NativeMaterial::Get(cinema::BaseMaterial* material)
{
	if (!material || material->GetType() != ID_NATIVE_MATERIAL)
		return nullptr;
	return material->GetNodeData<NativeMaterial>();
}

const NativeMaterial* NativeMaterial::Get(const cinema::BaseMaterial* material)
{
	return Get(const_cast<cinema::BaseMaterial*>(material));
}

maxon::String NativeMaterial::GetItemId() const
{
	return "native_"_s + maxon::String::HexToString(_key, false);
}

void NativeMaterial::NewKey()
{
	static maxon::AtomicInt32 counter;
	const maxon::UInt64 time = maxon::UInt64(maxon::TimeValue::GetTime().GetMicroseconds());
	const maxon::UInt64 count = maxon::UInt64(counter.SwapIncrement());
	_key = (time * 0x9E3779B97F4A7C15ull) ^ (maxon::UInt64(reinterpret_cast<maxon::UInt>(this)) << 7) ^ (count * 0xC2B2AE3D27D4EB4Full);
	if (_key == 0)
		_key = 1;
}

maxon::Result<void> NativeMaterial::SetBlend(cinema::BaseMaterial* material, const maxon::Char* data, maxon::Int count)
{
	iferr_scope;
	_blend.Resize(count) iferr_return;
	if (count > 0)
		maxon::MemCopyType(_blend.GetFirst(), data, count);
	if (material)
		material->SetDirty(cinema::DIRTYFLAGS::DATA);
	return maxon::OK;
}

maxon::Result<void> NativeMaterial::SetPreview(cinema::BaseMaterial* material, maxon::Int32 width, maxon::Int32 height, const maxon::UChar* rgb)
{
	iferr_scope;
	const maxon::Int count = maxon::Int(width) * height * 3;
	_preview.Resize(count) iferr_return;
	if (count > 0)
		maxon::MemCopyType(_preview.GetFirst(), rgb, count);
	_previewWidth = width;
	_previewHeight = height;
	if (material)
	{
		// Update() alone does not render the thumbnail anew; a changed material does.
		material->SetDirty(cinema::DIRTYFLAGS::DATA);
		material->Message(MSG_UPDATE);
		material->Update(true, true);
	}
	return maxon::OK;
}

cinema::Bool NativeMaterial::Init(cinema::GeListNode* node, cinema::Bool isCloneInit)
{
	if (!isCloneInit)
		NewKey();
	return true;
}

cinema::Bool NativeMaterial::Write(const cinema::GeListNode* node, cinema::HyperFile* hf) const
{
	if (!hf->WriteUInt64(_key) || !hf->WriteInt64(_blend.GetCount()))
		return false;
	if (!_blend.IsEmpty() && !hf->WriteMemory(_blend.GetFirst(), _blend.GetCount()))
		return false;
	if (!hf->WriteInt32(_previewWidth) || !hf->WriteInt32(_previewHeight) || !hf->WriteInt64(_preview.GetCount()))
		return false;
	if (!_preview.IsEmpty() && !hf->WriteMemory(_preview.GetFirst(), _preview.GetCount()))
		return false;
	return SUPER::Write(node, hf);
}

template <typename T> static maxon::Bool ReadArray(cinema::HyperFile* hf, maxon::BaseArray<T>& array)
{
	maxon::Int64 count = 0;
	if (!hf->ReadInt64(&count))
		return false;
	array.Reset();
	if (count <= 0)
		return true;
	void* data = nullptr;
	maxon::Int size = 0;
	if (!hf->ReadMemory(&data, &size))
		return false;
	maxon::Bool ok = size == maxon::Int(count);
	if (ok)
	{
		iferr (array.Resize(size))
			ok = false;
	}
	if (ok)
		maxon::MemCopyType(array.GetFirst(), reinterpret_cast<const T*>(data), size);
	DeleteMem(data);
	return ok;
}

cinema::Bool NativeMaterial::Read(cinema::GeListNode* node, cinema::HyperFile* hf, cinema::Int32 level)
{
	if (!hf->ReadUInt64(&_key) || !ReadArray(hf, _blend))
		return false;
	if (!hf->ReadInt32(&_previewWidth) || !hf->ReadInt32(&_previewHeight) || !ReadArray(hf, _preview))
		return false;
	if (_preview.GetCount() != maxon::Int(_previewWidth) * _previewHeight * 3)
	{
		_preview.Reset();
		_previewWidth = _previewHeight = 0;
	}
	return SUPER::Read(node, hf, level);
}

cinema::Bool NativeMaterial::CopyTo(cinema::NodeData* dest, const cinema::GeListNode* snode, cinema::GeListNode* dnode, cinema::COPYFLAGS flags,
	cinema::AliasTrans* trn) const
{
	NativeMaterial* const copy = static_cast<NativeMaterial*>(dest);
	iferr (copy->_blend.CopyFrom(_blend))
		return false;
	iferr (copy->_preview.CopyFrom(_preview))
		return false;
	copy->_previewWidth = _previewWidth;
	copy->_previewHeight = _previewHeight;
	// A copy keeps the key: a render works on a copy of the document, and must name the material
	// as the document does. Two materials of one document with one key are told apart when one
	// of them is opened in the editor (OpenNativeMaterialEditor) and by the exporter.
	copy->_key = _key;
	return SUPER::CopyTo(dest, snode, dnode, flags, trn);
}

static maxon::Bool IsBeingEdited(const maxon::String& itemId);

cinema::Bool NativeMaterial::GetDParameter(const cinema::GeListNode* node, const cinema::DescID& id, cinema::GeData& t_data, cinema::DESCFLAGS_GET& flags) const
{
	switch (id[0].id)
	{
		case CYCLESNATIVE_INFO:
		{
			maxon::String text;
			if (_blend.IsEmpty())
				text = "Not edited yet: Blender's default material."_s;
			else
				text = FormatString("Blender material stored: @ KB.", (_blend.GetCount() + 512) / 1024);
			if (IsBeingEdited(GetItemId()))
				text += " Open in Blender; changes are taken in as you make them."_s;
			t_data = cinema::GeData(text);
			flags |= cinema::DESCFLAGS_GET::PARAM_GET;
			return true;
		}
		case CYCLESNATIVE_STATE:
		{
			t_data = cinema::GeData(FormatString("bytes=@ preview=@x@ editing=@ id=@", _blend.GetCount(), _previewWidth, _previewHeight,
				IsBeingEdited(GetItemId()) ? 1 : 0, GetItemId()));
			flags |= cinema::DESCFLAGS_GET::PARAM_GET;
			return true;
		}
		default: break;
	}
	return SUPER::GetDParameter(node, id, t_data, flags);
}

cinema::Bool NativeMaterial::SetDParameter(cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& t_data, cinema::DESCFLAGS_SET& flags)
{
	switch (id[0].id)
	{
		case CYCLESNATIVE_IMPORT:
		{
			maxon::BaseArray<maxon::Char> data;
			if (!ReadFile(cinema::Filename(t_data.GetString()), data))
				return false;
			iferr (SetBlend(static_cast<cinema::BaseMaterial*>(node), data.GetFirst(), data.GetCount()))
				return false;
			flags |= cinema::DESCFLAGS_SET::PARAM_SET;
			return true;
		}
		case CYCLESNATIVE_TEST_CHANGE:
		{
			_testChange = t_data.GetString();
			flags |= cinema::DESCFLAGS_SET::PARAM_SET;
			return true;
		}
		default: break;
	}
	return SUPER::SetDParameter(node, id, t_data, flags);
}

cinema::Bool NativeMaterial::Message(cinema::GeListNode* node, cinema::Int32 type, void* data)
{
	if (type == MSG_DESCRIPTION_COMMAND && data)
	{
		const cinema::DescriptionCommand* const command = static_cast<const cinema::DescriptionCommand*>(data);
		if (command->_descId[0].id == CYCLESNATIVE_OPEN)
		{
			iferr (OpenNativeMaterialEditor(static_cast<cinema::BaseMaterial*>(node)))
			{
				ApplicationOutput("Cycles Render: Blender's Shader Editor could not be opened. @", err.GetMessage());
				cinema::StatusSetText("Cycles Render: Blender's Shader Editor could not be opened. "_s + err.GetMessage());
			}
		}
	}
	return SUPER::Message(node, type, data);
}

//----------------------------------------------------------------------------------------
// What Cinema 4D itself shows of the material (its thumbnail, the Standard renderer): the
// picture of the ball Blender rendered, looked up by the direction the surface faces. On
// Cinema 4D's own preview ball that is Blender's picture again.
//----------------------------------------------------------------------------------------
cinema::VOLUMEINFO NativeMaterial::GetRenderInfo(cinema::BaseMaterial* mat)
{
	return cinema::VOLUMEINFO::NONE;
}

// What a material shows before Blender has sent a picture of it: Blender's preview of its
// default material, the un-edited node setup (res/native_default.preview, made by
// tools/make_native_placeholder.py in the format the bridge writes previews in).
struct PlaceholderPreview
{
	maxon::BaseArray<maxon::UChar> rgb;
	maxon::Int32 width = 0;
	maxon::Int32 height = 0;
};

static PlaceholderPreview g_placeholder;   // loaded once, when the material type is registered

static void LoadPlaceholderPreview()
{
	maxon::BaseArray<maxon::Char> data;
	if (!ReadFile(cinema::GeGetPluginResourcePath() + cinema::Filename("native_default.preview"_s), data) || data.GetCount() <= 8)
		return;
	maxon::Int32 size[2];
	maxon::MemCopyType(reinterpret_cast<maxon::Char*>(size), data.GetFirst(), 8);
	if (size[0] <= 0 || size[1] <= 0 || data.GetCount() != 8 + maxon::Int(size[0]) * size[1] * 3)
		return;
	iferr (g_placeholder.rgb.Resize(data.GetCount() - 8))
		return;
	maxon::MemCopyType(g_placeholder.rgb.GetFirst(), reinterpret_cast<const maxon::UChar*>(data.GetFirst() + 8), data.GetCount() - 8);
	g_placeholder.width = size[0];
	g_placeholder.height = size[1];
}

cinema::INITRENDERRESULT NativeMaterial::InitRender(cinema::BaseMaterial* mat, const cinema::InitRenderStruct& irs)
{
	_renderPreview.Reset();
	_renderWidth = _renderHeight = 0;
	// The material's own picture, or the placeholder until the first one has come from Blender.
	const maxon::Bool own = _previewWidth > 0 && _previewHeight > 0 && _preview.GetCount() == maxon::Int(_previewWidth) * _previewHeight * 3;
	const maxon::UChar* const rgb = own ? _preview.GetFirst() : g_placeholder.rgb.GetFirst();
	const maxon::Int32 width = own ? _previewWidth : g_placeholder.width;
	const maxon::Int32 height = own ? _previewHeight : g_placeholder.height;
	const maxon::Int count = maxon::Int(width) * height;
	if (count <= 0 || !rgb)
		return cinema::INITRENDERRESULT::OK;
	iferr (_renderPreview.Resize(count))
		return cinema::INITRENDERRESULT::OUTOFMEMORY;
	for (maxon::Int i = 0; i < count; ++i)
	{
		// The picture is as displayed; the renderer wants linear light.
		_renderPreview[i] = maxon::Vector32(maxon::Float32(maxon::Pow(rgb[i * 3] / 255.0, 2.2)), maxon::Float32(maxon::Pow(rgb[i * 3 + 1] / 255.0, 2.2)),
			maxon::Float32(maxon::Pow(rgb[i * 3 + 2] / 255.0, 2.2)));
	}
	_renderWidth = width;
	_renderHeight = height;
	return cinema::INITRENDERRESULT::OK;
}

void NativeMaterial::FreeRender(cinema::BaseMaterial* mat)
{
	_renderPreview.Reset();
	_renderWidth = _renderHeight = 0;
}

void NativeMaterial::CalcSurface(cinema::BaseMaterial* mat, cinema::VolumeData* vd)
{
	const cinema::Vector view = vd->ray->v;
	const cinema::Vector normal = cinema::Vector(vd->bumpn);
	if (_renderWidth <= 0)
	{
		// No picture at all (the placeholder file is missing): a plain grey.
		vd->col = cinema::Vector(0.05 + 0.4 * maxon::Abs(Dot(view, normal)));
		return;
	}
	// The picture's axes: right and up as the eye sees them.
	cinema::Vector right = Cross(cinema::Vector(0.0, 1.0, 0.0), view);
	if (right.GetSquaredLength() < 1e-8)
		right = cinema::Vector(1.0, 0.0, 0.0);
	right.Normalize();
	const cinema::Vector up = Cross(view, right);
	// The ball fills the picture; stay a little inside its rim.
	const maxon::Float x = maxon::Clamp01(0.5 + 0.48 * Dot(normal, right));
	const maxon::Float y = maxon::Clamp01(0.5 - 0.48 * Dot(normal, up));
	const maxon::Int column = maxon::Min(maxon::Int(x * _renderWidth), maxon::Int(_renderWidth - 1));
	const maxon::Int row = maxon::Min(maxon::Int(y * _renderHeight), maxon::Int(_renderHeight - 1));
	const maxon::Vector32& colour = _renderPreview[row * _renderWidth + column];
	vd->col = cinema::Vector(colour.x, colour.y, colour.z);
}

//----------------------------------------------------------------------------------------
// Editing: which materials are open in Blender, and the timer that takes their changes in.
//
// Blender cannot send a message nobody asked for. The bridge writes, per material, into the
// edit folder: <id>.blend (the material), <id>.preview (the ball) and <id>.state, which counts
// the versions of both and says when the window was closed. Files are replaced whole.
//----------------------------------------------------------------------------------------
// The editor has a Blender of its own, apart from the one the IPR renders in: with both in
// one Blender the IPR's picture went dark when the editor opened (cause not found).
// It is started with the first "open" and stays, hidden, for the next; its windows are the
// editors. The IPR learns of an edit as of any change in Cinema 4D: the material's bytes
// changed, so the material is sent again.
static IprWorker* g_editor = nullptr;   // main thread only

static maxon::Bool IsEditorRunning()
{
	if (!g_editor)
		return false;
	maxon::String token;
	maxon::Int processId = 0;
	return g_editor->GetSession(token, processId) || g_editor->GetPendingCount() > 0;
}

/// Sends a request to the editor's Blender, starting it first if it is not running.
static maxon::Result<void> PostToEditor(const JsonWriter& request)
{
	iferr_scope;
	if (!g_editor)
	{
		g_editor = NewObj(IprWorker) iferr_return;
		if (!g_editor->Start())
		{
			DeleteObj(g_editor);
			return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "the thread that talks to Blender could not be started"_s);
		}
	}
	if (!IsEditorRunning())
	{
		g_editor->ClearFailure();
		IprTask* const open = NewObj(IprTask) iferr_return;
		open->kind = IprTask::KIND::OPEN;
		g_editor->Post(open);
	}
	g_editor->PostRequest(request);
	return maxon::OK;
}

struct EditSession
{
	maxon::String itemId;
	maxon::Int32 blend = 0;       ///< versions taken in so far
	maxon::Int32 preview = 0;
	maxon::Int32 idleTicks = 0;   ///< ticks without Blender, before the session is given up
};

static maxon::BaseArray<EditSession> g_sessions;   // main thread only

static maxon::Bool IsBeingEdited(const maxon::String& itemId)
{
	for (const EditSession& session : g_sessions)
	{
		if (session.itemId == itemId)
			return true;
	}
	return false;
}

static cinema::BaseMaterial* FindMaterial(const maxon::String& itemId)
{
	for (cinema::BaseDocument* doc = cinema::GetFirstDocument(); doc; doc = doc->GetNext())
	{
		for (cinema::BaseMaterial* material = doc->GetFirstMaterial(); material; material = material->GetNext())
		{
			const NativeMaterial* const native = NativeMaterial::Get(material);
			if (native && native->GetItemId() == itemId)
				return material;
		}
	}
	return nullptr;
}

static maxon::Int32 StateValue(const maxon::String& state, const maxon::Char* key)
{
	maxon::Int position = 0;
	const maxon::String wanted = maxon::String(key) + "="_s;
	if (!state.Find(wanted, &position))
		return 0;
	maxon::Int32 value = 0;
	for (maxon::Int i = position + wanted.GetLength(); i < state.GetLength(); ++i)
	{
		const maxon::Utf32Char c = state[i];
		if (c < '0' || c > '9')
			break;
		value = value * 10 + maxon::Int32(c - '0');
	}
	return value;
}

/// Takes in what the bridge wrote for one material.
/// @param[out] gone  Set when the material is in no open document any more (deleted, or its document closed).
/// @return  False when the session is over.
static maxon::Bool TakeChanges(EditSession& session, maxon::Bool& changed, maxon::Bool& gone)
{
	cinema::BaseMaterial* const material = FindMaterial(session.itemId);
	NativeMaterial* const native = NativeMaterial::Get(material);
	if (!native)
	{
		gone = true;
		return false;
	}

	const cinema::Filename folder = EditFolder();
	maxon::BaseArray<maxon::Char> data;
	if (!ReadFile(folder + cinema::Filename(session.itemId + ".state"_s), data) || data.IsEmpty())
		return true;   // nothing written yet
	maxon::String state;
	iferr (state.SetCString(data.GetFirst(), data.GetCount()))
		return true;
	const maxon::Int32 blend = StateValue(state, "blend");
	const maxon::Int32 preview = StateValue(state, "preview");
	const maxon::Bool closed = StateValue(state, "closed") != 0;

	if (blend != session.blend)
	{
		if (ReadFile(folder + cinema::Filename(session.itemId + ".blend"_s), data) && !data.IsEmpty())
		{
			iferr (native->SetBlend(material, data.GetFirst(), data.GetCount()))
				return true;
			session.blend = blend;
			changed = true;
		}
	}
	if (preview != session.preview)
	{
		if (ReadFile(folder + cinema::Filename(session.itemId + ".preview"_s), data) && data.GetCount() > 8)
		{
			maxon::Int32 size[2];
			maxon::MemCopyType(reinterpret_cast<maxon::Char*>(size), data.GetFirst(), 8);
			if (size[0] > 0 && size[1] > 0 && size[0] <= 4096 && size[1] <= 4096 && data.GetCount() == 8 + maxon::Int(size[0]) * size[1] * 3)
			{
				iferr (native->SetPreview(material, size[0], size[1], reinterpret_cast<const maxon::UChar*>(data.GetFirst() + 8)))
					return true;
				session.preview = preview;
				changed = true;
			}
		}
	}
	return !closed || blend != session.blend;
}

void NativeMaterialEditTick()
{
	if (g_sessions.IsEmpty())
		return;
	const maxon::Bool running = IsEditorRunning();
	maxon::Bool changed = false;
	for (maxon::Int i = g_sessions.GetCount() - 1; i >= 0; --i)
	{
		EditSession& session = g_sessions[i];
		maxon::Bool gone = false;
		maxon::Bool keep = TakeChanges(session, changed, gone);
		if (gone && running)
		{
			// Nothing is left to take the edits in: close the material's window in Blender.
			JsonWriter request;
			request.BeginObject();
			request.String("op", "close_material_editor"_s);
			request.String("material", session.itemId);
			iferr (PostToEditor(request))
			{
			}
		}
		// Blender gone (quit, crashed, restarted from the IPR window): what it wrote last has
		// just been taken in; give it a moment in case it is only starting.
		session.idleTicks = running ? 0 : session.idleTicks + 1;
		if (session.idleTicks > 40)
			keep = false;
		if (!keep)
		{
			iferr (g_sessions.Erase(i))
			{
			}
			changed = true;
		}
	}
	if (changed)
		cinema::EventAdd();
}

class NativeEditMessage : public cinema::MessageData
{
public:
	virtual cinema::Int32 GetTimer() override { return 250; }
	virtual cinema::Bool CoreMessage(cinema::Int32 id, const cinema::BaseContainer& bc) override
	{
		if (id == MSG_TIMER)
			NativeMaterialEditTick();
		return true;
	}
};

maxon::Result<void> OpenNativeMaterialEditor(cinema::BaseMaterial* material)
{
	iferr_scope;
	NativeMaterial* const native = NativeMaterial::Get(material);
	if (!native)
		return maxon::IllegalArgumentError(MAXON_SOURCE_LOCATION, "not a Cycles Native Material"_s);

	// A copy made in the Material Manager has the key of its original: give this one its own,
	// or both would be one material in Blender.
	if (cinema::BaseDocument* const doc = material->GetDocument())
	{
		for (cinema::BaseMaterial* other = doc->GetFirstMaterial(); other; other = other->GetNext())
		{
			const NativeMaterial* const otherNative = NativeMaterial::Get(other);
			if (other != material && otherNative && otherNative->GetKey() == native->GetKey())
			{
				native->NewKey();
				material->SetDirty(cinema::DIRTYFLAGS::DATA);
				break;
			}
		}
	}
	const maxon::String itemId = native->GetItemId();

	// What Blender starts from, if it does not hold the material already.
	const cinema::Filename folder = EditFolder();
	if (!cinema::GeFExist(folder, true) && !cinema::GeFCreateDirRec(folder))
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "the folder for edited materials could not be made: "_s + maxon::String(folder.GetString()));
	cinema::GeFKill(folder + cinema::Filename(itemId + ".state"_s));
	cinema::GeFKill(folder + cinema::Filename(itemId + ".preview"_s));
	const cinema::Filename blend = folder + cinema::Filename(itemId + ".blend"_s);
	if (native->GetBlend().IsEmpty())
		cinema::GeFKill(blend);
	else if (!WriteFile(blend, native->GetBlend()))
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "the material could not be written for Blender: "_s + maxon::String(blend.GetString()));

	// The editor's Blender must work in the document's colour space before the material is made
	// or loaded there: a colour chosen in the editor is a colour in Blender's working space, and
	// the Blenders that render work in the document's.
	{
		cinema::BaseDocument* const doc = material->GetDocument();
		JsonWriter settings;
		maxon::BaseArray<maxon::String> warnings;
		WriteRenderSettings(settings, GetDocumentRenderSettings(doc), doc, NOTOK, warnings);
		PostToEditor(settings) iferr_return;
	}

	JsonWriter request;
	request.BeginObject();
	request.String("op", "edit_material"_s);
	request.String("material", itemId);
	request.String("folder", folder.GetString());
	request.String("name", material->GetName());
	const maxon::String testChange = native->TakeTestChange();
	if (!testChange.IsEmpty())
		request.Raw("test_change", testChange);
	PostToEditor(request) iferr_return;

	for (EditSession& session : g_sessions)
	{
		if (session.itemId == itemId)
		{
			session.blend = session.preview = session.idleTicks = 0;
			return maxon::OK;
		}
	}
	EditSession session;
	session.itemId = itemId;
	g_sessions.Append(std::move(session)) iferr_return;
	return maxon::OK;
}

void FreeNativeMaterialEditing()
{
	g_sessions.Reset();
	DeleteObj(g_editor);   // ends the thread and the editor's Blender
}

void NativeMaterialEditorTestCommand(const maxon::String& command)
{
	if (command == "kill"_s && g_editor)
		g_editor->KillBlenderForTest();
	else if (command == "free"_s)
		FreeNativeMaterialEditing();
}

// "Cycles Native Material" in the Extensions menu and the Commander: makes one in the active
// document and selects it, so the Attribute Manager shows its button. Commands have ids of
// their own kind, so the material's id serves (as the renderer's does for the IPR command).
class NativeMaterialCommand : public cinema::CommandData
{
public:
	virtual cinema::Bool Execute(cinema::BaseDocument* doc, cinema::GeDialog* parentManager) override
	{
		if (!doc)
			return false;
		cinema::BaseMaterial* const material = cinema::BaseMaterial::Alloc(ID_NATIVE_MATERIAL);
		if (!material)
			return false;
		material->SetName(cinema::GeLoadString(IDS_NATIVE_MATERIAL_NAME));
		doc->StartUndo();
		doc->InsertMaterial(material);
		doc->AddUndo(cinema::UNDOTYPE::NEWOBJ, material);
		doc->EndUndo();
		doc->SetActiveMaterial(material);
		cinema::EventAdd();
		return true;
	}
};

cinema::Bool RegisterNativeMaterial()
{
	// Switched off for users (docs/roadmap.md, "Open bugs": what is edited in Blender does not
	// always arrive). Without the setting there is no command, and the material type is hidden
	// from the menus; it stays registered, so that a scene holding such a material still loads
	// and renders.
	const maxon::Bool enabled = g_cyclesRenderNativeMaterial;
	if (enabled)
	{
		iferr (NativeMaterialCommand* const command = NewObj(NativeMaterialCommand))
			return false;
		cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
		if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("cyclesrender_grey.png"_s)) != cinema::IMAGERESULT::OK)
			cinema::BaseBitmap::Free(icon);
		const cinema::Bool commandRegistered = cinema::RegisterCommandPlugin(ID_NATIVE_MATERIAL, cinema::GeLoadString(IDS_NATIVE_MATERIAL_NAME), 0, icon,
			"Create a material that is edited in Blender's Shader Editor"_s, command);
		cinema::BaseBitmap::Free(icon);
		if (!commandRegistered)
			ApplicationOutput("Cycles Render: the command that makes a Cycles Native Material could not be registered.");
	}

	iferr (NativeEditMessage* const message = NewObj(NativeEditMessage))
		return false;
	if (!cinema::RegisterMessagePlugin(ID_NATIVE_MATERIAL_EDITS, maxon::String(), 0, message))
		return false;
	LoadPlaceholderPreview();
	return cinema::RegisterMaterialPlugin(ID_NATIVE_MATERIAL, cinema::GeLoadString(IDS_NATIVE_MATERIAL_NAME), enabled ? 0 : PLUGINFLAG_HIDE,
		NativeMaterial::Alloc, "Mcyclesnative"_s, 0);
}

} // namespace cyclesrender
