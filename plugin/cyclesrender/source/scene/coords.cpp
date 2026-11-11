// SPDX-License-Identifier: Apache-2.0
// Cinema 4D's coordinates and units to Blender's (coords.h).

#include "c4d.h"
#include "customgui_unitscale.h"
#include "ddoc.h"

#include "coords.h"

namespace cyclesrender
{

static maxon::Float64 MetresPerUnit(cinema::DOCUMENT_UNIT unit)
{
	switch (unit)
	{
		case cinema::DOCUMENT_UNIT::KM: return 1000.0;
		case cinema::DOCUMENT_UNIT::M: return 1.0;
		case cinema::DOCUMENT_UNIT::CM: return 0.01;
		case cinema::DOCUMENT_UNIT::MM: return 0.001;
		case cinema::DOCUMENT_UNIT::MICRO: return 1e-6;
		case cinema::DOCUMENT_UNIT::NM: return 1e-9;
		case cinema::DOCUMENT_UNIT::MILE: return 1609.344;
		case cinema::DOCUMENT_UNIT::YARD: return 0.9144;
		case cinema::DOCUMENT_UNIT::FOOT: return 0.3048;
		case cinema::DOCUMENT_UNIT::INCH: return 0.0254;
		default: return 0.01;
	}
}

Coords Coords::FromDocument(const cinema::BaseDocument* doc)
{
	if (!doc)
		return Coords();

	cinema::GeData data;
	if (!doc->GetParameter(ConstDescIDLevel(cinema::DOCUMENT_DOCUNIT), data, cinema::DESCFLAGS_GET::NONE))
		return Coords();
	const cinema::UnitScaleData* const unitScale = data.GetCustomDataType<cinema::UnitScaleData>();
	if (!unitScale)
		return Coords();

	cinema::Float scale = 1.0;
	cinema::DOCUMENT_UNIT unit = cinema::DOCUMENT_UNIT::CM;
	if (!unitScale->GetUnitScale(scale, unit) || scale <= 0.0)
		return Coords();
	return Coords(scale * MetresPerUnit(unit));
}

// Writes the matrix with columns x, y, z and translation t in row-major order.
static void WriteRows(const cinema::Vector& x, const cinema::Vector& y, const cinema::Vector& z, const cinema::Vector& t, maxon::Float64* out)
{
	out[0] = x.x; out[1] = y.x; out[2] = z.x; out[3] = t.x;
	out[4] = x.y; out[5] = y.y; out[6] = z.y; out[7] = t.y;
	out[8] = x.z; out[9] = y.z; out[10] = z.z; out[11] = t.z;
	out[12] = 0.0; out[13] = 0.0; out[14] = 0.0; out[15] = 1.0;
}

// The axis swap that takes a Cinema 4D vector to Blender.
static cinema::Vector Swap(const cinema::Vector& v)
{
	return cinema::Vector(v.x, v.z, v.y);
}

void Coords::ObjectMatrix(const cinema::Matrix& mg, maxon::Float64* out) const
{
	// With P the axis swap, the linear part becomes P * L * P: Blender's local X, Y, Z axes
	// are Cinema 4D's local X, Z, Y axes, each swapped into Blender's world.
	WriteRows(Swap(mg.sqmat.v1), Swap(mg.sqmat.v3), Swap(mg.sqmat.v2), Swap(mg.off) * _scale, out);
}

void Coords::RawMatrix(const cinema::Matrix& mg, maxon::Float64 unitScale, maxon::Float64* out) const
{
	// A .vdb file is right-handed and Cinema 4D reads and writes it with Z negated: a point q
	// of the file lies at mg * ((q.x, q.y, -q.z) * unitScale) in Cinema 4D. Swapped and in metres
	// that is the matrix below applied to q unchanged. (Measured from the side with a ball
	// off-centre along Z, for a built and for a loaded volume: tests/c4d/volumes.py.)
	const maxon::Float64 scale = _scale * unitScale;
	WriteRows(Swap(mg.sqmat.v1) * scale, Swap(mg.sqmat.v2) * scale, Swap(mg.sqmat.v3) * -scale, Swap(mg.off) * _scale, out);
}

void Coords::AimedMatrix(const cinema::Matrix& mg, maxon::Float64* out) const
{
	// Right and up stay right and up; the viewing direction +Z becomes Blender's -Z.
	const cinema::Vector x = Swap(mg.sqmat.v1).GetNormalized();
	const cinema::Vector y = Swap(mg.sqmat.v2).GetNormalized();
	const cinema::Vector z = -Swap(mg.sqmat.v3).GetNormalized();
	WriteRows(x, y, z, Swap(mg.off) * _scale, out);
}

} // namespace cyclesrender
