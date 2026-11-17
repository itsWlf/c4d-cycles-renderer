// SPDX-License-Identifier: Apache-2.0
// Navigating the scene from the IPR's picture (ipr_navigation.h).
//
// Reading order: the scheme (NavigationForDrag) and its constants, the preferences, then one drag:
// Begin (which camera, which point), Step (the moves), End and Cancel. The drag loop itself is the
// picture's (ipr_image_area.cpp, NavigationDrag); the window sends the camera and shows the frames
// during it (ipr_dialog.cpp).

#include "c4d.h"
#include "lib_prefs.h"
#include "ocamera.h"
#include "prefsnavigation.h"
#include "tcyclescamera.h"
#include "tprotection.h"

#include "../scene/parameters.h"
#include "../scene/scene_exporter.h"
#include "../sdk_compat.h"
#include "../tags/camera_tag.h"
#include "ipr_controller.h"
#include "ipr_navigation.h"
#include "ipr_trace.h"

namespace cyclesrender
{

// --- the scheme --------------------------------------------------------------------------------
// Everything a maintainer may want to change about how navigating feels is here.

// Orbit: radians per screen pixel at Cinema 4D's orbit speed 100 % (an estimate, 0.4 degrees: to be
// compared with a view by hand; Cinema 4D's own rate could not be measured, posted input does not
// drive a view).
static const maxon::Float64 ORBIT_RADIANS_PER_PIXEL = 0.4 * maxon::PI / 180.0;
// Dolly: the distance to the point the dolly heads for shrinks by e^(-rate) per screen pixel (100 px:
// to 61 %); a parallel view's width the same way. An estimate as above.
static const maxon::Float64 DOLLY_PER_PIXEL = 0.005;
// Which way the mouse dollies in: right and up bring the camera nearer (+1 each); left and down
// take it away.
static const maxon::Float64 DOLLY_RIGHT = 1.0;
static const maxon::Float64 DOLLY_UP = 1.0;
// A dolly of more than this many pixels in one drag goes no further (e^-10: a 22000th of the distance).
static const maxon::Float64 DOLLY_MAX_PIXELS = 2000.0;
// How far the point under the mouse is looked for, in document units.
static const maxon::Float64 PICK_LENGTH = 1.0e7;
// Panning a perspective view moves the point at this depth with the mouse; never nearer than this
// (document units), so that a point picked right at the lens does not stop the pan.
static const maxon::Float64 MIN_PAN_DEPTH = 1.0;

IPRNAV NavigationForDrag(cinema::Int32 channel, cinema::Int32 qualifier, cinema::HOTKEYFLAGS rotate, cinema::HOTKEYFLAGS move,
	cinema::HOTKEYFLAGS scale)
{
	// Cinema 4D's navigation keys, as the user's key map has them (by default 3, 1, 2 held while dragging).
	if ((rotate & cinema::HOTKEYFLAGS::KEYBOARD) != cinema::HOTKEYFLAGS::NONE)
		return IPRNAV::ORBIT;
	if ((move & cinema::HOTKEYFLAGS::KEYBOARD) != cinema::HOTKEYFLAGS::NONE)
		return IPRNAV::PAN;
	if ((scale & cinema::HOTKEYFLAGS::KEYBOARD) != cinema::HOTKEYFLAGS::NONE)
		return IPRNAV::DOLLY;
	// Alt and a mouse button, as in Cinema 4D's views.
	if ((qualifier & cinema::QALT) != 0)
	{
		if (channel == cinema::BFM_INPUT_MOUSELEFT)
			return IPRNAV::ORBIT;
		if (channel == cinema::BFM_INPUT_MOUSEMIDDLE)
			return IPRNAV::PAN;
		if (channel == cinema::BFM_INPUT_MOUSERIGHT)
			return IPRNAV::DOLLY;
	}
	// Everything else is the picture's own: a drag pans it, the wheel zooms it.
	return IPRNAV::NONE;
}

maxon::String NavigationVerb(IPRNAV mode, maxon::Bool parallel)
{
	switch (mode)
	{
		case IPRNAV::ORBIT: return "Orbiting"_s;
		case IPRNAV::PAN: return "Panning"_s;
		case IPRNAV::DOLLY: return parallel ? "Zooming"_s : "Dollying"_s;
		default: return maxon::String();
	}
}

// --- the preferences ---------------------------------------------------------------------------

NavigationPrefs NavigationPrefs::Read()
{
	NavigationPrefs prefs;
	const cinema::BasePlugin* const hook = cinema::FindPlugin(PREFS_NAVIGATION, cinema::PLUGINTYPE::PREFS);
	if (!hook)
		return prefs;
	prefs.cameraMode = GetIntParameter(hook, PREF_NAVIGATION_CAMERA, prefs.cameraMode);
	prefs.pointOfInterest = GetIntParameter(hook, PREF_NAVIGATION_POI, prefs.pointOfInterest);
	prefs.cursorPlacement = GetIntParameter(hook, PREF_NAVIGATION_CURSOR, prefs.cursorPlacement);
	prefs.dollyToCursor = GetIntParameter(hook, PREF_NAVIGATION_DOLLY, prefs.dollyToCursor);
	prefs.lockPoiForDolly = GetBoolParameter(hook, PREF_NAVIGATION_FIXEDDOLLY, prefs.lockPoiForDolly);
	prefs.reverseOrbit = GetBoolParameter(hook, PREF_NAVIGATION_CAMERAREVERSE, prefs.reverseOrbit);
	prefs.trackball = GetBoolParameter(hook, PREF_NAVIGATION_TRACKBALL, prefs.trackball);
	// The Ctrl modifier is named from the 2026 SDK on, 2024 has it as well (sdk_compat.h).
	prefs.ctrlModifier = GetIntParameter(hook, NAVIGATION_CTRL_MODIFIER, prefs.ctrlModifier);
	prefs.panSpeed = GetFloatParameter(hook, PREF_NAVIGATION_MOVE, prefs.panSpeed);
	prefs.dollySpeed = GetFloatParameter(hook, PREF_NAVIGATION_SCALE, prefs.dollySpeed);
	prefs.orbitSpeed = GetFloatParameter(hook, PREF_NAVIGATION_ROTATE, prefs.orbitSpeed);
	return prefs;
}

// --- one drag -----------------------------------------------------------------------------------

// The point of interest a drag left a camera at, so that the next drag of the same camera goes on
// from it while nothing else has moved the camera. One per Cinema 4D, main thread only.
struct RememberedPoint
{
	const cinema::BaseDocument* doc = nullptr;	///< compared, never dereferenced
	const cinema::BaseObject* camera = nullptr;
	maxon::UInt64 guid = 0;
	cinema::Matrix mg;
	cinema::Vector point;
};
static RememberedPoint g_remembered;

// v turned by `angle` (radians) around the unit vector `axis` (Rodrigues' formula, in plain numbers:
// around +Y a positive angle takes +Z towards +X). Step's comments say which way that is on screen.
static cinema::Vector Turn(const cinema::Vector& v, const cinema::Vector& axis, maxon::Float64 angle)
{
	const maxon::Float64 c = maxon::Cos(angle);
	const maxon::Float64 s = maxon::Sin(angle);
	return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1.0 - c));
}

