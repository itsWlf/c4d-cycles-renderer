// SPDX-License-Identifier: Apache-2.0
// The "Cycles Scatter" object: scatters the objects below it over a surface, on its points, from a CSV
// file, with MoGraph's effectors; drawn cheaply in the viewport and rendered by Blender as instances.

#ifndef CYCLESRENDER_SCATTER_OBJECT_H__
#define CYCLESRENDER_SCATTER_OBJECT_H__

#include "c4d.h"

#include "scatter_distribution.h"

namespace cyclesrender
{

/// One object the scatter scatters: one of its children (not a light or a camera), as the polygon objects
/// it showed when the copies were computed. A Cycles Proxy among the children (objects/proxy_object.h), which
/// has no polygons, is scattered as itself: its clone is a copy of the proxy object.
struct ScatterSource
{
	const cinema::BaseObject* clone = nullptr;   ///< the child's hierarchy as polygon objects (HIERARCHYCLONEFLAGS::ASPOLY), or a copy of a Cycles Proxy; owned by the scatter object
	maxon::UInt64 guid = 0;                       ///< the child's GUID: what the exporter keys its items by, the same from one export to the next
	maxon::String name;                           ///< the child's name
	cinema::Vector boxCenter = cinema::Vector(0.0);   ///< the bounds of its polygons in the child's own axes (the viewport's boxes)
	cinema::Vector boxRadius = cinema::Vector(0.0);
};

/// What the scatter object computed at its last evaluation.
struct ScatterResult
{
	ScatterCopies copies;                             ///< the copies; copies.sources index into sources
	maxon::BaseArray<ScatterSource> sources;
	maxon::BaseArray<maxon::String> warnings;         ///< for the console, sentences after the object's name
	maxon::BaseArray<maxon::String> skipped;          ///< the names of the children that are not scattered (lights, cameras)
	ScatterStats stats;
	maxon::Int evaluations = 0;                       ///< GetVirtualObjects calls since the object was made (for tests)
	maxon::Int builds = 0;                            ///< times the copies were computed since then
	maxon::UInt64 dependencies = 0;                   ///< ScatterDependencies() when they were computed
};

/// Registers the object with Cinema 4D. Called once from PluginStart(). The object is a generator whose
/// children are its input (OBJECT_GENERATOR | OBJECT_INPUT, as a Cloner): they disappear from the
/// viewport and from Cinema 4D's renderers, and it has the green "enabled" check. At every evaluation it
/// computes its copies when anything they follow from changed (its parameters and children, the surface,
/// the vertex map, the shaders, the effectors and fields, its own place; scatter_distribution.h,
/// ScatterDependencies): the children are cloned as polygon objects once, the copies are matrices in its
/// axes with a MoGraph colour each. Its cache is an empty Null: the copies are no geometry in Cinema 4D;
/// the viewport draws them as points or boxes (Display tab), and the exporter sends one `instancer` per
/// polygon object of each child (scene/scene_exporter.h).
cinema::Bool RegisterScatterObject();

/// What the scatter object @p scatter computed at its last evaluation (ScatterResult), or null when @p scatter
/// is none or was never evaluated. Valid until the document is evaluated again: read it on the thread that
/// evaluated the document, or on the main thread when no evaluation runs (the exporter does).
const ScatterResult* GetScatterResult(const cinema::BaseObject* scatter);

//----------------------------------------------------------------------------------------
/// The copies of @p scatter as the scene is now: those of its last evaluation, or, when something they follow
/// from changed after it, computed again into @p fresh. Cinema 4D evaluates the objects in the order of the
/// Object Manager, so a surface below the scatter gets its polygons after the scatter has looked at it: its
/// copies then follow one evaluation late (none at the first). The exporter takes the copies from here, so
/// that what renders is the scene as it is. Reads @p scatter only.
/// @param[out] warnings  What the user should be told when they were computed again (as ScatterResult::warnings).
/// @return               The copies (the object's or @p fresh); null when @p scatter was never evaluated.
//----------------------------------------------------------------------------------------
maxon::Result<const ScatterCopies*> GetCurrentScatterCopies(cinema::BaseObject* scatter, const cinema::BaseDocument* doc, ScatterCopies& fresh,
	maxon::BaseArray<maxon::String>& warnings);

/// Whether the green check of the scatter @p scatter is on: off, its children are ordinary objects again.
maxon::Bool IsScatterSwitchedOn(const cinema::BaseObject* scatter);

/// Whether @p op is an object the scatter does not scatter, but names in the console: a light or a camera.
maxon::Bool IsNotScattered(const cinema::BaseObject* op);

} // namespace cyclesrender

#endif // CYCLESRENDER_SCATTER_OBJECT_H__
