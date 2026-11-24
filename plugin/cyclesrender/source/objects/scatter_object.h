// SPDX-License-Identifier: Apache-2.0
// The "Cycles Scatter" object: scatters the objects below it over a surface, on its points, inside a closed
// mesh, from a CSV file, with MoGraph's effectors; drawn cheaply in the viewport and rendered by Blender as
// instances.

#ifndef CYCLESRENDER_SCATTER_OBJECT_H__
#define CYCLESRENDER_SCATTER_OBJECT_H__

#include "c4d.h"

#include "scatter_distribution.h"

namespace cyclesrender
{

/// The clones of a scatter's children (a Null holding one clone per child, HIERARCHYCLONEFLAGS::ASPOLY). Made
/// again only when a child changed; every result computed from them holds a reference, so that they live as
/// long as anyone reads such a result (the exporter on the main thread while the next one is computed).
struct ScatterCloneRoot
{
	cinema::BaseObject* root = nullptr;   ///< owned
	ScatterCloneRoot() = default;
	~ScatterCloneRoot() { cinema::BaseObject::Free(root); }
	MAXON_DISALLOW_COPY_AND_ASSIGN(ScatterCloneRoot);
};

/// One object the scatter scatters: one of its children (not a light or a camera), as the polygon objects
/// it showed when the copies were computed. A Cycles Proxy among the children (objects/proxy_object.h), which
/// has no polygons, is scattered as itself: its clone is a copy of the proxy object.
struct ScatterSource
{
	const cinema::BaseObject* clone = nullptr;   ///< the child's hierarchy as polygon objects, or a copy of a Cycles Proxy; owned by the result (ScatterResult::cloneRoot, proxyCopies)
	maxon::UInt64 guid = 0;                       ///< the child's GUID: what the exporter keys its items by, the same from one export to the next
	maxon::String name;                           ///< the child's name
	cinema::Vector boxCenter = cinema::Vector(0.0);   ///< the bounds of its polygons in the child's own axes (the viewport's boxes)
	cinema::Vector boxRadius = cinema::Vector(0.0);
};

/// What the scatter object computed at one evaluation, with what the viewport draws of it. Never changed after
/// it was published: the object publishes a new one each time it computes again, and a reader keeps the one it
/// took (ScatterResultRef) for as long as it reads, on any thread.
struct ScatterResult
{
	ScatterCopies copies;                             ///< the copies; copies.sources index into sources
	maxon::BaseArray<ScatterSource> sources;
	maxon::BaseArray<maxon::String> warnings;         ///< for the console, sentences after the object's name
	maxon::BaseArray<maxon::String> skipped;          ///< the names of the children that are not scattered (lights, cameras)
	ScatterStats stats;
	maxon::Int builds = 0;                            ///< times the copies were computed since the object was made (for tests)
	maxon::UInt64 dependencies = 0;                   ///< ScatterDependencies() when they were computed
	// The viewport: the copies drawn (the display rate's share), their places and colours (3 floats each), the bounds.
	maxon::BaseArray<maxon::Int> drawCopies;
	maxon::BaseArray<cinema::Vector32> drawPoints;
	maxon::BaseArray<maxon::Float32> drawColors;
	cinema::Vector boundsLow = cinema::Vector(0.0);
	cinema::Vector boundsHigh = cinema::Vector(0.0);
	// What the sources' clones live in.
	maxon::StrongRef<ScatterCloneRoot> cloneRoot;
	maxon::BaseArray<cinema::BaseObject*> proxyCopies;   ///< the copies that stand for Cycles Proxy children (owned)

	ScatterResult() = default;
	~ScatterResult()
	{
		for (cinema::BaseObject*& copy : proxyCopies)
			cinema::BaseObject::Free(copy);
	}
	MAXON_DISALLOW_COPY_AND_ASSIGN(ScatterResult);
};

/// A result as a reader holds it: it stays whole and alive while held, whatever the object computes meanwhile.
using ScatterResultRef = maxon::StrongRef<const ScatterResult>;

/// Registers the object with Cinema 4D. Called once from PluginStart(). The object is a generator whose
/// children are its input (OBJECT_GENERATOR | OBJECT_INPUT, as a Cloner): they disappear from the
/// viewport and from Cinema 4D's renderers, and it has the green "enabled" check. At every evaluation it
/// computes its copies when anything they follow from changed (its parameters and children, the surface,
/// the vertex map, the shaders, the effectors and fields, its own place; scatter_distribution.h,
/// ScatterDependencies): the children are cloned as polygon objects once, the copies are matrices in its
/// axes with a MoGraph colour each. Its cache is an empty Null: the copies are no geometry in Cinema 4D;
/// the viewport draws them as points or boxes (Display tab), and the exporter sends one `instancer` per
/// polygon object of each child (scene/scene_exporter.h).
///
/// Threads: Cinema 4D evaluates the document on its drawing thread while the main thread may read it (the
/// IPR's export, the test hook). So the object never changes a result it has published: it computes the next
/// one aside and publishes it whole (GetScatterResult). Its copies are computed by one thread at a time (the
/// shaders are prepared for rendering, the field list sampled, the effectors run in the computation); the
/// evaluating thread gives up waiting, and computing, when Cinema 4D asks it to stop (BaseThread::TestBreak), and
/// the exporter does not wait at all (GetCurrentScatterCopies).
cinema::Bool RegisterScatterObject();

/// What the scatter object @p scatter computed at its last evaluation, or an empty reference when @p scatter is
/// none or was never evaluated. The result stays as it is while the reference is held, on any thread.
ScatterResultRef GetScatterResult(const cinema::BaseObject* scatter);

/// How often @p scatter's GetVirtualObjects ran since it was made (for tests); 0 for any other object.
maxon::Int GetScatterEvaluations(const cinema::BaseObject* scatter);

//----------------------------------------------------------------------------------------
/// The copies of @p scatter as the scene is now: those of @p result (its last evaluation), or, when something
/// they follow from changed after it, computed again into @p fresh. Cinema 4D evaluates the objects in the order
/// of the Object Manager, so a surface below the scatter gets its polygons after the scatter has looked at it:
/// its copies then follow one evaluation late (none at the first). The exporter takes the copies from here, so
/// that what renders is the scene as it is. Never waits: while the object computes its copies on another thread,
/// @p result's copies are returned (that computation ends with a new cache, which the IPR follows).
/// @param[in] result     What GetScatterResult() gave for @p scatter.
/// @param[out] warnings  What the user should be told when they were computed again (as ScatterResult::warnings).
/// @return               The copies (@p result's or @p fresh).
//----------------------------------------------------------------------------------------
maxon::Result<const ScatterCopies*> GetCurrentScatterCopies(cinema::BaseObject* scatter, const cinema::BaseDocument* doc, const ScatterResult& result,
	ScatterCopies& fresh, maxon::BaseArray<maxon::String>& warnings);

/// Whether the green check of the scatter @p scatter is on: off, its children are ordinary objects again.
maxon::Bool IsScatterSwitchedOn(const cinema::BaseObject* scatter);

/// Whether @p op is an object the scatter does not scatter, but names in the console: a light or a camera.
maxon::Bool IsNotScattered(const cinema::BaseObject* op);

} // namespace cyclesrender

#endif // CYCLESRENDER_SCATTER_OBJECT_H__
