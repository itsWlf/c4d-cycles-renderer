// SPDX-License-Identifier: Apache-2.0
// The curves of Blender's RGB Curves, Vector Curves and Float Curve nodes. The Cycles nodes hold
// each curve as a Cinema 4D spline (a port of type maxon::SplineMapper, shown with Cinema 4D's
// spline control); this unit turns a spline into the points of Blender's curve.
//
// Blender's points have no tangents of their own (each is AUTO, AUTO_CLAMPED or VECTOR), so a
// spline's knots cannot be handed over as they are. The spline is evaluated by Cinema 4D itself
// (SplineMapper::GetPoint) at many positions instead, and Blender gets these points joined by
// straight lines (VECTOR); points that a straight line between their neighbours already gives
// are left out. Cinema 4D's control shows only the square 0..1; it stands for the area of
// Blender's curve (-1..1 for Vector Curves). docs/reference/c4d-sdk.md, "Node space", has the
// measured difference.

#ifndef CYCLESRENDER_CYCLES_CURVES_H__
#define CYCLESRENDER_CYCLES_CURVES_H__

#include "c4d.h"
#include "maxon/graph.h"
#include "maxon/splinemapper.h"

#include "../nodespace/cycles_nodes.h"

namespace cyclesrender
{

/// One spline as Blender's curve gets it, in Blender's coordinates.
struct CurvePoints
{
	/// The points from left to right, x rising; at least two. Blender joins them with straight lines.
	maxon::BaseArray<maxon::Vector2d> points;
	/// The area of the curve: the spline's range, widened to every point. Blender's clipping
	/// rectangle, which also decides the span of the table Blender samples the curve into.
	maxon::Vector2d min;
	maxon::Vector2d max;
};

/// Evaluates @p spline the way Cinema 4D draws it, over its range (and over every knot that lies
/// outside it), keeps the points Blender needs to follow it within a small tolerance, and carries
/// them from the control's square 0..1 into Blender's area @p areaMin to @p areaMax
/// (GetCyclesCurveArea: x' = areaMin.x + x * (areaMax.x - areaMin.x), y alike).
/// What is not as the user will expect is added to @p problems as a sentence: no knots (the
/// spline is 0 everywhere), a range that is empty or no number (0..1 is used instead), a value
/// Cinema 4D does not give as a number (0 is used).
maxon::Result<void> SampleCurve(const maxon::SplineMapper& spline, const maxon::Vector2d& areaMin, const maxon::Vector2d& areaMax,
	CurvePoints& result, maxon::BaseArray<maxon::String>& problems);

/// The `mapping` property of a curve node (protocol.md, "Node graphs") from the node's settings:
/// every "curve:<index>" setting of @p definition (kind 'k') and "curve:extend". @p inputs is
/// the node's input list. Problems are added to @p problems, one sentence each, naming the curve.
maxon::Result<maxon::String> CurveMappingJson(const maxon::GraphNode& inputs, const CyclesNode& definition, maxon::BaseArray<maxon::String>& problems);

} // namespace cyclesrender

#endif // CYCLESRENDER_CYCLES_CURVES_H__
