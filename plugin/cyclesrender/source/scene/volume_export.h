// SPDX-License-Identifier: Apache-2.0
// Writes the volume grids one object produces (Volume Loader, Volume Builder, Pyro Output) as
// a `volume` item with its material.

#ifndef CYCLESRENDER_VOLUME_EXPORT_H__
#define CYCLESRENDER_VOLUME_EXPORT_H__

#include "c4d.h"
#include "lib_volumeobject.h"
#include "maxon/volume.h"

#include "coords.h"
#include "scene_writer.h"

namespace cyclesrender
{

/// The grids of one volume, as the scene walk found them in an object's cache.
struct VolumeSource
{
	const cinema::BaseObject* owner = nullptr;   ///< the object of the document that produced them; carries the Cycles Object tag
	const cinema::BaseObject* set = nullptr;     ///< the Volume Set (a loaded .vdb file) the grids sit in, or nullptr
	cinema::Matrix mg;                           ///< global matrix of what holds the grids (their parent in the cache)
	maxon::BaseArray<const cinema::VolumeObject*> grids;
};

//----------------------------------------------------------------------------------------
/// Blender reads volumes from .vdb files only, so:
///
/// - Grids a Volume Loader read from a file are not written again: the item names that file.
/// - Any other grids (Volume Builder, Pyro without a cache on disk) are saved to a .vdb file
///   in <preferences>/cyclesrender/volumes. Nothing is written while the grids are the same
///   objects as at the last export, so a volume that stands still is saved and sent once per
///   render job; a new file replaces and deletes the one before.
///
/// Only fog volumes render; a signed distance field is skipped (Cinema 4D meshes those).
/// The file keeps Cinema 4D's axes and units; the item's matrix converts them. A volume
/// object's own matrix is its grid's transform, global for grids built in the scene: a saved
/// file therefore needs no placing, and a loaded file is placed by the loader.
///
/// The material is a Principled Volume that reads the grid named "density" (or the first
/// fog grid), and "temperature" (as fire) and "color" when they exist. A Cycles Object tag
/// on the owner sets its density, colour, emission and blackbody strength.
///
/// @param[in] id  Id of the `volume` item; the material item is `<id>.material`.
/// @return        False when there is no fog grid and nothing was written.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportVolume(SceneWriter& writer, const Coords& coords, cinema::BaseDocument* doc, const maxon::String& id,
	const VolumeSource& source);

//----------------------------------------------------------------------------------------
/// The .vdb file last saved for the volume item @p id (SaveVolumeFile), if it was made from
/// @p sourceKey (a hash of everything its grids follow from) and still exists; otherwise an empty
/// name: the grids are to be made and saved anew.
//----------------------------------------------------------------------------------------
cinema::Filename FindVolumeFile(const maxon::String& id, maxon::UInt64 sourceKey);

//----------------------------------------------------------------------------------------
/// Saves @p volumes into <preferences>/cyclesrender/volumes, in a file named after @p id and its
/// content, as ExportVolume() saves the grids of a Volume Builder: the file saved before for @p id is
/// deleted, and FindVolumeFile() gives the new one while @p sourceKey stays the same. The grids keep
/// the axes and units Cinema 4D gives them (Coords::RawMatrix places them). Also for the noise of a
/// Cycles Fog (fog_export.h).
//----------------------------------------------------------------------------------------
maxon::Result<cinema::Filename> SaveVolumeFile(const maxon::String& id, maxon::BaseArray<maxon::Volume>& volumes, maxon::UInt64 sourceKey);

/// Deletes the .vdb files this process wrote. Call when the plugin is unloaded.
void FreeVolumeFiles();

} // namespace cyclesrender

#endif // CYCLESRENDER_VOLUME_EXPORT_H__
