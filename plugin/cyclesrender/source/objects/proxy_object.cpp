// SPDX-License-Identifier: Apache-2.0
// The Cycles Proxy object (proxy_object.h). Reading order: Init (the defaults), the Attribute Manager
// (the menu of what the file holds, the status line), the buttons and the asset messages, then the
// box in the viewport.

#include "c4d.h"
#include "c4d_objectdata.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"

#include "ocyclesproxy.h"

#include "../ids.h"
#include "../nodespace/path_commands.h"
#include "../scene/proxy_export.h"
#include "proxy_object.h"

namespace cyclesrender
{

class ProxyObject : public cinema::ObjectData
{
	INSTANCEOF(ProxyObject, cinema::ObjectData)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(ProxyObject); }

	virtual cinema::Bool Init(cinema::GeListNode* node, cinema::Bool isCloneInit) override
	{
		cinema::BaseList2D* const item = static_cast<cinema::BaseList2D*>(node);
		if (!item)
			return false;
		if (isCloneInit)
			return true;
		// No file; the file's scene, with its lights, at the document's frame.
		cinema::BaseContainer& data = item->GetDataInstanceRef();
		data.SetFilename(CYCLESPROXY_FILE, cinema::Filename());
		data.SetInt32(CYCLESPROXY_TAKE_KIND, CYCLESPROXY_TAKE_KIND_FILE_SCENE);
		data.SetString(CYCLESPROXY_TAKE_NAME, maxon::String());
		data.SetBool(CYCLESPROXY_LIGHTS, true);
		data.SetInt32(CYCLESPROXY_FRAME_OFFSET, 0);
		return true;
	}

	// The menu's entries: the file's scene, then the scenes and collections a Blender read from the file,
	// then, when the object names one that is not among them, that one.
	virtual cinema::Bool GetDDescription(const cinema::GeListNode* node, cinema::Description* description, cinema::DESCFLAGS_DESC& flags) const override
	{
		if (!node || !description || !description->LoadDescription(node->GetType()))
			return false;
		flags |= cinema::DESCFLAGS_DESC::LOADED;
		const cinema::BaseObject* const op = static_cast<const cinema::BaseObject*>(node);
		if (cinema::BaseContainer* const parameter = description->GetParameterI(CreateDescID(cinema::DescLevel(CYCLESPROXY_TAKE)), nullptr))
		{
			iferr (const ProxyContent content = GetProxyContent(op))
				return false;
			// The first entry keeps the name the description gives it.
			cinema::BaseContainer cycle = parameter->GetContainer(cinema::DESC_CYCLE);
			maxon::Int32 index = 1;
			for (const ProxyContentEntry& entry : content.entries)
				cycle.SetString(index++, EntryLabel(entry.collection, entry.name));
			const cinema::BaseContainer& data = op->GetDataInstanceRef();
			if (TakeIndex(op, content) == index)
			{
				cycle.SetString(index, EntryLabel(data.GetInt32(CYCLESPROXY_TAKE_KIND) == CYCLESPROXY_TAKE_KIND_COLLECTION, data.GetString(CYCLESPROXY_TAKE_NAME))
					+ " (not in the list)"_s);
			}
			parameter->SetContainer(cinema::DESC_CYCLE, cycle);
		}
		return SUPER::GetDDescription(node, description, flags);
	}

	// The menu shows what the stored kind and name say; the status line is made when asked for.
	virtual cinema::Bool GetDParameter(const cinema::GeListNode* node, const cinema::DescID& id, cinema::GeData& data, cinema::DESCFLAGS_GET& flags) const override
	{
		const cinema::BaseObject* const op = static_cast<const cinema::BaseObject*>(node);
		const maxon::Int32 parameter = id.GetDepth() > 0 ? id[0].id : 0;
		if (op && parameter == CYCLESPROXY_TAKE)
		{
			iferr (const ProxyContent content = GetProxyContent(op))
				return false;
			data = cinema::GeData(TakeIndex(op, content));
			flags |= cinema::DESCFLAGS_GET::PARAM_GET;
			return true;
		}
		if (op && parameter == CYCLESPROXY_STATUS)
		{
			data = cinema::GeData(DescribeProxyContent(op, op->GetDocument()));
			flags |= cinema::DESCFLAGS_GET::PARAM_GET;
			return true;
		}
		return SUPER::GetDParameter(node, id, data, flags);
	}

