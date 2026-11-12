// SPDX-License-Identifier: Apache-2.0
// Finds what a Cinema 4D document renders and writes it as scene items for the bridge.

#ifndef CYCLESRENDER_SCENE_EXPORTER_H__
#define CYCLESRENDER_SCENE_EXPORTER_H__

#include "c4d.h"
#include "maxon/hashmap.h"

#include "coords.h"
#include "document_colors.h"
#include "scene_writer.h"

namespace cyclesrender
{

/// The render settings that change what is exported.
struct ExportSettings
{
	maxon::Int32 width = 1280;                         ///< image size in pixels
	maxon::Int32 height = 720;
	maxon::Bool depthOfField = false;                  ///< send the camera's focus and f-stop
	maxon::Bool eevee = false;                         ///< the engine is Eevee (no panoramic camera there)
	maxon::Float64 lightMultiplier = 1.0;              ///< applied to every light
	cinema::Vector worldColor = cinema::Vector(0.05);  ///< environment when the scene has no Sky object
	maxon::Float64 worldStrength = 1.0;
	maxon::Int32 bakeResolution = 1024;                ///< side of the texture a shader is baked into
	maxon::Bool motionBlur = false;                    ///< final frames only; the caller samples the shutter (MotionSample)
	maxon::Float64 shutter = 0.5;                      ///< time the shutter is open, in frames
	maxon::Float64 shutterCentre = 0.0;                ///< middle of that time relative to the frame, in frames (motion blur "Position")
	cinema::BaseObject* camera = nullptr;              ///< render through this camera; nullptr: the render view's
	maxon::Bool writeCamera = true;                    ///< false: leave the camera item out (the IPR sends it separately)
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
/// - The volume grids one object produces (Volume Loader, Volume Builder, Pyro Output) become
///   one `volume` (volume_export.h).
/// - A Hair object becomes `curves`, a particle group a `pointcloud` (curves_export.h).
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

	/// Fills @p sample with where the collected things are now.
	maxon::Result<void> GetMotionSample(MotionSample& sample) const;

	/// Motion blur: where things are when the shutter opens and when it closes. Objects and the
	/// camera then carry those two matrices, and a mesh whose points move carries the way each
	/// point goes. Call before Write(); the samples must outlive it. Not done for instancers
	/// (the copies of a multi-instance or Cloner) and lights.
	void SetMotion(const MotionSample* open, const MotionSample* close) { _motionOpen = open; _motionClose = close; }

	/// Writes everything collected: meshes, objects, lights, materials, camera and world.
	maxon::Result<void> Write(SceneWriter& writer);

	/// One text line per collected item, for tests and debugging:
	///   mesh "Null/Cube" points=8 polygons=6 pos=(0, 100, 0) size=(200, 200, 200) material="Red"
	///   light "Key" type=0 pos=(200, 300, -200)
	///   sky "Sky" material="Panorama"
	///   camera "Camera" pos=(0, 0, -600)
	/// Positions are global, sizes are the bounding box of the mesh in its own axes, both in
	/// document units rounded to whole numbers. A path names the object and its ancestors;
	/// what a generator produces appears below the generator ("Cube/Cube").
	maxon::Result<maxon::String> GetListing() const;

private:
	enum class LEAF
	{
		MESH,
		LIGHT,
		VOLUME,
		HAIR,
		PARTICLES
	} MAXON_ENUM_LIST_CLASS(LEAF);

	/// Which rays see an object: the switches of its Compositing tag, or of the nearest one above it.
	struct Visibility
	{
		maxon::Bool camera = true;
		maxon::Bool shadow = true;         ///< casts shadows
		maxon::Bool diffuse = true;        ///< seen by bounced (GI) light
		maxon::Bool glossy = true;         ///< seen in reflections
		maxon::Bool transmission = true;   ///< seen through refracting and transparent surfaces
		maxon::Bool volumeScatter = true;  ///< seen by light scattered in volumes (Cycles Object tag only)
		maxon::Bool holdout = false;       ///< renders as a hole in the picture (Cycles Object tag only)
		maxon::Bool shadowCatcher = false; ///< shows only the shadows it receives (Cycles Object tag only)
		maxon::Bool IsDefault() const { return camera && shadow && diffuse && glossy && transmission && volumeScatter && !holdout && !shadowCatcher; }
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
		maxon::Bool hasColor = false;        ///< carries a MoGraph colour
		cinema::Vector color;
		maxon::Int instanceSet = NOTOK;      ///< index into _instanceSets
		const cinema::BaseObject* ownerObject = nullptr;   ///< the object behind `owner`
		const cinema::BaseObject* volumeSet = nullptr;     ///< volumes: the Volume Set (a loaded file) around it
		const cinema::BaseTag* linkTag = nullptr;          ///< the Cycles Object tag whose light list counts for it
		Visibility visibility;
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
		const cinema::BaseObject* volumeSet = nullptr;
		const cinema::BaseTag* linkTag = nullptr;
		maxon::Bool forceVisible = false;    ///< the next object is shown whatever its render dot says (instanced by a Cycles Object tag)
		Visibility visibility;
	};

