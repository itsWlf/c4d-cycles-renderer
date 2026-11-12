// SPDX-License-Identifier: Apache-2.0
// Turns the Cinema 4D splines of the curve nodes into the points of Blender's curves
// (cycles_curves.h).

#include "c4d.h"

#include "../session/json_writer.h"
#include "cycles_curves.h"

namespace cyclesrender
{

// How many even steps the spline is evaluated in across its span, besides at each knot. Blender
// turns a curve into a table of 256 steps over its area before it renders (CM_TABLE in
// blenkernel/intern/colortools.cc), and Cycles samples that table again in 256 steps, so a finer
// sampling would not show.
static const maxon::Int CURVE_STEPS = 256;
// How far a straight line between two kept points may miss a point that is left out, as a part
// of the height of the curve's range.
static const maxon::Float CURVE_TOLERANCE = 1e-4;
// Blender's clipping rectangle cannot reach further (clip_min_x and the others, rna_color.cc).
static const maxon::Float BLENDER_CLIP_LIMIT = 100.0;

static maxon::Bool IsNumber(maxon::Float value)
{
	return value == value && value > -1e30 && value < 1e30;
}

// Leaves out the points that a straight line between the kept points around them already gives
// within @p tolerance, measured along y (Douglas-Peucker). The first and last point stay.
static maxon::Result<void> Simplify(const maxon::BaseArray<maxon::Vector2d>& points, maxon::Float tolerance, maxon::BaseArray<maxon::Vector2d>& kept)
{
	iferr_scope;
	const maxon::Int count = points.GetCount();
	kept.Reset();
	if (count <= 2)
	{
		kept.CopyFrom(points) iferr_return;
		return maxon::OK;
	}
	maxon::BaseArray<maxon::Bool> keep;
	keep.Resize(count) iferr_return;
	for (maxon::Bool& one : keep)
		one = false;
	keep[0] = true;
	keep[count - 1] = true;
	// Spans still to look at, as pairs of indices of kept points.
	maxon::BaseArray<maxon::Int> spans;
	spans.Append(0) iferr_return;
	spans.Append(count - 1) iferr_return;
	while (!spans.IsEmpty())
	{
		maxon::Int last = 0;
		maxon::Int first = 0;
		spans.Pop(&last);
		spans.Pop(&first);
		const maxon::Vector2d& a = points[first];
		const maxon::Vector2d& b = points[last];
		maxon::Int worst = NOTOK;
		maxon::Float worstDistance = tolerance;
		for (maxon::Int i = first + 1; i < last; ++i)
		{
			const maxon::Float t = (points[i].x - a.x) / (b.x - a.x);
			const maxon::Float distance = maxon::Abs(points[i].y - (a.y + (b.y - a.y) * t));
			if (distance > worstDistance)
			{
				worstDistance = distance;
				worst = i;
			}
		}
		if (worst == NOTOK)
			continue;
		keep[worst] = true;
		spans.Append(first) iferr_return;
		spans.Append(worst) iferr_return;
		spans.Append(worst) iferr_return;
		spans.Append(last) iferr_return;
	}
	for (maxon::Int i = 0; i < count; ++i)
	{
		if (keep[i])
		{
			kept.Append(points[i]) iferr_return;
		}
	}
	return maxon::OK;
}

maxon::Result<void> SampleCurve(const maxon::SplineMapper& spline, const maxon::Vector2d& areaMin, const maxon::Vector2d& areaMax,
	CurvePoints& result, maxon::BaseArray<maxon::String>& problems)
{
	iferr_scope;
	result.points.Reset();
	maxon::BaseArray<maxon::Vector2d> kept;

	// GetPoint wants the knots in order of x (splinemapper.h); the port's value may not have them so.
	maxon::SplineMapper sorted = spline;
	sorted.SortKnots() iferr_return;
	maxon::Vector2d min;
	maxon::Vector2d max;
	sorted.GetRange(min, max);
	if (!IsNumber(min.x) || !IsNumber(min.y) || !IsNumber(max.x) || !IsNumber(max.y) || !(min.x < max.x) || !(min.y < max.y))
	{
		problems.Append(FormatString("its range (@, @ to @, @) is empty; 0, 0 to 1, 1 is used", min.x, min.y, max.x, max.y)) iferr_return;
		min = maxon::Vector2d(0.0, 0.0);
		max = maxon::Vector2d(1.0, 1.0);
	}

	const maxon::BaseArray<maxon::SplineMapperKnot>& knots = sorted.GetKnots();
	if (knots.IsEmpty())
	{
		// Measured: GetPoint then gives 0 everywhere. Blender gets the same, but it is hardly meant.
		problems.Append("it has no knots, so it gives 0 everywhere"_s) iferr_return;
	}

	// The span: the range, and any knot outside it (Cinema 4D's control keeps knots inside the
	// range; a script need not).
	maxon::Float from = min.x;
	maxon::Float to = max.x;
	for (const maxon::SplineMapperKnot& knot : knots)
	{
		if (IsNumber(knot.position.x))
		{
			from = maxon::Min(from, knot.position.x);
			to = maxon::Max(to, knot.position.x);
		}
	}

	// Where the spline is evaluated: in even steps, and at every knot, so that the corner between
	// two straight segments lands exactly. Both lists rise; merged, and positions closer than a
	// ten-millionth of the span taken once (knots at one position become one point: Blender's
	// curve then rises steeply over one step instead of jumping).
	const maxon::Float closest = (to - from) * 1e-7;
	maxon::BaseArray<maxon::Vector2d> samples;
	maxon::Int knot = 0;
	maxon::Bool notANumber = false;
	for (maxon::Int step = 0; step <= CURVE_STEPS || knot < knots.GetCount();)
	{
		maxon::Float x;
		const maxon::Float even = step <= CURVE_STEPS ? (step == CURVE_STEPS ? to : from + (to - from) * maxon::Float(step) / maxon::Float(CURVE_STEPS)) : to;
		if (knot < knots.GetCount() && (step > CURVE_STEPS || knots[knot].position.x < even))
		{
			x = knots[knot].position.x;
			++knot;
			if (!IsNumber(x))
				continue;
		}
		else
		{
			x = even;
			++step;
		}
		if (!samples.IsEmpty() && x - samples[samples.GetCount() - 1].x <= closest)
			continue;
		maxon::Float y = sorted.GetPoint(x).y;
		if (!IsNumber(y))
		{
			notANumber = true;
			y = 0.0;
		}
		samples.Append(maxon::Vector2d(x, y)) iferr_return;
	}
	if (notANumber)
	{
		problems.Append("Cinema 4D gave no number for some of its positions; 0 is used there"_s) iferr_return;
	}

	Simplify(samples, CURVE_TOLERANCE * (max.y - min.y), kept) iferr_return;

	// The area: the range, widened to every point (a Bezier segment may overshoot it).
	for (const maxon::Vector2d& point : kept)
	{
		min = maxon::Vector2d(maxon::Min(min.x, point.x), maxon::Min(min.y, point.y));
		max = maxon::Vector2d(maxon::Max(max.x, point.x), maxon::Max(max.y, point.y));
	}

	// From the control's square into Blender's area (the same for 0..1; twice as large and moved
	// for the -1..1 of Vector Curves).
	const maxon::Vector2d size = areaMax - areaMin;
	const auto toArea = [&areaMin, &size](const maxon::Vector2d& point) { return maxon::Vector2d(areaMin.x + point.x * size.x, areaMin.y + point.y * size.y); };
	result.points.EnsureCapacity(kept.GetCount()) iferr_return;
	for (const maxon::Vector2d& point : kept)
	{
		result.points.Append(toArea(point)) iferr_return;
	}
	result.min = toArea(min);
	result.max = toArea(max);
	return maxon::OK;
}

maxon::Result<maxon::String> CurveMappingJson(const maxon::GraphNode& inputs, const CyclesNode& definition, maxon::BaseArray<maxon::String>& problems)
{
	iferr_scope;

	// By the index of the curve in Blender's mapping.curves; a node has at most four.
	static const maxon::Int MAX_CURVES = 4;
	CurvePoints curves[MAX_CURVES];
	maxon::Bool present[MAX_CURVES] = { false, false, false, false };
	maxon::String extend = "EXTRAPOLATED"_s;

	for (maxon::Int i = 0; i < definition.propertyCount; ++i)
	{
		const CyclesProperty& property = definition.properties[i];
		const maxon::String name(property.identifier);
		if (!name.StartsWith("curve:"_s))
			continue;
		maxon::Id portId;
		portId.Init(maxon::String(property.port)) iferr_return;
		const maxon::GraphNode port = inputs.FindChild(portId) iferr_return;
		if (property.kind == 'e')
		{
			// "curve:extend"
			if (port.IsValid())
			{
				maxon::Id fallback;
				fallback.Init(maxon::String(property.text)) iferr_return;
				const maxon::Id chosen = port.GetEffectivePortValue<maxon::Id>().GetValueOr(fallback) iferr_return;
				extend = chosen.ToString();
			}
			continue;
		}
		const maxon::Int index = maxon::Int(name[name.GetLength() - 1]) - maxon::Int('0');
		if (property.kind != 'k' || index < 0 || index >= MAX_CURVES)
			continue;

		maxon::SplineMapper spline;
		if (port.IsValid())
		{
			const maxon::Opt<const maxon::SplineMapper&> value = port.GetEffectivePortValue<maxon::SplineMapper>() iferr_return;
			if (value)
				spline = *value;
		}
		// Not "spline == nullptr": for a copy-on-write reference that compares the contents
		// (IsEqual), which crashed in math.module on a spline that has a value.
		if (spline.GetPointer() == nullptr)
		{
			spline = CreateCyclesCurve() iferr_return;
		}
		maxon::Vector2d areaMin;
		maxon::Vector2d areaMax;
		GetCyclesCurveArea(property, areaMin, areaMax);
		maxon::BaseArray<maxon::String> own;
		SampleCurve(spline, areaMin, areaMax, curves[index], own) iferr_return;
		for (const maxon::String& problem : own)
		{
			problems.Append(FormatString("curve @: @", maxon::String(property.label), problem)) iferr_return;
		}
		present[index] = true;
	}

	// Blender has one clipping rectangle for all curves of a node: the area of them all.
	maxon::Bool any = false;
	maxon::Vector2d min;
	maxon::Vector2d max;
	maxon::Int count = 0;
	for (maxon::Int index = 0; index < MAX_CURVES; ++index)
	{
		if (!present[index])
			continue;
		count = index + 1;
		min = any ? maxon::Vector2d(maxon::Min(min.x, curves[index].min.x), maxon::Min(min.y, curves[index].min.y)) : curves[index].min;
		max = any ? maxon::Vector2d(maxon::Max(max.x, curves[index].max.x), maxon::Max(max.y, curves[index].max.y)) : curves[index].max;
		any = true;
	}
	const maxon::Float clip[4] = { maxon::ClampValue(min.x, -BLENDER_CLIP_LIMIT, BLENDER_CLIP_LIMIT), maxon::ClampValue(min.y, -BLENDER_CLIP_LIMIT, BLENDER_CLIP_LIMIT),
		maxon::ClampValue(max.x, -BLENDER_CLIP_LIMIT, BLENDER_CLIP_LIMIT), maxon::ClampValue(max.y, -BLENDER_CLIP_LIMIT, BLENDER_CLIP_LIMIT) };

	JsonWriter json;
	json.BeginObject();
	json.String("extend", extend);
	if (any)
		json.Floats("clip", clip, 4);
	json.BeginArray("curves");
	for (maxon::Int index = 0; index < count; ++index)
	{
		if (!present[index])
		{
			json.Null(nullptr);   // that curve stays as Blender has it
			continue;
		}
		json.BeginArray(nullptr);
		for (const maxon::Vector2d& point : curves[index].points)
		{
			json.BeginArray(nullptr);
			json.Float(nullptr, point.x);
			json.Float(nullptr, point.y);
			json.String(nullptr, "VECTOR"_s);
			json.EndArray();
		}
		json.EndArray();
	}
	json.EndArray();
	json.EndObject();
	return json.GetText();
}

} // namespace cyclesrender
