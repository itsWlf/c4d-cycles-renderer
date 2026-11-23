// SPDX-License-Identifier: Apache-2.0
// Finds what a Cinema 4D document renders and writes it as scene items for the bridge.

#ifndef CYCLESRENDER_SCENE_EXPORTER_H__
#define CYCLESRENDER_SCENE_EXPORTER_H__

#include "c4d.h"
#include "maxon/delegate.h"
#include "maxon/hashmap.h"

#include "../materials/material_export.h"
#include "coords.h"
#include "document_colors.h"
#include "environment_export.h"
#include "light_export.h"
#include "proxy_export.h"
#include "scene_writer.h"

namespace cyclesrender
{

/// The render settings that change what is exported.
struct ExportSettings
{
	maxon::Int32 width = 1280;                         ///< image size in pixels
	maxon::Int32 height = 720;
	maxon::Float64 pixelAspect = 1.0;                  ///< width of a pixel over its height (Cinema 4D's Pixel Aspect)
	maxon::Bool depthOfField = false;                 ///< send the camera's focus and f-stop
	maxon::Bool eevee = false;                         ///< the engine is Eevee (no panoramic camera there before Blender 5.3)
	/// The Blender that renders, as BlenderVersionCode() (503 for 5.3; session/blender_install.h), and
	/// as Blender writes it. What it lacks is left out with a warning: nodes, sockets, settings and
	/// options of the Cycles node space (cycles_node_material.cpp). 0: not known, nothing is left out.
	maxon::Int32 blenderVersion = 0;
	maxon::String blenderVersionText;
	maxon::Float64 lightMultiplier = 1.0;              ///< applied to every light
	cinema::Vector worldColor = cinema::Vector(0.05);  ///< environment when the scene has no Sky object
	maxon::Float64 worldStrength = 1.0;
	maxon::Int32 bakeResolution = 1024;                ///< side of the texture a shader is baked into
	maxon::Bool motionBlur = false;                    ///< final frames only; the caller samples the shutter (MotionSample)
	maxon::Float64 shutter = 0.5;                      ///< time the shutter is open, in frames
	maxon::Float64 shutterCentre = 0.0;                ///< middle of that time relative to the frame, in frames (motion blur "Position")
	cinema::BaseObject* camera = nullptr;              ///< render through this camera; nullptr: the render view's
	maxon::Bool writeCamera = true;                    ///< false: leave the camera item out (the IPR sends it separately)
	/// The IPR's record of what Blender holds: a mesh made from what Blender's copy was made from is
	/// not exported again (SceneWriter::KeepMesh; the preference "Skip Unchanged Meshes"). nullptr:
	/// every mesh is exported, as for final frames.
	const SyncState* reuse = nullptr;
	/// Note what each mesh is made from even without reuse (a scene sent from scratch), so that the
	/// next export can skip what did not change. Implied by reuse.
	maxon::Bool noteSources = false;
	/// Note what can be written again by itself when only its place changes (TakeWrittenObjects and what
	/// goes with it): for the IPR's updates of a change in progress.
	maxon::Bool noteFollowed = false;
	/// For SceneExporter::CollectOwners(): the material ids of the last whole export (TakeMaterialIds).
	const MaterialIdTable* materials = nullptr;
	/// Judge every mesh that is not judged by its identity by a pass over its data, as before Cinema 4D's
	/// data hashes were used (MESHHASH::BYTES): for comparisons (prefs/settings.h, IsIprUnoptimized).
	maxon::Bool hashBytes = false;
};

/// What one Write() did with the meshes, for tests and measurements.
struct ExportStats
{
	maxon::Int meshesExported = 0;     ///< exported in full
	maxon::Int meshesKept = 0;         ///< left as Blender holds them (ExportSettings::reuse)
	maxon::Int meshesByIdentity = 0;   ///< judged by what the object is (the rest by a hash of their data)
	maxon::Int meshesByDataHash = 0;   ///< of the rest: judged by Cinema 4D's own hashes of their arrays, without a pass over them (MESHHASH::DATA_HASH)
	maxon::Float64 meshMilliseconds = 0.0;   ///< spent deciding about and exporting meshes
};

/// Which rays see an object: the switches of its Compositing tag, or of the nearest one above it.
struct ObjectVisibility
{
	maxon::Bool camera = true;
	maxon::Bool shadow = true;         ///< casts shadows
	maxon::Bool diffuse = true;        ///< seen by bounced (GI) light
	maxon::Bool glossy = true;         ///< seen in reflections
	maxon::Bool transmission = true;   ///< seen through refracting and transparent surfaces
	maxon::Bool volumeScatter = true;  ///< seen by light scattered in volumes (Cycles Object tag only)
	maxon::Bool holdout = false;       ///< renders as a hole in the picture (Cycles Object tag only)
	maxon::Bool shadowCatcher = false; ///< shows only the shadows it receives (Cycles Object tag only)
	maxon::Int32 passIndex = 0;        ///< Blender's pass index (the Object Index pass): the Compositing tag's first enabled Object Buffer
	maxon::Bool IsDefault() const { return camera && shadow && diffuse && glossy && transmission && volumeScatter && !holdout && !shadowCatcher && passIndex == 0; }
};

//----------------------------------------------------------------------------------------
/// An `object` item Write() made that can be written again by itself when only its place changed
/// (SceneExporter::WriteObjectAlone): everything of the item but its matrix, and how the
/// matrix follows from the object of the document it belongs to. The `proxy` item of a Cycles Proxy
/// with one place is noted the same way (@p proxy), with what it says of its file instead of a mesh. The walk forms a matrix as a product
/// from the top of the document down; @p tail is the part below @p owner (the generator's cache and
/// the objects inside it), so that the product can be formed again in the same order and the item
/// agrees with a full export's to the last bit.
//----------------------------------------------------------------------------------------
struct WrittenObject
{
	const cinema::BaseObject* owner = nullptr;      ///< the object of the document itself, or the generator that made the mesh
	maxon::String id;                               ///< its `object` item
	maxon::String meshId;
	maxon::BaseArray<maxon::String> materialIds;
	ObjectVisibility visibility;
	maxon::Bool objectKeys = false;                 ///< the item carries the keys of Redshift's random nodes
	maxon::Float64 nameKey = 0.0;
	maxon::Int32 objectId = 0;
	maxon::BaseArray<cinema::Matrix> tail;          ///< local matrices below the owner, outermost first
	maxon::Bool proxy = false;                      ///< the item is a `proxy`, not an `object`: the mesh, the materials and the keys do not count
	ProxySource source;                             ///< a proxy's file and what is taken from it, as the whole export read them
};

/// An item Write() made for what an object of the document shows (a mesh and its points, an `object`,
/// an `instancer`), with ExportSettings::noteFollowed: what goes when that object shows it no longer.
struct OwnedItem
{
	const cinema::BaseObject* owner = nullptr;
	maxon::String id;
};

/// An object of the document (@p referrer) that shows another one (@p root) and everything below it at
/// a place of its own: an Instance object in any mode, a Cycles Object tag that instances objects.
struct ObjectReference
{
	const cinema::BaseObject* referrer = nullptr;
	const cinema::BaseObject* root = nullptr;
};

/// A light Write() went through that is an object of the document itself (not made by a generator
/// or an instance), with what it needs to be written again on its own (SceneExporter::WriteLightAlone).
struct WrittenLight
{
	cinema::BaseObject* object = nullptr;
	maxon::String id;              ///< its `light` item
	maxon::Bool linked = false;    ///< it has an include or exclude list, as `linking`
	LightLinking linking;          ///< in the item ids of that export
};

//----------------------------------------------------------------------------------------
/// Where everything is at one moment, for motion blur. Taken from an exporter that has
/// collected the document at that moment (SceneExporter::GetMotionSample) and given to the
/// exporter of the frame itself (SceneExporter::SetMotion).
//----------------------------------------------------------------------------------------
struct MotionSample
{
	maxon::HashMap<maxon::UInt64, cinema::Matrix> matrices;                     ///< global matrix of every mesh, by the key of its leaf
	maxon::HashMap<maxon::UInt64, maxon::BaseArray<cinema::Vector32>> points;   ///< its points, in its own axes
	maxon::Bool hasCamera = false;
	cinema::Matrix camera;
};

//----------------------------------------------------------------------------------------
/// One export of one document at its current time.
///
/// The document must be evaluated (its caches built) before Collect() is called: a render
/// document is, and for any other document call ExecutePasses() first. Nothing in the
/// document is modified.
///
/// The walk follows the SDK's recipe (doc comment of BaseObject::GetCache): an object's
/// deform cache or cache stands in for it, objects consumed by a generator are skipped, and
/// polygon objects at the leaves are what renders. Render visibility is inherited from
/// parents, and a layer with rendering off hides its objects and what they generate.
///
/// How a leaf is written:
/// - Normally as a `mesh` (with its `points`) and an `object` showing it.
/// - Texture tags restricted to polygon selections give the mesh several material slots.
/// - A multi-instance becomes one `instancer` per mesh of the object it refers to.
/// - A Cycles Scatter object becomes one `instancer` per mesh of each child it scatters, with a matrix and
///   a MoGraph colour per copy (objects/scatter_object.h); it is the whole export's in the IPR.
/// - The volume grids one object produces (Volume Loader, Volume Builder, Pyro Output) become
///   one `volume` (volume_export.h).
/// - A Hair object becomes `curves`, a particle group a `pointcloud` (curves_export.h).
/// - The world is the highest of the Sky objects, Redshift dome lights and Cycles Environment objects
///   that render, in the order of the Object Manager; each other one is named in the console.
/// - A Cycles Fog object becomes a box `mesh` of its size with a material of its own that has only
///   a volume, and an `object` at its place, which moves alone in the IPR like a mesh's (fog_export.h);
///   with a noise, its sampled grid as a `volume`, written by a whole export only.
/// - A Cycles Proxy object becomes a `proxy` item: its Blender file's path and date, what to take from
///   it, and its place (proxy_export.h); it moves alone in the IPR like a mesh's object. Inside a
///   multi-instance or below a Cycles Scatter it is one `proxy` item with a matrix per copy.
/// - A Compositing tag's "seen by" and "cast shadows" switches travel with the objects below
///   it; a light's include or exclude list becomes light linking.
/// - What a generator produces is compared: copies of one mesh with the same materials are
///   sent as one `instancer` when there are at least BATCH_MINIMUM of them. The clones of a
///   Cloner always are, because an instancer is what carries MoGraph's colour per clone.
//----------------------------------------------------------------------------------------
class SceneExporter
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(SceneExporter);

public:
	SceneExporter(cinema::BaseDocument* doc, const ExportSettings& settings);
	~SceneExporter();

