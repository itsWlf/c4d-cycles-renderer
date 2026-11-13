// SPDX-License-Identifier: Apache-2.0
// Collects the items and binary blobs of one `scene` request (protocol/protocol.md), and
// reduces it to what changed since the last request (delta sync).

#ifndef CYCLESRENDER_SCENE_WRITER_H__
#define CYCLESRENDER_SCENE_WRITER_H__

#include "c4d.h"
#include "maxon/hashmap.h"

#include "../session/bridge_client.h"
#include "../session/json_writer.h"

namespace cyclesrender
{

/// A fast 64-bit fingerprint of raw bytes, continuing from @p seed. It only has to tell "same
/// as last time" from "changed"; it is not stored anywhere.
maxon::UInt64 HashBytes(const maxon::Char* data, maxon::Int count, maxon::UInt64 seed);

//----------------------------------------------------------------------------------------
/// What Blender currently holds: a fingerprint of every scene item, by item id. One per
/// live session; SceneWriter::KeepChanges() compares against it and brings it up to date.
///
/// For items whose export can be skipped (the IPR's meshes, SceneWriter::KeepMesh), it also
/// keeps the signature of what each was made from (SceneWriter::NoteMeshSource).
//----------------------------------------------------------------------------------------
class SyncState
{
public:
	/// Forget everything, as after Blender's scene was cleared.
	void Reset() { _items.Reset(); _sources.Reset(); }
	maxon::Int GetItemCount() const { return _items.GetCount(); }
	/// Makes this a copy of @p other (for comparing a fresh export with it, without changing it).
	maxon::Result<void> CopyFrom(const SyncState& other);

private:
	friend class SceneWriter;
	maxon::HashMap<maxon::String, maxon::UInt64> _items;
	maxon::HashMap<maxon::String, maxon::UInt64> _sources;   ///< item id -> signature of what it was made from
};

//----------------------------------------------------------------------------------------
/// One `scene` request under construction. Exporters append items through BeginItem() and
/// bulk data through the Alloc functions. Allocate an item's blobs before or while the item
/// is open, and finish one item before the next: a blob belongs to the item that is closed
/// next, and its index counts from that item's first blob.
///
///   SceneWriter writer(true);
///   maxon::Int index = 0;
///   maxon::Float32* points = writer.AllocFloats(count * 3, index) iferr_return;
///   ... fill points ...
///   JsonWriter& json = writer.BeginItem("mesh", id);
///   json.Int("points", index);
///   writer.EndItem();
///   writer.Send(client, thread) iferr_return;
//----------------------------------------------------------------------------------------
class SceneWriter
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(SceneWriter);

public:
	/// @param[in] clear  True to make the bridge drop everything sent before.
	explicit SceneWriter(maxon::Bool clear);
	/// Lets go of the baked files the request names (HoldBakedFile).
	~SceneWriter();

	/// Opens an item of the given kind and id and returns the writer for its other fields.
	/// The reference is valid until EndItem().
	JsonWriter& BeginItem(const maxon::Char* kind, const maxon::String& id);
	void EndItem();
	/// For the open item: whenever delta sync sends the item @p id (written earlier), this one
	/// is sent too, changed or not. For data that the bridge loses when it rebuilds the other.
	void SendWith(const maxon::String& id);

	/// Reserves a blob of @p count values and returns the memory to fill. The pointer stays
	/// valid until the writer is destroyed. @p blobIndex is what the item's field must hold.
	maxon::Result<maxon::Float32*> AllocFloats(maxon::Int count, maxon::Int& blobIndex);
	maxon::Result<maxon::Int32*> AllocInts(maxon::Int count, maxon::Int& blobIndex);
	/// The same for bytes that are no numbers (a file's content).
	maxon::Result<maxon::Char*> AllocBytes(maxon::Int count, maxon::Int& blobIndex);

	maxon::Int GetItemCount() const { return _items.GetCount(); }

	/// For materials/baked_files.cpp: the request names the baked file of image item @p id. It
	/// is not deleted while the writer exists, and Send() tells which Blender gets it.
	maxon::Result<void> HoldBakedFile(const maxon::String& id);

	/// Something the user should know about, shown in Cinema 4D's console after the export.
	void Warn(const maxon::String& message);
	const maxon::BaseArray<maxon::String>& GetWarnings() const { return _warnings; }

	//----------------------------------------------------------------------------------------
	/// Delta sync: drops every item @p state says Blender already holds unchanged, and lists
	/// for removal every item of @p state that this request no longer contains. @p state then
	/// describes Blender as it will be after Send(). With clear set, everything is kept.
	/// @param[in] partial  The request holds only some items (for example just the camera):
	///                     nothing is removed, and the other entries of @p state stay.
	/// @return             The number of items left to send plus the number to remove.
	//----------------------------------------------------------------------------------------
	maxon::Result<maxon::Int> KeepChanges(SyncState& state, maxon::Bool partial = false);

	//----------------------------------------------------------------------------------------
	/// Skipping the export of a mesh Blender already has. ExportMesh() writes two items, the mesh
	/// @p meshId and its points; NoteMeshSource() then attaches @p signature, a hash of everything
	/// they were made from (scene_exporter.cpp, MeshSignature), and KeepChanges() stores it in the
	/// SyncState. Next time KeepMesh() with the same signature puts the two items into the request
	/// as Blender holds them, by their fingerprints and without their content, instead of an export.
	/// @return  False, with nothing added, unless @p state holds both items made from @p signature.
	//----------------------------------------------------------------------------------------
	maxon::Bool KeepMesh(const SyncState& state, const maxon::String& meshId, maxon::UInt64 signature);
	void NoteMeshSource(const maxon::String& meshId, maxon::UInt64 signature);
	/// True when KeepChanges() found an item of KeepMesh() that Blender does not hold as kept: the
	/// request lacks its content and must be made again without KeepMesh(). Never seen; a guard.
	maxon::Bool IsMissingKeptItems() const { return _missingKept; }
	/// After KeepChanges(): the ids of the items it left to send and of those it lists for removal.
	maxon::Result<maxon::BaseArray<maxon::String>> GetChangedIds() const;

	/// Sends the request and waits for the reply. The writer cannot be used afterwards.
	maxon::Result<void> Send(BridgeClient& client, cinema::BaseThread* cancel);

private:
	struct Item
	{
		maxon::String id;
		JsonWriter json;
		maxon::Int firstBlob = 0;
		maxon::Int blobCount = 0;
		maxon::Bool send = true;
		maxon::Int sendWith = NOTOK;     ///< index of the item whose sending forces this one
		maxon::Bool kept = false;        ///< KeepMesh(): no content, only the fingerprint below
		maxon::UInt64 keptFingerprint = 0;
		maxon::Bool hasSource = false;   ///< NoteMeshSource() or KeepMesh() gave it a signature
		maxon::UInt64 source = 0;
	};

	maxon::UInt64 Fingerprint(const Item& item) const;

	maxon::Bool _clear;
	maxon::BaseArray<Item> _items;
	maxon::BaseArray<Blob> _blobs;
	maxon::BaseArray<maxon::String> _removed;
	maxon::BaseArray<maxon::String> _warnings;
	maxon::BaseArray<maxon::String> _bakedFiles;   ///< image item ids of baked files the request names
	maxon::Int _itemFirstBlob = 0;   ///< index of the first blob of the item being written
	maxon::Bool _itemOpen = false;
	maxon::Bool _missingKept = false;
	JsonWriter _discarded;           ///< receives the fields of an item that could not be stored
};

} // namespace cyclesrender

#endif // CYCLESRENDER_SCENE_WRITER_H__