	/// What a mesh leaf needs in Blender: the mesh item and one material per slot.
	struct Resolved
	{
		maxon::String meshId;                ///< empty: nothing to show
		maxon::BaseArray<maxon::String> materialIds;
	};

	/// @param[in] key      Identity of @p op that survives from one export to the next.
	/// @param[in] inCache  @p op is generated, not an object of the document.
	maxon::Result<void> Walk(cinema::BaseObject* op, const cinema::Matrix& mg, Inherited inherited, const maxon::String& parentPath,
		maxon::Bool instanceRoot, maxon::UInt64 key, maxon::Bool inCache);
	/// Instances the objects listed in a Cycles Object tag on the particles of a particle group
	/// or the matrices of a Matrix object. Returns false when @p op has no such tag.
	maxon::Result<maxon::Bool> WalkTagInstances(cinema::BaseObject* op, const cinema::Matrix& mg, const Inherited& inherited, const maxon::String& path, maxon::UInt64 key);
	maxon::Result<void> AddMeshLeaf(cinema::BaseObject* object, const cinema::Matrix& mg, const Inherited& inherited, const maxon::String& path,
		maxon::UInt64 key, maxon::Bool generated);
	/// The stack for @p op: @p parent extended by the object's own texture tags.
	maxon::Result<maxon::Int> ExtendStack(maxon::Int parent, cinema::BaseObject* op);
	/// The material that covers a whole object with this stack, or nullptr.
	cinema::BaseMaterial* GetStackMaterial(maxon::Int stack) const;

	class WriteState;
	maxon::Result<void> Resolve(const Leaf& leaf, WriteState& state, Resolved& resolved);
	maxon::Result<void> WriteObject(SceneWriter& writer, const Leaf& leaf, const Resolved& resolved);
	/// The light's include or exclude list as item ids. @p itemIds: the item written for each leaf, or empty.
	/// @return  False when the light has no such list.
	maxon::Result<maxon::Bool> GetLightLinking(const Leaf& light, const maxon::BaseArray<maxon::String>& itemIds, struct LightLinking& linking) const;
	/// @param[in] matrices  Global matrix of every copy.
	/// @param[in] colors    MoGraph colour of every copy as MoGraph stores it, or nullptr.
	maxon::Result<void> WriteInstancer(SceneWriter& writer, const maxon::String& id, const Resolved& resolved,
		const maxon::BaseArray<cinema::Matrix>& matrices, const maxon::BaseArray<cinema::Vector>* colors, const Visibility& visibility);

	cinema::BaseDocument* _doc;
	DocumentColors _colors;                    ///< the document's colour settings, for colours written here (lights, MoGraph)
	ExportSettings _settings;
	Coords _coords;
	maxon::BaseArray<Leaf> _leaves;
	maxon::BaseArray<TextureStack> _stacks;
	maxon::BaseArray<InstanceSet> _instanceSets;
	maxon::BaseArray<maxon::BaseArray<cinema::Matrix>*> _ownedMatrices;   ///< matrices of the instance sets made here (WalkTagInstances)
	maxon::BaseArray<maxon::String> _notes;   ///< things skipped on purpose, reported as warnings
	cinema::BaseObject* _camera = nullptr;
	cinema::BaseObject* _dome = nullptr;      ///< a Redshift dome light: the environment when there is no Sky object
	cinema::Matrix _domeMg;
	cinema::BaseObject* _sky = nullptr;
	cinema::Matrix _skyMg;
	cinema::BaseMaterial* _skyMaterial = nullptr;
	cinema::OcioConverter* _ocio = nullptr;   ///< for MoGraph colours; made when first needed
	maxon::Bool _ocioTried = false;
	const MotionSample* _motionOpen = nullptr;
	const MotionSample* _motionClose = nullptr;

public:
	/// Copies of one mesh inside one generator are sent as one instancer from this number on.
	static const maxon::Int BATCH_MINIMUM = 16;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_SCENE_EXPORTER_H__