	/// Walks the document and remembers what renders. Call once, before the other two.
	maxon::Result<void> Collect();

	//----------------------------------------------------------------------------------------
	/// Instead of Collect(): remembers only what the objects of the document in @p owners show
	/// themselves (not what the objects below them show, unless those are listed too), each with what it
	/// inherits from the objects above it, exactly as Collect() finds it. Write() then writes only their
	/// meshes, `object` and `instancer` items, the same, byte for byte, as a whole export gives for them
	/// as long as the conditions below hold, and no material, light, camera or world: the IPR's update of
	/// single objects whose generated or deformed meshes changed. ExportSettings::materials
	/// must name the ids of the last whole export.
	///
	/// What the caller has to make sure of, because items depend on more than their own object:
	/// - Objects that can show one polygon object are listed together (an object and every Instance
	///   object or Cycles Object tag that shows it or an object above it: GetReferences()): the first of
	///   them in the document names the mesh. They are given in the document's order.
	/// - None of them was noted by GetFixedOwners() in the last whole export.
	/// Cycles Fog boxes and Cycles Proxies are written too (their items are their own).
	/// After Write(), IsRefused() says that something turned up that only a whole export writes right (a
	/// light, a volume, hair or particles of a generator, the sky, a material the last whole export did
	/// not write): what was written is then to be thrown away.
	//----------------------------------------------------------------------------------------
	maxon::Result<void> CollectOwners(const maxon::BaseArray<const cinema::BaseObject*>& owners);
	maxon::Bool IsRefused() const { return _refused; }

