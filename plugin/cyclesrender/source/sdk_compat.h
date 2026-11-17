// SPDX-License-Identifier: Apache-2.0
// What differs between the Cinema 4D SDKs this module is built with, in one place.
//
// The module is built once per Cinema 4D version (2024, 2025, 2026, 2027) from the same sources,
// each against that version's SDK (tools/build_plugin.ps1 -Version). Wherever the SDKs differ in
// a name or a signature the code uses, the difference is answered here, and the code calls the
// helper instead of testing a version itself. Each entry says which SDKs differ and how; the full
// list with how it was found is in docs/reference/c4d-sdk.md, "Cinema 4D versions".
//
// One difference is not in this file because no header can answer it: in the 2024 SDK the
// classic API has no namespace (Bool, BaseObject, GeLoadString, ... are global), from 2025 on it
// lives in namespace cinema. The sources write cinema::X throughout; the 2024 build defines the
// macro cinema as nothing (tools/build_plugin.ps1), so that cinema::X reads ::X. The one place
// where that does not work is a declaration whose return type is written cinema::Bool right before
// a qualified name (cinema::Bool cinema::F() would become ::Bool::F()); main.cpp writes those with
// a trailing return type. The 2024 build is also C++17 (the 2024 SDK's own setting), the others
// C++20: comparisons rely on operator== only (C++17 does not derive != from it).
//
// API_VERSION (cinema.framework, operatingsystem.h) is the SDK's own number: 2024500 (2024.5),
// 2025302 (2025.3.2), 2026300 (2026.3), 2027000 (2027.0).

#ifndef CYCLESRENDER_SDK_COMPAT_H__
#define CYCLESRENDER_SDK_COMPAT_H__

#include "c4d.h"
#include "maxon/graph.h"
#include "maxon/optional.h"
#include "maxon/system_process.h"

/// The Cinema 4D version this build is for, as a year (2024 to 2027), from the SDK it is built with.
#define CYCLESRENDER_C4D_YEAR (API_VERSION / 1000)

/// ConstDescIDLevel(id), the description id of one level known at compile time, is a macro of
/// the SDKs from 2025 on; the 2024 SDK writes the same as ConstDescID(DescLevel(id)).
#if API_VERSION < 2025000 && !defined(ConstDescIDLevel)
#define ConstDescIDLevel(...) ConstDescID(DescLevel(__VA_ARGS__))
#endif

