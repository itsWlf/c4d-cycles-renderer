// SPDX-License-Identifier: Apache-2.0
// Navigating the scene from the IPR's picture (0.2.0, F17): a drag with Cinema 4D's navigation keys
// orbits, pans or dollies the camera the IPR renders through, as in Cinema 4D's own views.

#ifndef CYCLESRENDER_IPR_NAVIGATION_H__
#define CYCLESRENDER_IPR_NAVIGATION_H__

#include "c4d.h"

#include "../scene/camera_export.h"

namespace cyclesrender
{

/// What a drag in the IPR's picture does to the camera.
enum class IPRNAV
{
	NONE,		///< nothing: the drag is the picture's own (it pans the picture, as before 0.2.0)
	ORBIT,	///< turns the camera around a point (Cinema 4D's "Rotate Camera")
	PAN,		///< moves it in the plane of the picture ("Move Camera")
	DOLLY		///< moves it nearer or further; a parallel view zooms instead ("Scale Camera")
} MAXON_ENUM_LIST(IPRNAV);

//----------------------------------------------------------------------------------------
/// The key and button scheme, in one place (ipr_navigation.cpp): which drag in the IPR's picture
/// navigates the scene, and how. Decided with what the picture already did in mind (docs/architecture.md,
/// "The IPR window"): a plain drag (left or middle button) pans the picture and the wheel zooms it,
/// like the Picture Viewer, and both stay so. Navigating needs what navigates in Cinema 4D's views:
/// - Cinema 4D's navigation keys as the user has them (Customize Commands; by default 1 move, 2 scale,
///   3 rotate) held while dragging with any button: they are asked through IsHotkeyDown, and only
///   their keyboard part counts, so that a mouse binding Cinema 4D keeps for its views cannot take
///   a plain drag away from the picture;
/// - Alt with the left (orbit), middle (pan) or right button (dolly), Cinema 4D's default in its views.
/// @param[in] channel    BFM_INPUT_MOUSELEFT, _MOUSEMIDDLE or _MOUSERIGHT.
/// @param[in] qualifier  BFM_INPUT_QUALIFIER of the mouse-down event.
//----------------------------------------------------------------------------------------
IPRNAV NavigationForDrag(cinema::Int32 channel, cinema::Int32 qualifier, cinema::HOTKEYFLAGS rotate, cinema::HOTKEYFLAGS move,
	cinema::HOTKEYFLAGS scale);

//----------------------------------------------------------------------------------------
/// Cinema 4D's navigation preferences (Preferences > Navigation, description/prefsnavigation.h) as
/// far as the IPR follows them, read from the preference hook (FindPlugin(PREFS_NAVIGATION,
/// PLUGINTYPE::PREFS)) at the start of every drag. Defaults are those of Cinema 4D 2027 (measured).
/// The camera mode is a preset of the point of interest and the cursor placement (measured: setting
/// it sets those two, and setting either of them makes it Custom), so only those two decide.
//----------------------------------------------------------------------------------------
struct NavigationPrefs
{
	maxon::Int32 cameraMode = 3;					///< PREF_NAVIGATION_CAMERA: 1 centre, 2 object, 3 cursor, 4 camera, 5 custom (reported, not used)
	maxon::Int32 pointOfInterest = 1;			///< PREF_NAVIGATION_POI: 1 screen centre, 2 selection axis, 3 camera axis
	maxon::Int32 cursorPlacement = 4;			///< PREF_NAVIGATION_CURSOR: 1 off, 2 selected, 3 selected and children, 4 all
	maxon::Int32 dollyToCursor = 3;				///< PREF_NAVIGATION_DOLLY: 1 off, 2 parallel views, 3 all views
	maxon::Bool lockPoiForDolly = false;	///< PREF_NAVIGATION_FIXEDDOLLY
	maxon::Bool reverseOrbit = false;			///< PREF_NAVIGATION_CAMERAREVERSE
	maxon::Bool trackball = false;				///< PREF_NAVIGATION_TRACKBALL
	maxon::Int32 ctrlModifier = 0;				///< PREF_NAVIGATION_CTRLCMD_MODIFIER (2026 on): 1065918 trackball, 1065919 view snapping
	maxon::Float64 panSpeed = 1.0;				///< PREF_NAVIGATION_MOVE
	maxon::Float64 dollySpeed = 1.0;			///< PREF_NAVIGATION_SCALE
	maxon::Float64 orbitSpeed = 1.0;			///< PREF_NAVIGATION_ROTATE

	static NavigationPrefs Read();
};

//----------------------------------------------------------------------------------------
/// One navigating drag in the IPR's picture. Begin() decides whether and how, Step() moves the
/// camera for every movement of the mouse, End() closes it. The camera moved is the one the IPR
/// renders through (IprController::GetRenderCamera):
/// - a camera object of the document: one undo step per drag (UNDOTYPE::CHANGE_SMALL on the camera,
///   and on its Cycles Camera tag when the tag's orthographic scale is zoomed), started at the first
///   movement, as a drag in Cinema 4D's views does; refused when the camera has a Protection tag that
///   locks or limits its position or rotation, or lies on a locked layer;
/// - a view's editor camera: one step of that view's own undo ("Undo View", BaseDraw::InitUndo);
/// - a fixed projection (Left to Dimetric) is panned and zoomed, never orbited (its axes are fixed).
/// Every move is computed from where the drag started and how far the mouse has gone in all, so
/// nothing drifts. Everything here runs on the main thread.
//----------------------------------------------------------------------------------------
class IprNavigation
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(IprNavigation);

public:
	IprNavigation() = default;
	~IprNavigation() { End(); }