	/// Fills @p sample with where the collected things are now.
	maxon::Result<void> GetMotionSample(MotionSample& sample) const;

	/// Motion blur: where things are when the shutter opens and when it closes. Objects and the
	/// camera then carry those two matrices, and a mesh whose points move carries the way each
	/// point goes. Call before Write(); the samples must outlive it. Not done for instancers
	/// (the copies of a multi-instance or Cloner) and lights.
	void SetMotion(const MotionSample* open, const MotionSample* close) { _motionOpen = open; _motionClose = close; }

	/// Writes everything collected: meshes, objects, lights, materials, camera and world.
	maxon::Result<void> Write(SceneWriter& writer);
	const ExportStats& GetStats() const { return _stats; }

	/// After Write(): the lights of the document it went through, also those that emitted nothing
	/// and were left out. Hands them over; a second call returns nothing.
	maxon::BaseArray<WrittenLight> TakeWrittenLights() { return std::move(_writtenLights); }

	//----------------------------------------------------------------------------------------
	/// After Write() with ExportSettings::noteFollowed: what the IPR needs to send a change of place by
	/// itself, and to know when it must not.
	/// - TakeWrittenObjects(): the `object` items that follow their owner's place and nothing else. Hands
	///   them over.
	/// - GetUnfollowedOwners(): objects of the document that own something whose place cannot be written
	///   alone: an instancer (a Cloner's clones, a multi-instance, copies batched into one), a volume, hair,
	///   particles, a light made by a generator, the sky or a dome light (the world), and everything shown
	///   through a render instance or a Cycles Object tag's instances. When one of them, or an object above
	///   it, moves, the scene is exported whole.
	/// - GetReferences(): the objects that an Instance object (in any mode) or a Cycles Object tag shows,
	///   each with the object that shows it. What lies below such an object shows at the instance's place
	///   as well (measured: an Instance object's copy follows without any counter of the Instance object
	///   changing), so when it changes, what shows it is written again with it.
	/// - GetDeformedOwners(): objects of the document with a deformed mesh among what they show. Their
	///   own cache counter does not say when that mesh is deformed anew (measured in Cinema 4D 2024: only
	///   the counter of the object inside that carries the deformed copy moves), so the IPR looks into
	///   their caches.
	/// - GetFixedOwners(): objects of the document whose items only a whole export writes right (they are
	///   among the unfollowed ones): those that show a volume, hair, particles, a light made by a
	///   generator, the sky or a dome light, and those whose mesh is shared with another object's because
	///   the two have the same content (the copies of generators that make many).
	/// - TakeOwnedItems(): the mesh, points, `object` and `instancer` items by the object of the document
	///   they were written for. Hands them over.
	/// - TakeMaterialIds(): the id each material got (MaterialIdTable). Hands them over.
	//----------------------------------------------------------------------------------------
	maxon::BaseArray<WrittenObject> TakeWrittenObjects() { return std::move(_writtenObjects); }
	const maxon::BaseArray<const cinema::BaseObject*>& GetUnfollowedOwners() const { return _unfollowedOwners; }
	const maxon::BaseArray<const cinema::BaseObject*>& GetDeformedOwners() const { return _deformedOwners; }
	const maxon::BaseArray<const cinema::BaseObject*>& GetFixedOwners() const { return _fixedOwners; }
	const maxon::BaseArray<ObjectReference>& GetReferences() const { return _references; }
	const maxon::BaseArray<OwnedItem>& GetOwnedItems() const { return _ownedItems; }
	maxon::BaseArray<OwnedItem> TakeOwnedItems() { return std::move(_ownedItems); }
	MaterialIdTable TakeMaterialIds() { return std::move(_materialIds); }
	/// After a whole Write(): a light was written with an include or exclude list, which names items.
	maxon::Bool HasLightLinking() const { return _lightLinking; }