namespace cyclesrender
{

/// The value of an optional as a Result. Opt::GetValue() is deprecated in the 2027 SDK in
/// favour of ToResult(), which the SDKs before 2027 do not have.
template <typename OPT> MAXON_ATTRIBUTE_FORCE_INLINE auto OptToResult(OPT&& optional)
{
#if API_VERSION >= 2027000
	return optional.ToResult();
#else
	return optional.GetValue();
#endif
}

/// The value of a Result, or the fallback when it holds an error. Result::GetValueOr exists from
/// the 2026 SDK on; this works with every SDK.
template <typename T> T ResultOr(const maxon::Result<T>& result, const T& fallback)
{
	return result == maxon::OK ? result.GetValue() : fallback;
}

/// The renderer's answer to GetRenderInfo. VIDEOPOSTINFO::EVALUATES_SCENE ("the video post
/// evaluates the scene itself", which skips Cinema 4D's preparation of objects for its own
/// renderer) exists from Cinema 4D 2026.2 on; before it there is nothing to say, and Cinema 4D
/// prepares the scene as for its own renderers.
#if API_VERSION >= 2026200
constexpr cinema::VIDEOPOSTINFO RENDERER_INFO = cinema::VIDEOPOSTINFO::EVALUATES_SCENE;
#else
constexpr cinema::VIDEOPOSTINFO RENDERER_INFO = cinema::VIDEOPOSTINFO::NONE;
#endif

/// The grey that Cinema 4D's own renderers give an object without a material in a document with
/// OCIO colour management, in render space. Measured with the Standard renderer (a plane under an
/// infinite light, 2026-11-16): 2026.3 and 2027.0 take the 80 % grey as sRGB, 0.604 in linear
/// light; 2024.5 takes it as it stands, 0.8 (its picture is 1.3 times as bright; the highlight is
/// the same). With "Basic" colour management all of them give 0.604. 2025 [untested: its
/// installation here has no Python] is taken to be like 2026.
#if API_VERSION >= 2025000
constexpr maxon::Float DEFAULT_GREY_OCIO = 0.604;
#else
constexpr maxon::Float DEFAULT_GREY_OCIO = 0.8;
#endif

/// Whether the Redshift that comes with this Cinema 4D takes a colour port's value as straight
/// (colour and alpha apart) or as already multiplied by its alpha. Measured with Color Composite and
/// Color Layer on colours with alpha below 1 (tests/c4d/redshift_nodes.py, 2026-11-16): the
/// Redshift of 2027.0 multiplies (0.6 at alpha 0.8 composites as 0.48), those of 2026.3 and 2024.5
/// do not (as 0.6). 2025 [untested: no Python here] is taken to be like 2026. A Redshift updated
/// apart from Cinema 4D may differ [untested].
#if API_VERSION >= 2027000
constexpr maxon::Bool REDSHIFT_STRAIGHT_COLORS = true;
#else
constexpr maxon::Bool REDSHIFT_STRAIGHT_COLORS = false;
#endif

/// PolygonObject::GetNgonCount() is const from the 2026 SDK on, not before.
inline cinema::Int32 NgonCount(const cinema::PolygonObject* op)
{
	return const_cast<cinema::PolygonObject*>(op)->GetNgonCount();
}

/// The value of a Result<Opt<T>>, or T's default when the optional is empty: GetOrDefault() from
/// the 2025 SDK on, GetValueOrDefault() in 2024.
template <typename RESULT> MAXON_ATTRIBUTE_FORCE_INLINE auto OptOrDefault(RESULT&& result)
{
#if API_VERSION >= 2025000
	return result.GetOrDefault();
#else
	return result.GetValueOrDefault();
#endif
}

/// An OCIO converter for the document's settings. From the 2025 SDK on an existing converter
/// (previous, may be null) is initialised again; the 2024 SDK can only make a new one, so
/// previous is deleted there. Either way the caller keeps only the returned converter.
inline maxon::Result<cinema::OcioConverter*> InitOcioConverter(const cinema::BaseDocument* doc, cinema::OcioConverter* previous)
{
#if API_VERSION >= 2025000
	return cinema::OcioConverter::Init(doc, previous);
#else
	if (previous)
		DeleteObj(previous);
	return cinema::OcioConverter::Init(doc);
#endif
}

/// Shows a picture in the Picture Viewer under a name. The 2024 SDK has no name argument: there the
/// entry gets Cinema 4D's default name.
inline cinema::Bool ShowInPictureViewer(cinema::BaseBitmap* bitmap, const maxon::String& name)
{
#if API_VERSION >= 2025000
	return cinema::ShowBitmap(bitmap, name);
#else
	return cinema::ShowBitmap(bitmap);
#endif
}

/// What a node command's ARGUMENTS::NODECORE::NODEPATH and NODEPATHLIST hold for one node: a
/// NodePath from the 2025 SDK on, an IoNodePath (the path and a port id) in 2024. NodePathOf gives
/// the node's path either way.
#if API_VERSION >= 2025000
using CommandNodePath = maxon::NodePath;
inline const maxon::NodePath& NodePathOf(const maxon::NodePath& path)
{
	return path;
}
inline CommandNodePath ToCommandNodePath(const maxon::NodePath& path)
{
	return path;
}
#else
using CommandNodePath = maxon::IoNodePath;
inline const maxon::NodePath& NodePathOf(const maxon::IoNodePath& path)
{
	return path.first;
}
inline CommandNodePath ToCommandNodePath(const maxon::NodePath& path)
{
	return CommandNodePath(path, maxon::InternedId());
}
#endif

/// The navigation preference "View Rotation Ctrl/Cmd Modifier" and its value "Trackball Rotation
/// (temporary)": PREF_NAVIGATION_CTRLCMD_MODIFIER and PREF_NAVIGATION_CTRLCMD_TRACKBALL of
/// description/prefsnavigation.h from the 2026 SDK on; the 2024 and 2025 SDKs do not name them, but
/// Cinema 4D 2024.5 has the preference (measured: it reads 1065919, and Ctrl's trackball works as in
/// 2027; tests/c4d/ipr_navigate.py). The IPR's navigation reads it by its number in every version
/// (ipr/ipr_navigation.cpp); 2025 [untested: no Python there] is taken to have it too.
constexpr maxon::Int32 NAVIGATION_CTRL_MODIFIER = 1065917;
constexpr maxon::Int32 NAVIGATION_CTRL_TRACKBALL = 1065918;

/// The flags for starting Blender in a way that ends it when Cinema 4D ends, also when Cinema 4D
/// is killed or crashes: EXECUTEPROGRAMFLAGS::KILL_IF_STARTING_PROCESS_ENDS exists from the 2025 SDK
/// on. With 2024 a Blender the plugin started still ends with its connection to the plugin (the
/// bridge quits when the socket closes), but not when Cinema 4D is killed while Blender hangs.
#if API_VERSION >= 2025000
constexpr maxon::EXECUTEPROGRAMFLAGS KILL_WITH_CINEMA = maxon::EXECUTEPROGRAMFLAGS::KILL_IF_STARTING_PROCESS_ENDS;
#else
constexpr maxon::EXECUTEPROGRAMFLAGS KILL_WITH_CINEMA = maxon::EXECUTEPROGRAMFLAGS::NONE;
#endif

} // namespace cyclesrender

#endif // CYCLESRENDER_SDK_COMPAT_H__