static cinema::Matrix TurnMatrix(const cinema::Matrix& m, const cinema::Vector& pivot, const cinema::Vector& axis, maxon::Float64 angle)
{
	return cinema::Matrix(pivot + Turn(m.off - pivot, axis, angle), Turn(m.sqmat.v1, axis, angle), Turn(m.sqmat.v2, axis, angle),
		Turn(m.sqmat.v3, axis, angle));
}

// A Protection tag that locks or limits the camera's position or rotation.
static maxon::Bool IsProtected(const cinema::BaseObject* camera)
{
	for (const cinema::BaseTag* tag = camera->GetFirstTag(); tag; tag = tag->GetNext())
	{
		if (tag->GetType() != Tprotection)
			continue;
		if (GetIntParameter(tag, PROTECTION_P, PROTECTION_NONE) != PROTECTION_NONE || GetIntParameter(tag, PROTECTION_R, PROTECTION_NONE) != PROTECTION_NONE)
			return true;
	}
	return false;
}

// The axis of the selection: the mean position of the selected objects, the camera itself left out;
// false when nothing else is selected.
static maxon::Bool SelectionAxis(cinema::BaseDocument* doc, const cinema::BaseObject* camera, cinema::Vector& axis)
{
	cinema::AutoAlloc<cinema::AtomArray> selection;
	if (!selection)
		return false;
	doc->GetActiveObjects(*selection, cinema::GETACTIVEOBJECTFLAGS::NONE);
	cinema::Vector sum;
	maxon::Int count = 0;
	for (maxon::Int32 i = 0; i < selection->GetCount(); ++i)
	{
		const cinema::BaseObject* const op = static_cast<const cinema::BaseObject*>(selection->GetIndex(i));
		if (!op || op == camera)
			continue;
		sum += op->GetMg().off;
		++count;
	}
	if (count > 0)
		axis = sum / maxon::Float64(count);
	return count > 0;
}