	/// The world of a whole Write(), as far as a step can write it again by itself (WriteWorldAlone).
	struct WrittenWorld
	{
		/// Every role is filled by a Cycles Environment that is an object of the document, or by nothing (and
		/// the camera's or the light's by one): the world can be written alone. Any other environment, a Sky or
		/// a dome light is then noted as fixed (GetFixedOwners), so that a change of it is a whole export.
		maxon::Bool alone = false;
		const cinema::BaseObject* objects[3] = { nullptr, nullptr, nullptr };   ///< what the camera sees, what lights the scene, the fog; nullptr: none
		EnvironmentRoles roles[3];      ///< the roles each of them asked for
		maxon::String images;           ///< the `image` items the world read, joined
	};
	const WrittenWorld& GetWrittenWorld() const { return _writtenWorld; }

	//----------------------------------------------------------------------------------------
	/// Writes the world of the last whole Write() again by itself from @p environments, the objects of
	/// @p written found again (nullptr: gone), each at its place formed as the walk forms it: the IPR's
	/// update of an environment whose values or place change, and of the sun of a light linked to one.
	/// @return  False when only a whole export can write it right: an environment gone, switched off or
	///          asking for other roles than then, or the world reading other images than then (the old one
	///          is to be removed).
	//----------------------------------------------------------------------------------------
	static maxon::Result<maxon::Bool> WriteWorldAlone(SceneWriter& writer, cinema::BaseDocument* doc, const ExportSettings& settings,
		const cinema::BaseObject* const environments[3], const WrittenWorld& written);

