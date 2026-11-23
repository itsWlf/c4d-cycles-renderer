// SPDX-License-Identifier: Apache-2.0
// Where the Cycles Scatter object puts its copies: over a surface (on its points, by area, or as a Poisson
// disc), or from a CSV file; thinned out by a vertex map, a shader, the surface's slope and a field list;
// turned to the surface; varied by shaders; then changed by MoGraph's effectors.

#ifndef CYCLESRENDER_SCATTER_DISTRIBUTION_H__
#define CYCLESRENDER_SCATTER_DISTRIBUTION_H__

#include "c4d.h"

namespace cyclesrender
{

/// The colour a copy has before any effector colours it: MoGraph's colour of a Cloner's clone that no
/// effector touched (measured, docs/reference/c4d-sdk.md, "The Cycles Scatter object").
static const cinema::Vector SCATTER_COPY_COLOR = cinema::Vector(1.0);

/// The copies of one scatter, in the scatter object's axes.
struct ScatterCopies
{
	maxon::BaseArray<cinema::Matrix> matrices;    ///< where each copy is, turned and scaled
	maxon::BaseArray<cinema::Vector> colors;      ///< its MoGraph colour, as MoGraph keeps it (a display colour, not clamped)
	maxon::BaseArray<maxon::Int32> sources;       ///< which source it shows: an index into the scatter's sources (scatter_object.h)
};

/// What one distribution did, for the console, the test hook and measurements.
struct ScatterStats
{
	maxon::Int candidates = 0;           ///< places tried (Surface: Count; Vertices: the points; Poisson: until full; File: the lines read)
	maxon::Int placed = 0;               ///< copies after the restrictions, before the effectors
	maxon::Int hidden = 0;               ///< copies an effector made invisible
	maxon::Int triangles = 0;            ///< of the surface
	maxon::Int effectorsRun = 0;         ///< effectors executed
	maxon::Bool fieldsSampled = false;   ///< the object's own field list was sampled
	maxon::Float64 distributeMs = 0.0;   ///< placing (with the shaders and the field list)
	maxon::Float64 effectorMs = 0.0;     ///< the effectors
};

//----------------------------------------------------------------------------------------
/// Computes the copies of the Cycles Scatter object @p scatter from its parameters
/// (res/description/ocyclesscatter.h) and the objects they link: the surface as Cinema 4D's caches have
/// it now, the vertex map, the shaders, the field list and the effectors. The same parameters and scene
/// give the same copies, bit for bit, in every session and Cinema 4D version: the random numbers are the
/// plugin's own (a hash of the seed, the candidate's number and what it is drawn for), and each candidate
/// draws its own, so a restriction that removes one copy does not move the others.
/// Call it from the object's GetVirtualObjects(), on the thread that evaluates the document.
/// @param[in] scatter      The scatter object; its global matrix places it.
/// @param[in] doc          Its document.
/// @param[in] sourceCount  How many sources the copies are spread over (its children that are scattered); 0: none.
/// @param[in] thread       The evaluating thread, for the effectors; may be null.
/// @param[out] copies      The copies, emptied first.
/// @param[out] stats       What was done.
/// @param[out] warnings    What the user should be told (a missing surface, a file not read, ...), each a sentence
///                         without the object's name.
//----------------------------------------------------------------------------------------
maxon::Result<void> ComputeScatter(cinema::BaseObject* scatter, const cinema::BaseDocument* doc, maxon::Int sourceCount, cinema::BaseThread* thread,
	ScatterCopies& copies, ScatterStats& stats, maxon::BaseArray<maxon::String>& warnings);

//----------------------------------------------------------------------------------------
/// A number of the objects and settings the copies of @p scatter follow from, outside the object's own
/// parameters and children: its global matrix, the surface (its matrix, its caches and the objects in
/// them), the vertex map, the shaders, the effectors (and their field lists), the object's field list,
/// and the document's time while anything that may move with it is used. Another number means the copies
/// must be computed again (the object's GetVirtualObjects compares it with the one they were computed from).
//----------------------------------------------------------------------------------------
maxon::UInt64 ScatterDependencies(const cinema::BaseObject* scatter, const cinema::BaseDocument* doc);

} // namespace cyclesrender

#endif // CYCLESRENDER_SCATTER_DISTRIBUTION_H__
