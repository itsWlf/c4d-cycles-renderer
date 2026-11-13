// SPDX-License-Identifier: Apache-2.0
// The IPR window's "Add" menu (ipr_add.h).

#include "c4d.h"
#include "c4d_basematerial.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "ocamera.h"

#include "../ids.h"
#include "../lights/light_tag.h"
#include "../nodespace/cycles_nodes.h"
#include "ipr_add.h"

namespace cyclesrender
{

static maxon::Bool IsCamera(const cinema::BaseObject* op)
{
	// Orscamera is the camera Cinema 4D creates by default; it is no instance of Ocamera.
	return op && (op->IsInstanceOf(Ocamera) || op->GetType() == Orscamera);
}

static maxon::Bool HasTag(const cinema::BaseObject* op, cinema::Int32 type)
{
	for (const cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
	{
		if (tag->GetType() == type)
			return true;
	}
	return false;
}

// A material of Cycles nodes, as the command "Cycles Material" makes it (materials/cycles_material.cpp).
static maxon::Int AddCyclesMaterial(cinema::BaseDocument* doc, maxon::String& message)
{
	cinema::BaseMaterial* material = cinema::BaseMaterial::Alloc(Mmaterial);
	if (!material)
		return 0;
	material->SetName(cinema::GeLoadString(IDS_CYCLES_MATERIAL_NAME));
	iferr (static_cast<cinema::NodeMaterial*>(material)->CreateDefaultGraph(maxon::Id(CYCLES_NODESPACE_ID)))
	{
		message = FormatString("The new material could not be given its Cycles node graph: @", err.GetMessage());
		cinema::BaseMaterial::Free(material);
		return 0;
	}
	doc->StartUndo();
	doc->InsertMaterial(material);
	doc->AddUndo(cinema::UNDOTYPE::NEWOBJ, material);
	// Selecting it inside the undo step: Cinema 4D records a selection as a step of its own
	// otherwise, and the first undo would only take the selection back.
	doc->SetActiveMaterial(material);
	doc->EndUndo();
	message = "Added a Cycles Material."_s;
	return 1;
}

// The camera the render view looks through: its scene camera, or its editor camera.
static const cinema::BaseObject* RenderViewCamera(cinema::BaseDocument* doc, cinema::BaseDraw*& view)
{
	view = doc->GetRenderBaseDraw() ? doc->GetRenderBaseDraw() : doc->GetActiveBaseDraw();
	const cinema::BaseObject* looking = view ? view->GetSceneCamera(doc) : nullptr;
	if (!looking && view)
		looking = view->GetEditorCamera();
	return looking;
}

// A Cycles Light (lights/light_object.h, Blender's defaults: a point light of 10 W) where the
// render view looks from, turned as that camera, so that a spot or an area light made of it later
// shines where the picture looks. Selected, in one undo step.
static maxon::Int AddCyclesLight(cinema::BaseDocument* doc, maxon::String& message)
{
	cinema::BaseObject* const light = cinema::BaseObject::Alloc(ID_LIGHT_OBJECT);
	if (!light)
	{
		message = "The Cycles Light could not be made."_s;
		return 0;
	}
	cinema::BaseDraw* view = nullptr;
	if (const cinema::BaseObject* const looking = RenderViewCamera(doc, view))
		light->SetMg(looking->GetMg());
	light->SetName(cinema::GeLoadString(IDS_LIGHT_OBJECT_NAME));
	doc->StartUndo();
	doc->InsertObject(light, nullptr, nullptr);
	doc->AddUndo(cinema::UNDOTYPE::NEWOBJ, light);
	doc->SetActiveObject(light, SELECTION_NEW);
	doc->EndUndo();
	message = "Added a Cycles Light where the render view looks from."_s;
	return 1;
}

// A camera where the active view looks from, with the view's focal length, and a Cycles Camera tag.
// It becomes the camera of the render view: the IPR (on "Render View") then shows it, and the
// tag's settings, at once, from where the view was.
static maxon::Int AddCameraWithTag(cinema::BaseDocument* doc, maxon::String& message)
{
	cinema::BaseObject* const camera = cinema::BaseObject::Alloc(Ocamera);
	if (!camera)
		return 0;
	cinema::BaseDraw* view = nullptr;
	const cinema::BaseObject* const looking = RenderViewCamera(doc, view);
	if (looking)
	{
		camera->SetMg(looking->GetMg());
		cinema::GeData focus;
		if (looking->GetParameter(CreateDescID(cinema::DescLevel(CAMERA_FOCUS)), focus, cinema::DESCFLAGS_GET::NONE) && focus.GetType() == cinema::DA_REAL)
			camera->SetParameter(CreateDescID(cinema::DescLevel(CAMERA_FOCUS)), focus, cinema::DESCFLAGS_SET::NONE);
	}
	camera->SetName("Cycles Camera"_s);
	cinema::BaseTag* const tag = camera->MakeTag(ID_CAMERA_TAG);
	doc->StartUndo();
	doc->InsertObject(camera, nullptr, nullptr);
	doc->AddUndo(cinema::UNDOTYPE::NEWOBJ, camera);
	if (view)
	{
		doc->AddUndo(cinema::UNDOTYPE::CHANGE_SMALL, view);
		view->SetSceneCamera(camera);
	}
	doc->SetActiveObject(camera, SELECTION_NEW);
	if (tag)
		doc->SetActiveTag(tag, SELECTION_NEW);
	doc->EndUndo();
	message = tag ? "Added a camera with a Cycles Camera tag; the render view looks through it."_s
		: "Added a camera; its Cycles Camera tag could not be made."_s;
	return 1;
}

// Which selected objects a tag goes on.
enum class TAGGED
{
	ANY,
	CAMERAS,
	LIGHTS
} MAXON_ENUM_LIST(TAGGED);

// A tag of @p type on every selected object that @p accepts and has none.
static maxon::Int AddTagToSelection(cinema::BaseDocument* doc, cinema::Int32 type, TAGGED accepts, maxon::String& message)
{
	cinema::AutoAlloc<cinema::AtomArray> selection;
	if (!selection)
		return 0;
	doc->GetActiveObjects(selection, cinema::GETACTIVEOBJECTFLAGS::CHILDREN);
	const maxon::String tagName = accepts == TAGGED::CAMERAS ? "Cycles Camera tag"_s : accepts == TAGGED::LIGHTS ? "Cycles Light tag"_s : "Cycles Object tag"_s;
	const maxon::String kind = accepts == TAGGED::CAMERAS ? "camera"_s : accepts == TAGGED::LIGHTS ? "light"_s : "object"_s;
	maxon::Int added = 0;
	maxon::Int skipped = 0;
	cinema::BaseTag* last = nullptr;
	doc->StartUndo();
	for (cinema::Int32 i = 0; i < selection->GetCount(); ++i)
	{
		cinema::BaseObject* const op = static_cast<cinema::BaseObject*>(selection->GetIndex(i));
		if (!op || (accepts == TAGGED::CAMERAS && !IsCamera(op)) || (accepts == TAGGED::LIGHTS && !IsTaggableLight(op)))
			continue;
		if (HasTag(op, type))
		{
			++skipped;
			continue;
		}
		cinema::BaseTag* const tag = op->MakeTag(type);
		if (!tag)
			continue;
		doc->AddUndo(cinema::UNDOTYPE::NEWOBJ, tag);
		last = tag;
		++added;
	}
	if (last)
		doc->SetActiveTag(last, SELECTION_NEW);
	doc->EndUndo();
	if (added > 0)
		message = FormatString("Added @ @(s).", added, tagName);
	else if (skipped > 0)
		message = FormatString("Every selected @ has a @ already.", kind, tagName);
	else
		message = FormatString("Select @ first.", accepts == TAGGED::CAMERAS ? "a camera"_s : accepts == TAGGED::LIGHTS ? "a light of Cinema 4D or Redshift"_s : "an object"_s);
	return added;
}

maxon::Int AddToDocument(cinema::BaseDocument* doc, IPRADD what, maxon::String& message)
{
	message = maxon::String();
	if (!doc)
		return 0;
	maxon::Int added = 0;
	switch (what)
	{
		case IPRADD::CYCLES_MATERIAL: added = AddCyclesMaterial(doc, message); break;
		case IPRADD::CAMERA_WITH_TAG: added = AddCameraWithTag(doc, message); break;
		case IPRADD::CAMERA_TAG: added = AddTagToSelection(doc, ID_CAMERA_TAG, TAGGED::CAMERAS, message); break;
		case IPRADD::OBJECT_TAG: added = AddTagToSelection(doc, ID_OBJECT_TAG, TAGGED::ANY, message); break;
		case IPRADD::CYCLES_LIGHT: added = AddCyclesLight(doc, message); break;
		case IPRADD::LIGHT_TAG: added = AddTagToSelection(doc, ID_LIGHT_TAG, TAGGED::LIGHTS, message); break;
	}
	cinema::EventAdd();
	return added;
}

} // namespace cyclesrender