	//----------------------------------------------------------------------------------------
	/// The global matrix of an object of the document as the walk forms it (Collect, CollectOwners): the
	/// top object's local matrix times each child's, from the top down, in that order. Not GetMg(), which
	/// may round differently in the last bit: an item written alone (WriteObjectAlone, WriteLightAlone)
	/// has to equal a whole export's to the last bit, so everything that places one uses this function,
	/// or continues its product one object further down (`WalkMatrix(parent) * child->GetMl()`).
	//----------------------------------------------------------------------------------------
	static cinema::Matrix WalkMatrix(const cinema::BaseObject* op);

	//----------------------------------------------------------------------------------------
	/// Writes one `object` item of the last Write() again for a new place of its owner, without walking
	/// the document: the IPR's update of an object that is being moved. The item is the
	/// same, byte for byte, as Write() gives once the owner stands at @p ownerMg, as long as nothing but
	/// places changed, so that the full export afterwards finds it unchanged.
	/// @param[in] ownerMg  The owner's global matrix, formed as the walk forms it (WalkMatrix).
	//----------------------------------------------------------------------------------------
	static void WriteObjectAlone(SceneWriter& writer, const Coords& coords, const WrittenObject& object, const cinema::Matrix& ownerMg);

	//----------------------------------------------------------------------------------------
	/// Writes one light of @p doc as Write() would, without walking the document: the IPR's light-only
	/// update, for a change of a light's values while Cinema 4D is in a modal loop. The item
	/// is the same, byte for byte, as Write() gives with the same @p settings and the light's
	/// @p linking (its global matrix is formed from the local ones as the walk forms it), so that the
	/// full export afterwards finds it unchanged.
	/// @param[in] light    A light that Write() found as an object of the document (WrittenLight).
	/// @return             False when the light emits nothing and was not written.
	//----------------------------------------------------------------------------------------
	static maxon::Result<maxon::Bool> WriteLightAlone(SceneWriter& writer, cinema::BaseDocument* doc, const ExportSettings& settings,
		const cinema::BaseObject* light, const maxon::String& id, const LightLinking* linking);

	/// One text line per collected item, for tests and debugging:
	///   mesh "Null/Cube" points=8 polygons=6 pos=(0, 100, 0) size=(200, 200, 200) material="Red"
	///   light "Key" type=0 pos=(200, 300, -200)
	///   sky "Sky" material="Panorama"
	///   camera "Camera" pos=(0, 0, -600)
	/// Positions are global, sizes are the bounding box of the mesh in its own axes, both in
	/// document units rounded to whole numbers. A path names the object and its ancestors;
	/// what a generator produces appears below the generator ("Cube/Cube").
	maxon::Result<maxon::String> GetListing() const;

	//----------------------------------------------------------------------------------------
	/// After Collect(): where a ray first meets a mesh that renders and that the camera sees (its
	/// Compositing or Cycles Object tag lets the camera see it), every copy of a multi-instance
	/// included; hair, volumes and particles are not looked at. For the IPR's navigation:
	/// the point under the mouse. Both sides of a polygon count.
	/// @param[in] origin     Where the ray starts, in document coordinates.
	/// @param[in] direction  Its direction, normalised.
	/// @param[in] length     How far it reaches.
	/// @param[in] accept     Whether the meshes of an object of the document (or made by it) count;
	///                       an empty delegate: all.
	/// @param[out] distance  From @p origin to the hit, when there is one.
	/// @return               False when the ray meets nothing within @p length.
	//----------------------------------------------------------------------------------------
	maxon::Bool FindNearestHit(const cinema::Vector& origin, const cinema::Vector& direction, maxon::Float64 length,
		const maxon::Delegate<maxon::Bool(const cinema::BaseObject*)>& accept, maxon::Float64& distance) const;

	/// For tests and measurements of the IPR's reuse of unchanged meshes: a JSON array with one
	/// object per mesh leaf: "path", "generated", "deformed", "object" (address), "identity"
	/// (HashMeshIdentity), "content" (HashMeshContent), and of the object of the document it belongs
	/// to "owner" (address) and "owner_dirty" (data, matrix, cache and children dirty counts).
	maxon::Result<maxon::String> GetSourceListing() const;

private:
	enum class LEAF
	{
		MESH,
		LIGHT,
		VOLUME,
		HAIR,
		PARTICLES,
		FOG,              ///< a Cycles Fog object: a box mesh of its own, its volume material, an `object` (fog_export.h)
		PROXY             ///< a Cycles Proxy object: a `proxy` item naming its Blender file (proxy_export.h)
	} MAXON_ENUM_LIST_CLASS(LEAF);