// Whether an object, or one of its parents, is selected.
static maxon::Bool IsSelectedOrBelow(const cinema::BaseObject* op, maxon::Bool children)
{
	for (; op; op = children ? op->GetUp() : nullptr)
	{
		if (op->GetBit(BIT_ACTIVE))
			return true;
	}
	return false;
}

// The view that shows the camera's picture: a view looking through it, the render view first.
static cinema::BaseDraw* FindViewOf(cinema::BaseDocument* doc, const cinema::BaseObject* camera)
{
	cinema::BaseDraw* const render = doc->GetRenderBaseDraw();
	if (render && render->GetSceneCamera(doc) == camera)
		return render;
	for (cinema::Int32 i = 0; i < doc->GetBaseDrawCount(); ++i)
	{
		cinema::BaseDraw* const view = doc->GetBaseDraw(i);
		if (view && view->GetSceneCamera(doc) == camera)
			return view;
	}
	return nullptr;
}

void IprNavigation::Reset()
{
	_doc = nullptr;
	_camera = nullptr;
	_editorView = nullptr;
	_view = nullptr;
	_zoomTag = nullptr;
	_mode = IPRNAV::NONE;
	_dx = 0.0;
	_dy = 0.0;
	_undoStarted = false;
	_documentUndo = false;
}

