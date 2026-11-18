// SPDX-License-Identifier: Apache-2.0
// Writes one polygon object as a `mesh` item and a `points` item.

#ifndef CYCLESRENDER_MESH_EXPORT_H__
#define CYCLESRENDER_MESH_EXPORT_H__

#include "c4d.h"

#include "coords.h"
#include "scene_writer.h"

namespace cyclesrender
{

/// What the object's texture tags decide about its mesh.
struct MeshMaterials
{
	const maxon::Int32* polygonSlots = nullptr;   ///< material slot of every polygon; nullptr: all use slot 0
	maxon::Int32 slotCount = 1;
	const cinema::UVWTag* renderUv = nullptr;     ///< the UVW tag textures use; nullptr: the object's first
};

/// The points of an object when the shutter opens and when it closes (motion blur).
struct MeshMotion
{
	const cinema::Vector32* open = nullptr;
	const cinema::Vector32* close = nullptr;
	maxon::Int32 count = 0;                       ///< must equal the point count of the object to be used
};

//----------------------------------------------------------------------------------------
/// Exports @p op, in its local space, as two items:
///
/// - the mesh @p meshId: polygons, every UVW tag, vertex maps and vertex colours (named
///   after their tags, so materials can refer to them), and the material slot per polygon;
/// - the points item @p meshId + ".points": positions and shading normals. These are all
///   that changes while a mesh deforms, so delta sync then sends this item alone. With
///   @p motion, and if any point moves, also the way each point goes while the shutter is open.
///
/// Normals come from a Normal tag if there is one, otherwise from the Phong tag; without
/// either the mesh is flat shaded, as in Cinema 4D. N-gons are sent as the triangles and
/// quads they are stored as; they render the same.
///
/// @return  False when the object has no polygons and nothing was written.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportMesh(SceneWriter& writer, const Coords& coords, const cinema::PolygonObject* op, const maxon::String& meshId,
	const MeshMaterials& materials = MeshMaterials(), const MeshMotion& motion = MeshMotion());

//----------------------------------------------------------------------------------------
/// A fingerprint of everything ExportMesh() reads from @p op. Two objects with the same
/// fingerprint export the same mesh, which is how the clones of a generator are recognised
/// as copies of each other.
//----------------------------------------------------------------------------------------
maxon::UInt64 HashMeshSource(const cinema::PolygonObject* op);

//----------------------------------------------------------------------------------------
/// HashMeshSource() and the rest of what ExportMesh() reads from @p op: the edges that break
/// the Phong normals, the n-gons, and whether vertex colours are per point. Two objects with the
/// same value export the same items for the same mesh id, slots and units.
///
/// @param[in] how  Where the arrays' content comes from (points, polygons, UVW, normal, vertex map and
///                 vertex colour tags).
///                 BYTES: a pass over the data itself, as HashMeshSource() does it: several times
///                 cheaper than exporting the mesh, but 10 ms for a million quads, and in Cinema 4D
///                 2027 asking a generated mesh's tag for its data or their number makes the tag build
///                 them again and the view draw the mesh anew (measured: 11 ms more here and 20 ms more
///                 in the next redraw for a plane of a million quads).
///                 DATA_HASH: Cinema 4D's own hash of each array (VariableTag::GetDataHash), which it
///                 keeps with the data: nothing per polygon, and no data is asked for (0.1 ms for the
///                 same plane, 2024 and 2027). The two kinds of value do not compare with each other,
///                 and in Cinema 4D 2027 a tag's data hash is another once its data were asked for
///                 (SceneWriter::NoteMeshSource). Measurements: docs/reference/c4d-sdk.md, "Skipping
///                 unchanged meshes in the IPR".
//----------------------------------------------------------------------------------------
enum class MESHHASH
{
	BYTES,
	DATA_HASH
} MAXON_ENUM_LIST(MESHHASH);
maxon::UInt64 HashMeshContent(const cinema::PolygonObject* op, MESHHASH how = MESHHASH::BYTES);

//----------------------------------------------------------------------------------------
/// Which object @p op is and how often it and its tags were changed: its address and GUID, its
/// data dirty count, and type, address, dirty count and name of each tag. Costs nothing per
/// polygon. It changes with every edit Cinema 4D records, but not with an edit that bypasses the
/// dirty counts, and an object made anew at a freed address can repeat it: the exporter trusts it
/// only for objects of the document itself (scene_exporter.cpp, MeshSignature).
//----------------------------------------------------------------------------------------
maxon::UInt64 HashMeshIdentity(const cinema::PolygonObject* op);

} // namespace cyclesrender

#endif // CYCLESRENDER_MESH_EXPORT_H__