	virtual cinema::Bool SetDParameter(cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data, cinema::DESCFLAGS_SET& flags) override
	{
		cinema::BaseObject* const op = static_cast<cinema::BaseObject*>(node);
		const maxon::Int32 parameter = id.GetDepth() > 0 ? id[0].id : 0;
		if (op && parameter == CYCLESPROXY_TAKE)
		{
			iferr (const ProxyContent content = GetProxyContent(op))
				return false;
			const maxon::Int32 index = data.GetInt32();
			cinema::BaseContainer& stored = op->GetDataInstanceRef();
			if (index == CYCLESPROXY_TAKE_FILE_SCENE)
			{
				stored.SetInt32(CYCLESPROXY_TAKE_KIND, CYCLESPROXY_TAKE_KIND_FILE_SCENE);
				stored.SetString(CYCLESPROXY_TAKE_NAME, maxon::String());
			}
			else if (index >= 1 && index <= content.entries.GetCount())
			{
				const ProxyContentEntry& entry = content.entries[index - 1];
				stored.SetInt32(CYCLESPROXY_TAKE_KIND, entry.collection ? CYCLESPROXY_TAKE_KIND_COLLECTION : CYCLESPROXY_TAKE_KIND_SCENE);
				stored.SetString(CYCLESPROXY_TAKE_NAME, entry.name);
			}
			// (Any other position is the entry of a name that is not in the list: it stays.)
			flags |= cinema::DESCFLAGS_SET::PARAM_SET;
			return true;
		}
		if (parameter == CYCLESPROXY_STATUS)
		{
			flags |= cinema::DESCFLAGS_SET::PARAM_SET; // Read-only.
			return true;
		}
		return SUPER::SetDParameter(node, id, data, flags);
	}

	virtual cinema::Bool Message(cinema::GeListNode* node, cinema::Int32 type, void* data) override
	{
		cinema::BaseObject* const op = static_cast<cinema::BaseObject*>(node);
		if (type == MSG_DESCRIPTION_COMMAND && data && op)
		{
			const cinema::DescriptionCommand* const command = static_cast<const cinema::DescriptionCommand*>(data);
			const maxon::Int32 id = command->_descId[0].id;
			if (id == CYCLESPROXY_MAKE_RELATIVE || id == CYCLESPROXY_MAKE_ABSOLUTE)
			{
				ChangeFilePath(op, id == CYCLESPROXY_MAKE_RELATIVE);
				return true;
			}
			if (id == CYCLESPROXY_READ_FILE)
			{
				// Asks a Blender in the background (about 2 s on the main thread: a button press).
				maxon::String text;
				iferr (const maxon::String read = ReadProxyFile(op, op->GetDocument()))
					text = FormatString("Cycles Render: the file of \"@\" was not read: @", op->GetName(), err.GetMessage());
				else
					text = FormatString("Cycles Render: \"@\": @", op->GetName(), read);
				ApplicationOutput("@", text);
				cinema::StatusSetText(text);
				cinema::EventAdd();
				return true;
			}
		}
		// The file is an asset of the scene: "Save Project with Assets" and Team Render take it along.
		if (type == MSG_GETALLASSETS && data && op)
		{
			const cinema::Filename file = op->GetDataInstanceRef().GetFilename(CYCLESPROXY_FILE);
			if (file.IsPopulated())
				static_cast<cinema::AssetData*>(data)->Add(file, op, CYCLESPROXY_FILE);
		}
		// After "Save Project with Assets" the file lies with the project: only its name is kept, which is
		// then found next to the document or in its "tex" folder.
		if (type == MSG_MULTI_CLEARSUGGESTEDFOLDER && op)
		{
			const cinema::Filename file = op->GetDataInstanceRef().GetFilename(CYCLESPROXY_FILE);
			if (file.IsPopulated())
				op->GetDataInstanceRef().SetFilename(CYCLESPROXY_FILE, file.GetFile());
		}
		return SUPER::Message(node, type, data);
	}

	// The box: what framing, selection and the view's clipping go by.
	virtual void GetDimension(const cinema::BaseObject* op, cinema::Vector* mp, cinema::Vector* rad) const override
	{
		GetProxyBox(op, op->GetDocument(), *mp, *rad);
	}