maxon::String IprNavigation::Begin(cinema::BaseDocument* doc, cinema::BaseObject* camera, cinema::BaseDraw* editorView, IPRNAV mode,
	maxon::Float64 imageX, maxon::Float64 imageY, const CameraFraming& framing, maxon::Float64 zoom, maxon::Bool ctrl)
{
	End();
	_picked = false;
	_pickMs = 0.0;
	if (mode == IPRNAV::NONE)
		return "Nothing to do."_s;
	if (!doc || !camera)
		return "The IPR renders through no camera."_s;
	if (framing.frameWidth <= 0.0 || zoom <= 0.0)
		return "The IPR has no picture yet."_s;
	const maxon::String name = camera->GetName();
	if (!editorView)
	{
		if (camera->GetDocument() != doc)
			return FormatString("The IPR renders through \"@\", which is no object of the document: it is not moved.", name);
		if (IsProtected(camera))
			return FormatString("Camera \"@\" has a Protection tag: the IPR does not move it.", name);
		const cinema::LayerData* const layer = camera->GetLayerData(doc);
		if (layer && layer->locked)
			return FormatString("Camera \"@\" is on a locked layer: the IPR does not move it.", name);
	}

	_values = CameraValues::Read(camera);
	const cinema::BaseTag* const tag = FindCameraTag(camera);
	const maxon::Int32 lens = tag ? tag->GetDataInstanceRef().GetInt32(CYCLESCAMERA_LENS_TYPE, CYCLESCAMERA_LENS_TYPE_CAMERA) : CYCLESCAMERA_LENS_TYPE_CAMERA;
	const maxon::Int32 projection = GetIntParameter(camera, CAMERA_PROJECTION, Pperspective);
	// Cinema 4D's fixed views look along their own axes whatever the camera's rotation (unless the
	// tag's lens replaces the projection).
	const maxon::Bool fixed = lens == CYCLESCAMERA_LENS_TYPE_CAMERA && projection != Pperspective && projection != Pparallel;
	if (mode == IPRNAV::ORBIT && fixed)
		return FormatString("Camera \"@\" shows a fixed projection (Top, Front, Isometric, ...): it can be panned and zoomed, not orbited.", name);

	_doc = doc;
	_camera = camera;
	_editorView = editorView;
	_view = editorView ? editorView : FindViewOf(doc, camera);
	_mode = mode;
	_prefs = NavigationPrefs::Read();
	_start = camera->GetMg();
	_current = _start;
	_right = _values.mg.sqmat.v1.GetNormalized();
	_up = _values.mg.sqmat.v2.GetNormalized();
	_forward = _values.mg.sqmat.v3.GetNormalized();
	_orthographic = _values.kind == CAMERAKIND::ORTHOGRAPHIC;
	_trackball = _prefs.trackball || (ctrl && _prefs.ctrlModifier == NAVIGATION_CTRL_TRACKBALL);
	if (_orthographic && lens == CYCLESCAMERA_LENS_TYPE_ORTHOGRAPHIC)
	{
		_zoomTag = const_cast<cinema::BaseTag*>(tag);
		_zoomStart = tag->GetDataInstanceRef().GetFloat(CYCLESCAMERA_LENS_ORTHO_SCALE, CAMERATAG_DEFAULT::ORTHO_SCALE);
	}
	else if (_orthographic)
	{
		_zoomStart = GetFloatParameter(camera, CAMERA_ZOOM, 1.0);
	}

	// The point of interest: where the last drag of this camera left it, if nothing has moved the
	// camera since; else on the view axis at the camera's target distance (the view's own point of
	// interest is not readable through the SDK).
	const cinema::Vector position = _values.mg.off;
	const maxon::Float64 targetDistance = GetFloatParameter(camera, CAMERAOBJECT_TARGETDISTANCE, 2000.0);
	_poiStart = position + _forward * (targetDistance > 0.0 ? targetDistance : 2000.0);
	if (g_remembered.doc == doc && g_remembered.camera == camera && g_remembered.guid == camera->GetGUID() && g_remembered.mg == _start)
		_poiStart = g_remembered.point;
	_pointOfInterest = _poiStart;

	// The point under the mouse, among the meshes that render (the navigation preference "Cursor
	// Placement": all, the selected objects, or those and their children; off: none).
	cinema::Vector origin;
	cinema::Vector direction;
	const maxon::Bool ray = CameraRay(_values, framing, imageX, imageY, origin, direction);
	cinema::Vector hit;
	if (ray && _prefs.cursorPlacement != PREF_NAVIGATION_CURSOR_OFF)
	{
		const maxon::TimeValue started = maxon::TimeValue::GetTime();
		ExportSettings settings;
		settings.camera = camera;
		SceneExporter exporter(doc, settings);
		const maxon::Int32 placement = _prefs.cursorPlacement;
		maxon::Delegate<maxon::Bool(const cinema::BaseObject*)> accept;
		if (placement == PREF_NAVIGATION_CURSOR_SELECTED || placement == PREF_NAVIGATION_CURSOR_CHILDREN)
		{
			accept = [placement](const cinema::BaseObject* owner) -> maxon::Bool
			{
				return IsSelectedOrBelow(owner, placement == PREF_NAVIGATION_CURSOR_CHILDREN);
			};
		}
		maxon::Float64 distance = 0.0;
		ifnoerr (exporter.Collect())
		{
			if (exporter.FindNearestHit(origin, direction, PICK_LENGTH * 2.0, accept, distance))
			{
				_picked = true;
				hit = origin + direction * distance;
			}
		}
		_pickMs = (maxon::TimeValue::GetTime() - started).GetMilliseconds();
		TraceIpr("nav_pick_ms", _pickMs);
	}

	// The point an orbit turns around: the point under the mouse if one was found, else the
	// preference "Point of Interest" (screen centre, selection axis, camera axis). Cinema 4D's
	// "Camera Mode" is a preset of these two (measured: Centre sets screen centre and no cursor
	// placement, Object the selection axis, Camera the camera axis, Cursor placement on all; changing
	// either afterwards makes it Custom), so it is not read on its own.
	cinema::Vector selected;
	if (_picked)
		_pivot = hit;
	else if (_prefs.pointOfInterest == PREF_NAVIGATION_POI_OBJ && SelectionAxis(doc, camera, selected))
		_pivot = selected;
	else if (_prefs.pointOfInterest == PREF_NAVIGATION_POI_CAM)
		_pivot = position;
	else
		_pivot = _poiStart;
	if (mode != IPRNAV::ORBIT)
		_pivot = _picked ? hit : _poiStart;

	// A pan moves the point under the mouse with it (at its depth), as far as the mouse goes.
	const maxon::Float64 depth = maxon::Max(Dot((_picked ? hit : _poiStart) - position, _forward), MIN_PAN_DEPTH);
	_unitsPerPixel = UnitsPerPixel(_values, framing, depth) / zoom;

	// A dolly heads for the point under the mouse ("Dolly to Cursor": all views), else for the point
	// of interest; a parallel view zooms, around the mouse unless "Dolly to Cursor" is off.
	const maxon::Bool toCursor = ray && (_prefs.dollyToCursor == PREF_NAVIGATION_DOLLY_ALL || (_orthographic && _prefs.dollyToCursor == PREF_NAVIGATION_DOLLY_ORTHO));
	_zoomCentre = cinema::Vector();
	if (_orthographic)
	{
		if (toCursor)
		{
			cinema::Vector centreOrigin;
			cinema::Vector centreDirection;
			// The camera's axis meets the picture where across and up are 0 (CameraRay's terms).
			const maxon::Float64 axisX = framing.width * 0.5 - _values.filmOffsetX * framing.frameWidth;
			const maxon::Float64 axisY = framing.height * 0.5 - _values.filmOffsetY * framing.frameHeight;
			if (CameraRay(_values, framing, axisX, axisY, centreOrigin, centreDirection))
				_zoomCentre = origin - centreOrigin;
		}
	}
	else if (toCursor)
	{
		const maxon::Float64 along = Dot(direction, _forward);
		_dollyTarget = _picked ? hit : origin + direction * (Dot(_poiStart - position, _forward) / maxon::Max(along, 1e-6));
	}
	else
	{
		_dollyTarget = position + _forward * maxon::Max(Dot(_poiStart - position, _forward), MIN_PAN_DEPTH);
	}
	TraceIpr("nav_begin", maxon::Float64(maxon::Int(mode)));
	return maxon::String();
}

