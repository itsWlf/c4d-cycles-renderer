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

// --- the render view's camera through undo and redo -------------------------------------------
//
// Cinema 4D's undo cannot hold a view (measured in c4dpy and in the application, 2026-11-12): after
// undoing a step that recorded the render view with AddUndo (CHANGE, CHANGE_NOCHILDREN or
// CHANGE_SMALL, the camera set with SetSceneCamera or as BASEDRAW_DATA_CAMERA), the document's view
// has no camera link at all and looks through its own editor camera (an "RS Camera" that is no object
// of the document), redo does not bring the link back, and the view object held before the undo is
// no longer the document's. Left out of the undo step, the view keeps its link: while the camera it
// names is gone (undone) the view falls back to its editor camera, and once the camera is back
// (redone) the link finds it again. So the view is kept out of the step, and the plugin itself puts
// the view back on the camera it had before while the added camera is gone, and on the added camera
// again when it returns: FollowAddedCameras(), on every change of a document.

namespace
{
/// One "Camera with Cycles Camera Tag": which view was switched, from which camera, to which.
struct CameraSwitch
{
	cinema::BaseDocument* doc = nullptr;		///< compared with the document list; used only while it is in it
	cinema::BaseDraw* view = nullptr;				///< compared with the document's views; used only while it is one
	cinema::BaseLink* previous = nullptr;		///< the camera the view looked through; empty: its editor camera
	cinema::BaseLink* added = nullptr;			///< the camera that was added
	maxon::Bool reverted = false;						///< the view was put back on the previous camera
};
}

// Main thread only. The newest last; a few are enough (each lasts while its undo step can come back).
static maxon::BaseArray<CameraSwitch> g_switches;
static const maxon::Int MAX_SWITCHES = 16;

static void FreeSwitch(CameraSwitch& entry)
{
	cinema::BaseLink::Free(entry.previous);
	cinema::BaseLink::Free(entry.added);
}

static void RememberSwitch(cinema::BaseDocument* doc, cinema::BaseDraw* view, cinema::BaseObject* previous, cinema::BaseObject* added)
{
	CameraSwitch entry;
	entry.doc = doc;
	entry.view = view;
	entry.previous = previous ? cinema::BaseLink::Alloc() : nullptr;
	entry.added = cinema::BaseLink::Alloc();
	if (entry.previous)
		entry.previous->SetLink(previous);
	if (!entry.added || (previous && !entry.previous))
	{
		FreeSwitch(entry);
		return;
	}
	entry.added->SetLink(added);
	if (g_switches.GetCount() >= MAX_SWITCHES)
	{
		FreeSwitch(g_switches[0]);
		g_switches.Erase(0) iferr_ignore("Erasing the first of a non-empty array cannot fail.");
	}
	iferr (g_switches.Append(entry))
		FreeSwitch(entry);
}

static maxon::Bool IsOpenDocument(const cinema::BaseDocument* doc)
{
	for (const cinema::BaseDocument* open = cinema::GetFirstDocument(); open; open = open->GetNext())
	{
		if (open == doc)
			return true;
	}
	return false;
}

static maxon::Bool IsViewOf(cinema::BaseDocument* doc, const cinema::BaseDraw* view)
{
	for (cinema::Int32 i = 0; i < doc->GetBaseDrawCount(); ++i)
	{
		if (doc->GetBaseDraw(i) == view)
			return true;
	}
	return false;
}

maxon::Int FollowAddedCameras()
{
	maxon::Int changed = 0;
	for (maxon::Int i = g_switches.GetCount() - 1; i >= 0; --i)
	{
		CameraSwitch& entry = g_switches[i];
		maxon::Bool keep = IsOpenDocument(entry.doc) && IsViewOf(entry.doc, entry.view);
		if (keep)
		{
			cinema::BaseDocument* const doc = entry.doc;
			cinema::BaseObject* const added = static_cast<cinema::BaseObject*>(entry.added->GetLink(doc, Obase));
			cinema::BaseObject* const previous = entry.previous ? static_cast<cinema::BaseObject*>(entry.previous->GetLink(doc, Obase)) : nullptr;
			// What the view's link names now; nothing while it names no camera of the document (its
			// editor camera, or a camera that is gone).
			cinema::BaseObject* const current = entry.view->HasCameraLink() ? entry.view->GetSceneCamera(doc) : nullptr;
			if (!entry.reverted)
			{
				if (added)
				{
					// Still added: as long as the view looks through it there is nothing to do; when the
					// user chose another camera the switch is theirs now.
					keep = current == added;
				}
				else if (!current)
				{
					// Undone (or deleted): the view goes back to where it looked before.
					entry.view->SetSceneCamera(previous);
					entry.reverted = true;
					++changed;
				}
				else
				{
					keep = false;
				}
			}
			else if (added)
			{
				// Redone: as the step did, the view looks through the added camera, unless the user has
				// chosen another one since.
				if (current == previous)
				{
					entry.view->SetSceneCamera(added);
					entry.reverted = false;
					++changed;
				}
				else
				{
					keep = false;
				}
			}
		}
		if (!keep)
		{
			FreeSwitch(entry);
			g_switches.Erase(i) iferr_ignore("Erasing an element of the array cannot fail.");
		}
	}
	if (changed > 0)
		cinema::EventAdd();
	return changed;
}

maxon::Int GetAddedCameraSwitchCount()
{
	return g_switches.GetCount();
}

void FreeAddedCameraSwitches()
{
	for (CameraSwitch& entry : g_switches)
		FreeSwitch(entry);
	g_switches.Reset();
}

// A camera where the active view looks from, with the view's focal length, and a Cycles Camera tag.
// It becomes the camera of the render view: the IPR (on "Render View") then shows it, and the
// tag's settings, at once, from where the view was. The view is not part of the undo step (see
// above): FollowAddedCameras() follows the step's undo and redo.
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
	// The camera the view's link names before, if any (without one it looks through its editor camera).
	cinema::BaseObject* const previous = view && view->HasCameraLink() ? view->GetSceneCamera(doc) : nullptr;
	doc->StartUndo();
	doc->InsertObject(camera, nullptr, nullptr);
	doc->AddUndo(cinema::UNDOTYPE::NEWOBJ, camera);
	if (view)
	{
		view->SetSceneCamera(camera);
		RememberSwitch(doc, view, previous, camera);
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