	virtual cinema::DRAWRESULT Draw(cinema::BaseObject* op, cinema::DRAWPASS drawpass, cinema::BaseDraw* bd, cinema::BaseDrawHelp* bh) override
	{
		if (drawpass != cinema::DRAWPASS::OBJECT && drawpass != cinema::DRAWPASS::HIGHLIGHTS)
			return SUPER::Draw(op, drawpass, bd, bh);
		if (drawpass == cinema::DRAWPASS::HIGHLIGHTS)
		{
			cinema::Vector color(0.0);
			if (!bd->GetHighlightPassColor(*bh, true, color))
				return cinema::DRAWRESULT::SKIP;
			bd->SetPen(color, SET_PEN_USE_PROFILE_COLOR);
		}
		else
		{
			bd->SetPen(bd->GetObjectColor(bh, op), SET_PEN_USE_PROFILE_COLOR);
		}
		bd->SetMatrix_Matrix(op, bh->GetMg());

		cinema::Vector c;
		cinema::Vector h;
		GetProxyBox(op, op->GetDocument(), c, h);
		// The twelve edges: from each corner on the -X side along X, and the two squares at -X and +X.
		for (maxon::Int side = 0; side < 2; ++side)
		{
			const maxon::Float64 x = side == 0 ? c.x - h.x : c.x + h.x;
			const cinema::Vector square[4] = { cinema::Vector(x, c.y - h.y, c.z - h.z), cinema::Vector(x, c.y + h.y, c.z - h.z),
				cinema::Vector(x, c.y + h.y, c.z + h.z), cinema::Vector(x, c.y - h.y, c.z + h.z) };
			for (maxon::Int i = 0; i < 4; ++i)
			{
				bd->DrawLine(square[i], square[(i + 1) % 4], 0);
				if (side == 0)
					bd->DrawLine(square[i], cinema::Vector(c.x + h.x, square[i].y, square[i].z), 0);
			}
		}
		// A cross on the floor at the file's origin, a fifth of the box's longest half edge to each side.
		const maxon::Float64 arm = 0.2 * maxon::Max(h.x, maxon::Max(h.y, h.z));
		bd->DrawLine(cinema::Vector(-arm, 0.0, 0.0), cinema::Vector(arm, 0.0, 0.0), 0);
		bd->DrawLine(cinema::Vector(0.0, 0.0, -arm), cinema::Vector(0.0, 0.0, arm), 0);
		return cinema::DRAWRESULT::OK;
	}

private:
	static maxon::String EntryLabel(maxon::Bool collection, const maxon::String& name)
	{
		return (collection ? "Collection: "_s : "Scene: "_s) + name;
	}

	// The menu's position for what the object stores: 0 the file's scene, 1.. the list's entries, and
	// one past them a name that is not in the list.
	static maxon::Int32 TakeIndex(const cinema::BaseObject* op, const ProxyContent& content)
	{
		const cinema::BaseContainer& data = op->GetDataInstanceRef();
		const maxon::Int32 kind = data.GetInt32(CYCLESPROXY_TAKE_KIND, CYCLESPROXY_TAKE_KIND_FILE_SCENE);
		if (kind != CYCLESPROXY_TAKE_KIND_SCENE && kind != CYCLESPROXY_TAKE_KIND_COLLECTION)
			return CYCLESPROXY_TAKE_FILE_SCENE;
		const maxon::String name = data.GetString(CYCLESPROXY_TAKE_NAME);
		for (maxon::Int i = 0; i < content.entries.GetCount(); ++i)
		{
			if (content.entries[i].collection == (kind == CYCLESPROXY_TAKE_KIND_COLLECTION) && content.entries[i].name == name)
				return maxon::Int32(i + 1);
		}
		return maxon::Int32(content.entries.GetCount() + 1);
	}

	// "Make Relative" and "Make Absolute": as the Cycles Environment's HDRI buttons, in one undo step.
	static void ChangeFilePath(cinema::BaseObject* op, maxon::Bool relative)
	{
		cinema::BaseDocument* const doc = op->GetDocument();
		const cinema::Filename file = op->GetDataInstanceRef().GetFilename(CYCLESPROXY_FILE);
		maxon::String problem;
		cinema::Filename changed;
		iferr (changed = ChangeFilenameKind(file, doc, relative, problem))
			problem = err.GetMessage();
		if (problem.IsEmpty())
		{
			if (doc)
			{
				doc->StartUndo();
				doc->AddUndo(cinema::UNDOTYPE::CHANGE_SMALL, op);
			}
			op->SetParameter(CreateDescID(cinema::DescLevel(CYCLESPROXY_FILE)), changed, cinema::DESCFLAGS_SET::USERINTERACTION);
			if (doc)
				doc->EndUndo();
			cinema::EventAdd();
			return;
		}
		const maxon::String text = FormatString("Cycles Render: the file of \"@\" was not made @: @", op->GetName(), relative ? "relative"_s : "absolute"_s, problem);
		ApplicationOutput("@", text);
		cinema::StatusSetText(text);
	}
};

cinema::Bool RegisterProxyObject()
{
	// An icon of its own (tools/make_icons.py, proxy_object).
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("proxy_object.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	// OBJECT_HASDEFORMMODE: the green "enabled" check, as the Cycles Light, Fog and Environment have,
	// without making it a generator.
	const cinema::Bool registered = cinema::RegisterObjectPlugin(ID_PROXY_OBJECT, cinema::GeLoadString(IDS_PROXY_OBJECT_NAME), OBJECT_HASDEFORMMODE, ProxyObject::Alloc,
		"Ocyclesproxy"_s, icon, 0, cinema::OBJECTCATEGORY::SCENE);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