void IprNavigation::StartUndo()
{
	_undoStarted = true;
	if (_editorView)
	{
		// An editor camera belongs to its view, whose undo is a list of its own ("Undo View").
		_editorView->InitUndo(_doc);
		return;
	}
	_doc->StartUndo();
	_doc->AddUndo(cinema::UNDOTYPE::CHANGE_SMALL, _camera);
	if (_zoomTag && _mode == IPRNAV::DOLLY)
		_doc->AddUndo(cinema::UNDOTYPE::CHANGE_SMALL, _zoomTag);
	_documentUndo = true;
}

void IprNavigation::SetZoom(maxon::Float64 factor)
{
	// The view's width becomes factor times what it was.
	if (_zoomTag)
		_zoomTag->SetParameter(ConstDescIDLevel(CYCLESCAMERA_LENS_ORTHO_SCALE), cinema::GeData(_zoomStart * factor), cinema::DESCFLAGS_SET::NONE);
	else
		_camera->SetParameter(ConstDescIDLevel(CAMERA_ZOOM), cinema::GeData(_zoomStart / factor), cinema::DESCFLAGS_SET::NONE);
}

void IprNavigation::Step(maxon::Float64 dx, maxon::Float64 dy)
{
	if (!_camera || (dx == 0.0 && dy == 0.0))
		return;
	if (!_undoStarted)
		StartUndo();
	const maxon::Float64 lastDx = _dx;
	const maxon::Float64 lastDy = _dy;
	_dx += dx;
	_dy += dy;

	cinema::Matrix mg = _start;
	switch (_mode)
	{
		case IPRNAV::ORBIT:
		{
			// The scene turns with the mouse, as if grabbed: right turns the camera to the left around the
			// point (yaw positive here, in Cinema 4D's left-handed axes), down lifts it over the point.
			// "Reverse Orbit" turns both round.
			const maxon::Float64 rate = ORBIT_RADIANS_PER_PIXEL * _prefs.orbitSpeed * (_prefs.reverseOrbit ? -1.0 : 1.0);
			if (_trackball)
			{
				// Around the camera's own axes as they are now, step by step.
				const cinema::Vector up = _current.sqmat.v2.GetNormalized();
				const cinema::Vector right = _current.sqmat.v1.GetNormalized();
				mg = TurnMatrix(TurnMatrix(_current, _pivot, right, rate * (_dy - lastDy)), _pivot, up, rate * (_dx - lastDx));
				_pointOfInterest = _pivot + Turn(Turn(_pointOfInterest - _pivot, right, rate * (_dy - lastDy)), up, rate * (_dx - lastDx));
			}
			else
			{
				// Turntable: tilted around the picture's horizontal axis, then turned around the world's up
				// axis (Y); the sums of all steps give exactly what the steps one by one would.
				const cinema::Vector worldUp(0.0, 1.0, 0.0);
				mg = TurnMatrix(TurnMatrix(_start, _pivot, _right, rate * _dy), _pivot, worldUp, rate * _dx);
				_pointOfInterest = _pivot + Turn(Turn(_poiStart - _pivot, _right, rate * _dy), worldUp, rate * _dx);
			}
			break;
		}
		case IPRNAV::PAN:
		{
			// The scene follows the mouse: the camera goes the other way.
			const maxon::Float64 units = _unitsPerPixel * _prefs.panSpeed;
			const cinema::Vector shift = _right * (-_dx * units) + _up * (_dy * units);
			mg.off = _start.off + shift;
			_pointOfInterest = _poiStart + shift;
			break;
		}
		case IPRNAV::DOLLY:
		{
			const maxon::Float64 pixels = maxon::ClampValue((DOLLY_RIGHT * _dx - DOLLY_UP * _dy) * _prefs.dollySpeed, -DOLLY_MAX_PIXELS, DOLLY_MAX_PIXELS);
			const maxon::Float64 factor = maxon::Exp(-DOLLY_PER_PIXEL * pixels);
			cinema::Vector shift;
			if (_orthographic)
			{
				SetZoom(factor);
				// The point under the mouse stays there: it was _zoomCentre from the axis, it will be that
				// times factor.
				shift = _zoomCentre * (1.0 - factor);
			}
			else
			{
				shift = (_dollyTarget - _start.off) * (1.0 - factor);
			}
			mg.off = _start.off + shift;
			_pointOfInterest = _prefs.lockPoiForDolly && !_orthographic ? _poiStart : _poiStart + shift;
			break;
		}
		default:
			return;
	}
	_current = mg;
	_camera->SetMg(mg);
	TraceIpr("nav_moved");
}