	using Visibility = ObjectVisibility;

	/// A local matrix the walk multiplied on below an object of the document, and the one before it
	/// (WrittenObject::tail): kept only with ExportSettings::noteFollowed.
	struct TailNode
	{
		maxon::Int parent = NOTOK;         ///< index into _tails, or NOTOK for the first below the owner
		cinema::Matrix ml;
	};

	/// One texture tag as far as the exporter reads it.
	struct TextureEntry
	{
		cinema::BaseMaterial* material = nullptr;
		maxon::String restriction;           ///< name of a polygon selection, or empty
		const cinema::BaseTag* tag = nullptr;
	};

	/// The texture tags that apply to an object, in the order Cinema 4D applies them: those of
	/// its ancestors first, its own last. A later entry covers an earlier one.
	using TextureStack = maxon::BaseArray<TextureEntry>;

	/// The copies of a multi-instance: arrays owned by the instance object.
	struct InstanceSet
	{
		const maxon::BaseArray<cinema::Matrix>* matrices = nullptr;       ///< global
		const maxon::BaseArray<maxon::Color64>* colors = nullptr;
	};

	/// MoGraph's colour per clone, and where each clone sits relative to its generator.
	struct CloneColors
	{
		maxon::BaseArray<cinema::Vector> colors;
		maxon::BaseArray<cinema::Vector> offsets;
	};

	struct Leaf
	{
		LEAF kind = LEAF::MESH;
		cinema::BaseObject* object = nullptr;
		cinema::Matrix mg;                   ///< global; inside a multi-instance: relative to each copy
		maxon::Int stack = NOTOK;            ///< index into _stacks, or NOTOK without texture tags
		maxon::String path;
		maxon::UInt64 key = 0;               ///< identifies the leaf from one export to the next
		maxon::UInt64 owner = 0;             ///< key of the object of the document it belongs to or was generated by
		maxon::Bool generated = false;       ///< part of a generator's cache
		maxon::Bool deformed = false;        ///< a deform cache, or inside one
		maxon::Bool hasColor = false;        ///< carries a MoGraph colour
		cinema::Vector color;
		maxon::Int instanceSet = NOTOK;      ///< index into _instanceSets
		const cinema::BaseObject* ownerObject = nullptr;   ///< the object behind `owner`
		maxon::Int tail = NOTOK;             ///< index into _tails: the matrices between the owner's and `mg`
		maxon::Bool followable = true;       ///< `mg` follows from the owner's place and the tail alone (not shown through an instance)
		const cinema::BaseObject* volumeSet = nullptr;     ///< volumes: the Volume Set (a loaded file) around it
		const cinema::BaseTag* linkTag = nullptr;          ///< the Cycles Object tag whose light list counts for it
		Visibility visibility;
		/// The keys Redshift's random nodes (Jitter, Random Color Switch) vary by, as the object
		/// properties OBJECT_NAME_KEY and OBJECT_ID_KEY (material_export.h): a number in 0..1 from
		/// the names on the way to the leaf (ObjectKeyPath), and the Object ID of the nearest Redshift
		/// Object tag that overrides it (0 without one).
		maxon::Float64 nameKey = 0.0;
		maxon::Int32 objectId = 0;
	};

	/// What an object hands down to its children and to the objects it generates.
	struct Inherited
	{
		maxon::Bool visible = true;          ///< render visibility dot, resolved through the parents
		maxon::Bool layerVisible = true;     ///< false inside the cache of an object on a hidden layer
		maxon::Int stack = NOTOK;
		maxon::Int instanceDepth = 0;        ///< guards against instances that refer to each other
		maxon::UInt64 owner = 0;
		maxon::Bool hasColor = false;
		cinema::Vector color;
		maxon::Int instanceSet = NOTOK;
		const CloneColors* clones = nullptr; ///< set for the root of a Cloner's cache only
		const cinema::BaseObject* ownerObject = nullptr;
		maxon::Int tail = NOTOK;             ///< Leaf::tail of what is being walked
		maxon::Bool followable = true;       ///< Leaf::followable
		const cinema::BaseObject* volumeSet = nullptr;
		const cinema::BaseTag* linkTag = nullptr;
		maxon::Bool forceVisible = false;    ///< the next object is shown whatever its render dot says (instanced by a Cycles Object tag)
		maxon::Bool deformed = false;        ///< inside a deform cache
		/// The next object is the cache of a Sphere that is not itself the input of a generator. A
		/// render document that Cinema 4D prepared for its own renderer (before 2026.2, sdk_compat.h,
		/// RENDERER_INFO) marks that cache as consumed when "Render Perfect" is on, as its renderer
		/// draws a perfect sphere instead; here the mark is ignored and the polygons render.
		maxon::Bool sphereCache = false;
		Visibility visibility;
		/// The names from the top down to the object being walked, joined by "/", which Redshift's
		/// random nodes vary by in their "Name ID" mode (measured: equal paths, equal values; see
		/// Walk). Equal to the walk's path except below a render instance, which takes the path of
		/// the object it shows. Walk sets it for the object it is at; children start from it.
		maxon::String keyPath;
		maxon::Int32 objectId = 0;           ///< Redshift Object tag's Object ID, when one above overrides it
	};

