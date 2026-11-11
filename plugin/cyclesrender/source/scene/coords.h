// SPDX-License-Identifier: Apache-2.0
// Conversion from Cinema 4D's coordinates and units to Blender's.
//
//   Cinema 4D: left-handed, Y up, Z forward, lengths in document units (centimetres by default).
//   Blender:   right-handed, Z up, Y forward, lengths in metres.
//
// A position (x, y, z) becomes (x, z, y) * scale. Swapping two axes mirrors space, which is
// what turns a left-handed system into a right-handed one; it also reverses the winding of
// every polygon, so mesh export reverses the corner order of each face.
//
// Cameras and lights look along +Z in Cinema 4D and along -Z in Blender, both with +Y up.
// Their matrices get the extra flip in AimedMatrix().

#ifndef CYCLESRENDER_COORDS_H__
#define CYCLESRENDER_COORDS_H__

#include "c4d.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// Converts points, directions and matrices of one document.
//----------------------------------------------------------------------------------------
class Coords
{
public:
	/// @param[in] metresPerUnit  Length of one document unit in metres (0.01 for centimetres).
	explicit Coords(maxon::Float64 metresPerUnit = 0.01) : _scale(metresPerUnit) { }

	/// Reads the unit scale from the document's project settings.
	static Coords FromDocument(const cinema::BaseDocument* doc);

	maxon::Float64 GetScale() const { return _scale; }

	/// A length in document units, in metres.
	maxon::Float64 Length(maxon::Float64 value) const { return value * _scale; }

	/// A position: axes swapped and scaled to metres. Writes three values.
	void Point(const cinema::Vector& p, maxon::Float32* out) const
	{
		out[0] = maxon::Float32(p.x * _scale);
		out[1] = maxon::Float32(p.z * _scale);
		out[2] = maxon::Float32(p.y * _scale);
	}

	/// A direction such as a normal: axes swapped, length kept. Writes three values.
	static void Direction(const cinema::Vector32& n, maxon::Float32* out)
	{
		out[0] = n.x;
		out[1] = n.z;
		out[2] = n.y;
	}

	/// The world matrix of an object that carries geometry, as 16 values in row-major order.
	/// Use it together with Point() for the object's local points.
	void ObjectMatrix(const cinema::Matrix& mg, maxon::Float64* out) const;

	/// The world matrix for the grids of a .vdb file, which Blender reads as they are: the file's
	/// Z axis (negated by Cinema 4D), the swap of axes and the conversion to metres are part of
	/// the matrix.
	/// @param[in] unitScale  Document units per unit of the data.
	void RawMatrix(const cinema::Matrix& mg, maxon::Float64 unitScale, maxon::Float64* out) const;

	/// The world matrix of a camera or light: axes normalised, looking along Blender's -Z.
	void AimedMatrix(const cinema::Matrix& mg, maxon::Float64* out) const;

private:
	maxon::Float64 _scale;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_COORDS_H__