void IprNavigation::End()
{
	if (!_camera)
		return;
	if (_documentUndo)
		_doc->EndUndo();
	if (_undoStarted)
	{
		g_remembered.doc = _doc;
		g_remembered.camera = _camera;
		g_remembered.guid = _camera->GetGUID();
		g_remembered.mg = _camera->GetMg();
		g_remembered.point = _pointOfInterest;
		cinema::EventAdd();
	}
	TraceIpr("nav_end");
	Reset();
}

void IprNavigation::Cancel()
{
	if (!_camera)
		return;
	if (_undoStarted)
	{
		// Back where the drag began, then the step is undone while it is still open, as the SDK's own
		// tools cancel on Escape (DoUndo(true) closes it). Measured (c4dpy): the step before it stays the
		// next one to undo; and since the camera is back already, a redo brings back nothing.
		_camera->SetMg(_start);
		if (_orthographic)
			SetZoom(1.0);
		if (_documentUndo)
			_doc->DoUndo(true);
		else if (_editorView)
			_editorView->DoUndo(_doc);
		cinema::EventAdd();
	}
	TraceIpr("nav_cancel");
	Reset();
}

// --- for the window and the test hook -------------------------------------------------------------

maxon::String BeginIprNavigation(IprNavigation& navigation, IPRNAV mode, maxon::Float64 imageX, maxon::Float64 imageY, maxon::Int32 shownWidth,
	maxon::Int32 shownHeight, maxon::Float64 zoom, maxon::Bool ctrl)
{
	IprController& ipr = IprController::Get();
	if (ipr.GetState() != IPRSTATE::RUNNING)
		return "Start the IPR to navigate from its picture."_s;
	if (ipr.IsCameraLocked())
		return "The IPR's camera is locked: unlock it (the lock next to the camera list) to navigate."_s;
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	cinema::BaseDraw* editorView = nullptr;
	cinema::BaseObject* const camera = ipr.GetRenderCamera(doc, editorView);
	// The frame on screen may still be of the size before the window was resized.
	const CameraFraming& framing = ipr.GetFraming();
	maxon::Float64 x = imageX;
	maxon::Float64 y = imageY;
	maxon::Float64 scale = 1.0;
	if (shownWidth > 0 && shownHeight > 0 && (shownWidth != framing.width || shownHeight != framing.height))
	{
		scale = maxon::Float64(framing.width) / maxon::Float64(shownWidth);
		x *= scale;
		y *= maxon::Float64(framing.height) / maxon::Float64(shownHeight);
	}
	return navigation.Begin(doc, camera, editorView, mode, x, y, framing, zoom / scale, ctrl);
}

} // namespace cyclesrender