	/// What a mesh leaf needs in Blender: the mesh item and one material per slot.
	struct Resolved
	{
		maxon::String meshId;                ///< empty: nothing to show
		maxon::BaseArray<maxon::String> materialIds;
		maxon::Bool objectKeys = false;      ///< one of the materials reads the object keys (Leaf::nameKey)
	};

	/// @param[in] key      Identity of @p op that survives from one export to the next.
	/// @param[in] inCache  @p op is generated, not an object of the document.
	maxon::Result<void> Walk(cinema::BaseObject* op, const cinema::Matrix& mg, Inherited inherited, const maxon::String& parentPath,
		maxon::Bool instanceRoot, maxon::UInt64 key, maxon::Bool inCache);
	/// Instances the objects listed in a Cycles Object tag on the particles of a particle group
	/// or the matrices of a Matrix object. Returns false when @p op has no such tag.
	maxon::Result<maxon::Bool> WalkTagInstances(cinema::BaseObject* op, const cinema::Matrix& mg, const Inherited& inherited, const maxon::String& path, maxon::UInt64 key);
	/// Instances the sources of a Cycles Scatter object (@p op, its green check on) on its copies, as
	/// WalkTagInstances does for a tag: each source walked once, with one instance set of the copies that show it
	/// (matrices and MoGraph colours). Its children are not walked as objects of their own.
	maxon::Result<void> WalkScatter(cinema::BaseObject* op, const cinema::Matrix& mg, const Inherited& inherited, const maxon::String& path, maxon::UInt64 key,
		maxon::Bool inCache);
	maxon::Result<void> AddMeshLeaf(cinema::BaseObject* object, const cinema::Matrix& mg, const Inherited& inherited, const maxon::String& path,
		maxon::UInt64 key, maxon::Bool generated);
	/// The stack for @p op: @p parent extended by the object's own texture tags.
	maxon::Result<maxon::Int> ExtendStack(maxon::Int parent, cinema::BaseObject* op);
	/// The material that covers a whole object with this stack, or nullptr.
	cinema::BaseMaterial* GetStackMaterial(maxon::Int stack) const;

	class WriteState;
	maxon::Result<void> Resolve(const Leaf& leaf, WriteState& state, Resolved& resolved);
	/// Everything the mesh items of @p leaf are made from (ExportSettings::reuse), as one hash.
	/// @param[in] count  Count the leaf in the statistics (false for the second reading after its export).
	maxon::UInt64 MeshSignature(const Leaf& leaf, const maxon::String& meshId, const struct MeshMaterials& materials, maxon::Bool count);
	/// Writes the `object` item of a mesh leaf, and with ExportSettings::noteFollowed notes it, or its
	/// owner when its place cannot be written alone.
	maxon::Result<void> WriteObject(SceneWriter& writer, const Leaf& leaf, const Resolved& resolved);
	/// Writes the `proxy` item of a Cycles Proxy leaf (one place, or a matrix per copy of the multi-instance
	/// it is in), with the same notes. A proxy without a file that is found is named in a warning and left out.
	/// @return  The item's id, or an empty string when nothing was written.
	maxon::Result<maxon::String> WriteProxy(SceneWriter& writer, const Leaf& leaf);
	/// The walk went one level down inside a generator's cache: @p ml joins the tail. Returns the new
	/// index, or NOTOK when tails are not kept.
	maxon::Result<maxon::Int> ExtendTail(maxon::Int parent, const cinema::Matrix& ml);
	/// Notes that the place of what @p owner owns cannot be written alone.
	maxon::Result<void> NoteUnfollowed(const cinema::BaseObject* owner);
	/// Notes that only a whole export writes what @p owner owns (GetFixedOwners); it is unfollowed too.
	maxon::Result<void> NoteFixed(const cinema::BaseObject* owner);
	/// Notes an item written for what @p owner shows (TakeOwnedItems).
	maxon::Result<void> NoteOwned(const cinema::BaseObject* owner, const maxon::String& id);
	/// Notes that @p referrer shows @p root (GetReferences).
	maxon::Result<void> NoteReference(const cinema::BaseObject* referrer, const cinema::BaseObject* root);
	/// The light's include or exclude list as item ids. @p itemIds: the item written for each leaf, or empty.
	/// @return  False when the light has no such list.
	maxon::Result<maxon::Bool> GetLightLinking(const Leaf& light, const maxon::BaseArray<maxon::String>& itemIds, struct LightLinking& linking) const;
	/// @param[in] matrices  Global matrix of every copy.
	/// @param[in] colors    MoGraph colour of every copy as MoGraph stores it, or nullptr.
	/// @param[in] keys      The object keys of every copy (x the name key, y the Object ID), or nullptr.
	maxon::Result<void> WriteInstancer(SceneWriter& writer, const maxon::String& id, const Resolved& resolved,
		const maxon::BaseArray<cinema::Matrix>& matrices, const maxon::BaseArray<cinema::Vector>* colors, const maxon::BaseArray<maxon::Vector2d>* keys,
		const Visibility& visibility);

