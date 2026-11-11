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

} // namespace cyclesrender

#endif // CYCLESRENDER_MESH_EXPORT_H__