	//----------------------------------------------------------------------------------------
	/// Starts a drag at the point (@p imageX, @p imageY) of the IPR's image (render pixels from its
	/// top left, the image framed by @p framing). Reads the navigation preferences, finds the point
	/// under the mouse (SceneExporter::FindNearestHit) and the point to orbit around.
	/// @param[in] editorView  The view whose editor camera @p camera is; nullptr for a camera object.
	/// @param[in] zoom        Screen pixels per image pixel of the picture (its own zoom).
	/// @param[in] ctrl        Ctrl is held (the Ctrl modifier of the navigation preferences).
	/// @return                Empty when the drag navigates; otherwise why not, for the status bar.
	//----------------------------------------------------------------------------------------
	maxon::String Begin(cinema::BaseDocument* doc, cinema::BaseObject* camera, cinema::BaseDraw* editorView, IPRNAV mode, maxon::Float64 imageX,
		maxon::Float64 imageY, const CameraFraming& framing, maxon::Float64 zoom, maxon::Bool ctrl);

	/// The mouse went (@p dx, @p dy) screen pixels further, right and down positive: moves the camera.
	void Step(maxon::Float64 dx, maxon::Float64 dy);

	/// The drag is over: the undo step is closed, the point of interest remembered for the next drag
	/// of this camera, and Cinema 4D told (EventAdd).
	void End();

	/// The drag was cancelled (Escape): the camera goes back to where it was and its undo step is
	/// undone (BaseDocument::DoUndo(true) while it is open, as Cinema 4D's tools cancel; for an editor
	/// camera BaseDraw::DoUndo).
	void Cancel();

	maxon::Bool IsActive() const { return _camera != nullptr; }
	IPRNAV GetMode() const { return _mode; }
	cinema::BaseObject* GetCamera() const { return _camera; }
	/// The view that shows the camera's picture (the editor camera's view, or a view looking through
	/// the camera object), or nullptr when no view does; for redrawing it during the drag.
	cinema::BaseDraw* GetView() const { return _view; }
	/// The point the drag orbits around (orbit), or whose depth sets the pan's speed (pan), or the
	/// dolly moves towards (dolly).
	const cinema::Vector& GetPivot() const { return _pivot; }
	/// Whether the mouse was over a mesh at the start (the point is a hit, not the point of interest).
	maxon::Bool IsPicked() const { return _picked; }
	maxon::Float64 GetPickMilliseconds() const { return _pickMs; }
	/// The navigation preferences this drag follows (read by Begin).
	const NavigationPrefs& GetPrefs() const { return _prefs; }

private:
	void StartUndo();
	void SetZoom(maxon::Float64 factor);
	void Reset();

	cinema::BaseDocument* _doc = nullptr;
	cinema::BaseObject* _camera = nullptr;
	cinema::BaseDraw* _editorView = nullptr;
	cinema::BaseDraw* _view = nullptr;
	cinema::BaseTag* _zoomTag = nullptr;	///< the Cycles Camera tag whose orthographic scale a dolly zooms
	IPRNAV _mode = IPRNAV::NONE;
	NavigationPrefs _prefs;
	CameraValues _values;									///< the camera as the picture shows it, at the start
	cinema::Matrix _start;								///< the camera object's global matrix at the start
	cinema::Matrix _current;							///< as set by the last step (trackball orbits go on from it)
	cinema::Vector _right;								///< the picture's axes, normalised
	cinema::Vector _up;
	cinema::Vector _forward;
	maxon::Bool _orthographic = false;
	maxon::Bool _trackball = false;
	maxon::Float64 _zoomStart = 1.0;			///< CAMERA_ZOOM, or the tag's orthographic scale
	maxon::Float64 _unitsPerPixel = 0.0;	///< document units per screen pixel at the pan's depth
	cinema::Vector _pivot;
	cinema::Vector _pointOfInterest;			///< at the start, and as the drag moves it (End remembers it)
	cinema::Vector _poiStart;
	cinema::Vector _dollyTarget;					///< perspective: where a dolly heads
	cinema::Vector _zoomCentre;						///< parallel: the offset of the mouse's point from the camera's axis
	maxon::Bool _picked = false;
	maxon::Float64 _pickMs = 0.0;
	maxon::Float64 _dx = 0.0;							///< screen pixels the mouse went in all
	maxon::Float64 _dy = 0.0;
	maxon::Bool _undoStarted = false;
	maxon::Bool _documentUndo = false;
};

//----------------------------------------------------------------------------------------
/// What every navigating drag starts with, for the window and the test hook alike: the IPR must be
/// rendering and its camera not locked; then the camera it renders through is found and
/// IprNavigation::Begin() called. (@p imageX, @p imageY) are pixels of the frame on screen, which is
/// @p shownWidth x @p shownHeight; they are taken over to the size the IPR renders at now.
/// @return  Empty when the drag navigates; otherwise why not.
//----------------------------------------------------------------------------------------
maxon::String BeginIprNavigation(IprNavigation& navigation, IPRNAV mode, maxon::Float64 imageX, maxon::Float64 imageY, maxon::Int32 shownWidth,
	maxon::Int32 shownHeight, maxon::Float64 zoom, maxon::Bool ctrl);

/// The verb for the status bar: "Orbiting", "Panning", "Dollying" (or "Zooming" for a parallel view).
maxon::String NavigationVerb(IPRNAV mode, maxon::Bool parallel);

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_NAVIGATION_H__