	cinema::BaseDocument* _doc;
	DocumentColors _colors;                    ///< the document's colour settings, for colours written here (lights, MoGraph)
	ExportSettings _settings;
	Coords _coords;
	maxon::BaseArray<Leaf> _leaves;
	maxon::BaseArray<TextureStack> _stacks;
	maxon::BaseArray<InstanceSet> _instanceSets;
	maxon::BaseArray<maxon::BaseArray<cinema::Matrix>*> _ownedMatrices;   ///< matrices of the instance sets made here (WalkTagInstances, WalkScatter)
	maxon::BaseArray<maxon::BaseArray<maxon::Color64>*> _ownedColors;     ///< colours of the instance sets made here (WalkScatter)
	maxon::BaseArray<maxon::String> _notes;   ///< things skipped on purpose, reported as warnings
	cinema::BaseObject* _camera = nullptr;
	/// What can be the world, in the order of the walk: the highest in the Object Manager first. Per role
	/// the first that asks for it is used (Write()); the others are named.
	struct WorldSource
	{
		WORLDPART kind = WORLDPART::SKY;
		cinema::BaseObject* object = nullptr;
		cinema::Matrix mg;
		cinema::BaseMaterial* material = nullptr;          ///< a Sky's
		const cinema::BaseObject* owner = nullptr;         ///< the object of the document it belongs to
		maxon::Bool own = false;                           ///< it is an object of the document itself (not made by a generator)
		EnvironmentRoles roles;                            ///< a Sky and a dome light: the camera's and the light's
	};
	maxon::BaseArray<WorldSource> _worlds;
	WrittenWorld _writtenWorld;
	cinema::OcioConverter* _ocio = nullptr;   ///< for MoGraph colours; made when first needed
	maxon::Bool _ocioTried = false;
	const MotionSample* _motionOpen = nullptr;
	const MotionSample* _motionClose = nullptr;
	ExportStats _stats;
	maxon::BaseArray<WrittenLight> _writtenLights;
	maxon::BaseArray<TailNode> _tails;
	maxon::BaseArray<WrittenObject> _writtenObjects;
	maxon::BaseArray<const cinema::BaseObject*> _unfollowedOwners;
	maxon::BaseArray<const cinema::BaseObject*> _deformedOwners;
	maxon::BaseArray<const cinema::BaseObject*> _fixedOwners;
	maxon::BaseArray<ObjectReference> _references;
	maxon::BaseArray<OwnedItem> _ownedItems;
	MaterialIdTable _materialIds;
	/// CollectOwners(): the objects of the document to look at, and the objects above them.
	maxon::Bool _partial = false;
	maxon::HashSet<const cinema::BaseObject*> _only;
	maxon::HashSet<const cinema::BaseObject*> _onlyAbove;
	maxon::Bool _refused = false;
	maxon::Bool _lightLinking = false;

public:
	/// Copies of one mesh inside one generator are sent as one instancer from this number on.
	static const maxon::Int BATCH_MINIMUM = 16;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_SCENE_EXPORTER_H__
