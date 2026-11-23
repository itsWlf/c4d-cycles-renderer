// SPDX-License-Identifier: Apache-2.0
// The scene walk, and the writing of what it found (scene_exporter.h).
//
// Two passes. Collect() walks the object tree once (Walk) and records every "leaf", a thing
// that renders, together with what it inherited from the objects above it: visibility,
// texture tags, the Cycles Object tag, MoGraph colours. Write() then turns the leaves into
// scene items: it works out the materials per polygon (Resolve), groups equal copies into
// instancers, and writes the lights last, because their include and exclude lists name the
// items the other leaves became.

#include "c4d.h"
#include "c4d_baseeffectordata.h"
#include "ddoc.h"
#include "lib_instanceobject.h"
#include "lib_particlegroupobject.h"
#include "maxon/hashmap.h"
#include "oinstance.h"
#include "olight.h"
#include "orslight.h"
#include "customgui_inexclude.h"
#include "ocyclesenvironment.h"
#include "ocyclesfog.h"
#include "ocyclesproxy.h"
#include "ocycleslight.h"
#include "tcompositing.h"
#include "tcyclesobject.h"
#include "ttexture.h"

#include "../ids.h"
#include "../lights/light_tag.h"
#include "../materials/material_export.h"
#include "../objects/scatter_object.h"
#include "../sdk_compat.h"
#include "camera_export.h"
#include "curves_export.h"
#include "lib_hair.h"
#include "environment_export.h"
#include "fog_export.h"
#include "light_export.h"
#include "mesh_export.h"
#include "parameters.h"
#include "redshift_ids.h"
#include "scene_exporter.h"
#include "volume_export.h"

namespace cyclesrender
{

// Instances may refer to instances; this stops a chain that refers back to itself.
static const maxon::Int MAX_INSTANCE_DEPTH = 32;

// A clone is matched to MoGraph's data by its position when the cache does not list the
// clones one by one; positions closer than this (document units) are the same.
static const maxon::Float64 CLONE_MATCH_DISTANCE = 1e-4;

// Item ids must stay the same from one export to the next, or delta sync would send
// everything again. Objects of the document have a persistent GUID; what a generator produces
// is identified by its generator and its place in the generated hierarchy.
static maxon::UInt64 MixKeys(maxon::UInt64 a, maxon::UInt64 b)
{
	maxon::UInt64 hash = (a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2))) * 0xFF51AFD7ED558CCDull;
	return hash ^ (hash >> 33);
}

// The key an object of the document is walked under: what names its items from one export to the next.
// Its GUID, which stays while the object does; but a particle group's GUID is another at every frame of
// its simulation (measured, Cinema 4D 2027: docs/reference/c4d-sdk.md, "Instances and MoGraph"), which gave everything
// instanced on its particles new ids, and so new meshes and instancers in Blender, at every frame. Its
// marker stays, so that is hashed instead.
static maxon::UInt64 ObjectKey(const cinema::BaseObject* op)
{
	if (op->GetType() != Ofpgroup)
		return op->GetGUID();
	void* data = nullptr;
	maxon::Int32 size = 0;
	op->GetMarker().GetMemory(data, size);
	if (!data || size <= 0)
		return op->GetGUID();
	return HashBytes(static_cast<const maxon::Char*>(data), size, 0x70617274);
}

static maxon::String KeyId(const maxon::Char* prefix, maxon::UInt64 key)
{
	return maxon::String(prefix) + maxon::String::HexToString(key, false);
}

// A path of names joined by "/" ("Cloner/Null/Cube 0").
static maxon::String JoinPath(const maxon::String& parent, const maxon::String& name)
{
	if (name.IsEmpty())
		return parent;
	return parent.IsEmpty() ? name : parent + "/"_s + name;
}

// The names from the top of @p op's tree down to @p op.
static maxon::String TreePath(const cinema::BaseObject* op)
{
	maxon::String path;
	for (; op; op = op->GetUp())
		path = path.IsEmpty() ? maxon::String(op->GetName()) : maxon::String(op->GetName()) + "/"_s + path;
	return path;
}

// The name key of Redshift's random nodes: a number in 0..1 (24 bits, exact in a float) from a
// path, by FNV-1a over its UTF-8 bytes, the same in every session and Cinema 4D version. Equal
// paths give equal keys, as Redshift's own (other) hash does.
static maxon::Float64 NameKey(const maxon::String& path)
{
	maxon::UInt32 hash = 2166136261u;
	iferr (const maxon::BaseArray<maxon::Char> bytes = path.GetCString())
		return 0.0;
	for (const maxon::Char c : bytes)
	{
		if (c == 0)
			break;
		hash = (hash ^ maxon::UInt32(maxon::UChar(c))) * 16777619u;
	}
	return maxon::Float64(hash >> 8) / 16777216.0;
}

static maxon::String VectorText(const cinema::Vector& v)
{
	// Rounded to whole units, and -0 avoided, so listings compare as text.
	const maxon::Int64 x = maxon::Int64(maxon::Floor(v.x + 0.5));
	const maxon::Int64 y = maxon::Int64(maxon::Floor(v.y + 0.5));
	const maxon::Int64 z = maxon::Int64(maxon::Floor(v.z + 0.5));
	return FormatString("(@, @, @)", x, y, z);
}

// MoGraph's colour and position of every visible clone of a generator such as the Cloner.
static maxon::Result<maxon::Bool> ReadCloneColors(cinema::BaseObject* op, maxon::BaseArray<cinema::Vector>& colors, maxon::BaseArray<cinema::Vector>& offsets)
{
	iferr_scope;

	cinema::BaseTag* const tag = op->GetTag(ID_MOTAGDATA);
	if (!tag)
		return false;
	cinema::GetMoDataMessage message;
	if (!tag->Message(MSG_GET_MODATA, &message) || !message.modata)
		return false;

	cinema::MoData* const data = message.modata;
	const maxon::Int count = data->GetCount();
	const cinema::MDArray<cinema::Vector> colorArray = data->GetVectorArray(cinema::MODATA_COLOR);
	const cinema::MDArray<cinema::Matrix> matrixArray = data->GetMatrixArray(cinema::MODATA_MATRIX);
	const cinema::MDArray<maxon::Int32> flagArray = data->GetLongArray(cinema::MODATA_FLAGS);
	for (maxon::Int i = 0; i < count; ++i)
	{
		const maxon::Int32 flags = flagArray[i];
		if (!(flags & MOGENFLAG_CLONE_ON) || (flags & MOGENFLAG_DISABLE))
			continue;
		colors.Append(colorArray[i]) iferr_return;
		offsets.Append(matrixArray[i].off) iferr_return;
	}
	return !colors.IsEmpty();
}

// The frame is rendered by an Eevee that shows no light to the camera (ExportLight).
static maxon::Bool EeveeHidesLights(const ExportSettings& settings)
{
	return settings.eevee && settings.blenderVersion != 0 && settings.blenderVersion < EEVEE_LIGHT_VISIBLE_VERSION;
}

maxon::Result<maxon::Bool> SceneExporter::WriteLightAlone(SceneWriter& writer, cinema::BaseDocument* doc, const ExportSettings& settings,
	const cinema::BaseObject* light, const maxon::String& id, const LightLinking* linking)
{
	if (!doc || !light)
		return maxon::NullptrError(MAXON_SOURCE_LOCATION);

	// Its place as the walk forms it, so that the numbers agree to the last bit.
	const DocumentColors colors(doc);
	return ExportLight(writer, Coords::FromDocument(doc), colors, light, WalkMatrix(light), id, settings.lightMultiplier, linking, EeveeHidesLights(settings));
}

maxon::Result<maxon::Bool> SceneExporter::WriteWorldAlone(SceneWriter& writer, cinema::BaseDocument* doc, const ExportSettings& settings,
	const cinema::BaseObject* const environments[3], const WrittenWorld& written)
{
	iferr_scope;
	if (!doc)
		return maxon::NullptrError(MAXON_SOURCE_LOCATION);
	WorldPart parts[3];
	for (maxon::Int role = 0; role < 3; ++role)
	{
		if (!written.objects[role])
			continue;
		// Gone, or asking for other roles than then (switched off, a box ticked or not): which environment
		// fills which role is the whole export's to say.
		if (!environments[role] || GetEnvironmentRoles(environments[role]) != written.roles[role])
			return false;
		parts[role].kind = WORLDPART::ENVIRONMENT;
		parts[role].object = environments[role];
		parts[role].mg = WalkMatrix(environments[role]);
	}
	MaterialExporter materials(writer, doc, settings.bakeResolution);
	materials.SetBlenderVersion(settings.blenderVersion, settings.blenderVersionText);
	maxon::String read;
	ExportWorldItem(materials, parts[0], parts[1], parts[2], settings.worldColor, settings.worldStrength, &read) iferr_return;
	// Another image read (another type, file or colour space): what Blender holds of the old one is to go.
	return read == written.images;
}

cinema::Matrix SceneExporter::WalkMatrix(const cinema::BaseObject* op)
{
	// The same product, in the same order, as Collect() and Walk() form on the way down: Collect() starts
	// each top object with its GetMl(), and Walk() hands every child `mg * child->GetMl()`.
	const cinema::BaseObject* const up = op->GetUp();
	return up ? WalkMatrix(up) * op->GetMl() : op->GetMl();
}

SceneExporter::SceneExporter(cinema::BaseDocument* doc, const ExportSettings& settings)
	: _doc(doc), _colors(doc), _settings(settings), _coords(Coords::FromDocument(doc))
{
}

SceneExporter::~SceneExporter()
{
	if (_ocio)
		DeleteObj(_ocio);
	for (maxon::BaseArray<cinema::Matrix>*& matrices : _ownedMatrices)
		DeleteObj(matrices);
	for (maxon::BaseArray<maxon::Color64>*& colors : _ownedColors)
		DeleteObj(colors);
}

maxon::Result<void> SceneExporter::Collect()
{
	iferr_scope;

	if (!_doc)
		return maxon::NullptrError(MAXON_SOURCE_LOCATION);

	for (cinema::BaseObject* op = _doc->GetFirstObject(); op; op = op->GetNext())
	{
		Walk(op, op->GetMl(), Inherited(), maxon::String(), false, ObjectKey(op), false) iferr_return;
	}
	_camera = _settings.camera ? _settings.camera : FindRenderCamera(_doc);
	return maxon::OK;
}

maxon::Result<void> SceneExporter::CollectOwners(const maxon::BaseArray<const cinema::BaseObject*>& owners)
{
	iferr_scope;

	if (!_doc || !_settings.materials)
		return maxon::NullptrError(MAXON_SOURCE_LOCATION);
	_partial = true;
	for (const cinema::BaseObject* const owner : owners)
	{
		_only.Insert(owner) iferr_return;
		for (const cinema::BaseObject* up = owner->GetUp(); up; up = up->GetUp())
		{
			_onlyAbove.Insert(up) iferr_return;
		}
	}
	// The same walk from the top, so that every object gets what it inherits, and its matrix as the
	// product a whole export forms; Walk() leaves out what the other objects show.
	for (cinema::BaseObject* op = _doc->GetFirstObject(); op; op = op->GetNext())
	{
		if (!_only.Contains(op) && !_onlyAbove.Contains(op))
			continue;
		Walk(op, op->GetMl(), Inherited(), maxon::String(), false, ObjectKey(op), false) iferr_return;
	}
	return maxon::OK;
}

maxon::Result<void> SceneExporter::GetMotionSample(MotionSample& sample) const
{
	iferr_scope;

	for (const Leaf& leaf : _leaves)
	{
		// A fog's box only moves: its matrix is all it needs.
		if (leaf.kind == LEAF::FOG)
		{
			sample.matrices.Insert(leaf.key, leaf.mg) iferr_return;
			continue;
		}
		if (leaf.kind != LEAF::MESH)
			continue;
		sample.matrices.Insert(leaf.key, leaf.mg) iferr_return;
		const cinema::PolygonObject* const mesh = static_cast<const cinema::PolygonObject*>(leaf.object);
		const cinema::Vector* const points = mesh->GetPointR();
		const maxon::Int32 count = mesh->GetPointCount();
		if (!points || count <= 0)
			continue;
		maxon::BaseArray<cinema::Vector32> copy;
		copy.Resize(count) iferr_return;
		for (maxon::Int32 i = 0; i < count; ++i)
			copy[i] = cinema::Vector32(points[i]);
		sample.points.Insert(leaf.key, std::move(copy)) iferr_return;
	}
	if (_camera)
	{
		sample.hasCamera = true;
		sample.camera = CameraValues::Read(_camera).mg;	// the view's axes, as ExportCamera uses them
	}
	return maxon::OK;
}

maxon::Result<maxon::Int> SceneExporter::ExtendStack(maxon::Int parent, cinema::BaseObject* op)
{
	iferr_scope;

	TextureStack own;
	for (cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
	{
		if (!tag->IsInstanceOf(Ttexture))
			continue;
		cinema::BaseMaterial* const material = static_cast<cinema::TextureTag*>(tag)->GetMaterial();
		if (!material)
			continue;
		TextureEntry entry;
		entry.material = material;
		entry.restriction = GetStringParameter(tag, TEXTURETAG_RESTRICTION);
		entry.tag = tag;
		own.Append(std::move(entry)) iferr_return;
	}
	if (own.IsEmpty())
		return parent;

	TextureStack stack;
	if (parent != NOTOK)
	{
		stack.CopyFrom(_stacks[parent]) iferr_return;
	}
	stack.AppendAll(own) iferr_return;
	_stacks.Append(std::move(stack)) iferr_return;
	return _stacks.GetCount() - 1;
}

cinema::BaseMaterial* SceneExporter::GetStackMaterial(maxon::Int stack) const
{
	if (stack == NOTOK)
		return nullptr;
	// The last tag that is not restricted to a selection covers the whole object.
	const TextureStack& entries = _stacks[stack];
	for (maxon::Int i = entries.GetCount() - 1; i >= 0; --i)
	{
		if (entries[i].restriction.IsEmpty())
			return entries[i].material;
	}
	return nullptr;
}

maxon::Result<void> SceneExporter::AddMeshLeaf(cinema::BaseObject* object, const cinema::Matrix& mg, const Inherited& inherited, const maxon::String& path,
	maxon::UInt64 key, maxon::Bool generated)
{
	iferr_scope;

	Leaf leaf;
	leaf.kind = LEAF::MESH;
	leaf.object = object;
	leaf.mg = mg;
	leaf.stack = inherited.stack;
	leaf.path = path;
	leaf.key = key;
	leaf.owner = inherited.owner;
	leaf.generated = generated;
	leaf.deformed = inherited.deformed;
	leaf.hasColor = inherited.hasColor;
	leaf.color = inherited.color;
	leaf.instanceSet = inherited.instanceSet;
	leaf.ownerObject = inherited.ownerObject;
	leaf.tail = inherited.tail;
	leaf.followable = inherited.followable;
	leaf.linkTag = inherited.linkTag;
	leaf.visibility = inherited.visibility;
	leaf.nameKey = NameKey(inherited.keyPath);
	leaf.objectId = inherited.objectId;
	_leaves.Append(std::move(leaf)) iferr_return;
	if (_settings.noteFollowed && inherited.deformed && inherited.ownerObject &&
		(_deformedOwners.IsEmpty() || _deformedOwners[_deformedOwners.GetCount() - 1] != inherited.ownerObject))
	{
		_deformedOwners.Append(inherited.ownerObject) iferr_return;
	}
	return maxon::OK;
}

maxon::Result<maxon::Int> SceneExporter::ExtendTail(maxon::Int parent, const cinema::Matrix& ml)
{
	iferr_scope;
	if (!_settings.noteFollowed)
		return maxon::Int(NOTOK);
	TailNode node;
	node.parent = parent;
	node.ml = ml;
	_tails.Append(node) iferr_return;
	return _tails.GetCount() - 1;
}

maxon::Result<void> SceneExporter::NoteUnfollowed(const cinema::BaseObject* owner)
{
	iferr_scope;
	// The leaves of one owner follow each other, so looking at the last entry keeps the list short.
	if (!_settings.noteFollowed || !owner || (!_unfollowedOwners.IsEmpty() && _unfollowedOwners[_unfollowedOwners.GetCount() - 1] == owner))
		return maxon::OK;
	_unfollowedOwners.Append(owner) iferr_return;
	return maxon::OK;
}

maxon::Result<void> SceneExporter::NoteFixed(const cinema::BaseObject* owner)
{
	iferr_scope;
	NoteUnfollowed(owner) iferr_return;
	if (!_settings.noteFollowed || !owner || (!_fixedOwners.IsEmpty() && _fixedOwners[_fixedOwners.GetCount() - 1] == owner))
		return maxon::OK;
	_fixedOwners.Append(owner) iferr_return;
	return maxon::OK;
}

maxon::Result<void> SceneExporter::NoteOwned(const cinema::BaseObject* owner, const maxon::String& id)
{
	iferr_scope;
	if (!_settings.noteFollowed || !owner)
		return maxon::OK;
	OwnedItem item;
	item.owner = owner;
	item.id = id;
	_ownedItems.Append(std::move(item)) iferr_return;
	return maxon::OK;
}

maxon::Result<void> SceneExporter::NoteReference(const cinema::BaseObject* referrer, const cinema::BaseObject* root)
{
	iferr_scope;
	if (!_settings.noteFollowed || !referrer || !root)
		return maxon::OK;
	ObjectReference reference;
	reference.referrer = referrer;
	reference.root = root;
	_references.Append(reference) iferr_return;
	return maxon::OK;
}

// The rotation a unit quaternion stands for, as the axes of a matrix.
static void SetRotation(cinema::Matrix& m, const maxon::Quaternion<maxon::Float32>& rotation)
{
	const maxon::Float64 w = rotation.w;
	const maxon::Float64 x = rotation.v.x;
	const maxon::Float64 y = rotation.v.y;
	const maxon::Float64 z = rotation.v.z;
	m.sqmat.v1 = cinema::Vector(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + z * w), 2.0 * (x * z - y * w));
	m.sqmat.v2 = cinema::Vector(2.0 * (x * y - z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + x * w));
	m.sqmat.v3 = cinema::Vector(2.0 * (x * z + y * w), 2.0 * (y * z - x * w), 1.0 - 2.0 * (x * x + y * y));
}

maxon::Result<maxon::Bool> SceneExporter::WalkTagInstances(cinema::BaseObject* op, const cinema::Matrix& mg, const Inherited& inherited,
	const maxon::String& path, maxon::UInt64 key)
{
	iferr_scope;

	const cinema::BaseTag* tag = nullptr;
	for (const cinema::BaseTag* candidate = op->GetFirstTag(); candidate && !tag; candidate = candidate->GetNext())
	{
		if (candidate->GetType() == ID_OBJECT_TAG && candidate->GetDataInstanceRef().GetBool(CYCLESOBJECT_INSTANCES_ENABLE))
			tag = candidate;
	}
	if (!tag)
		return false;

	maxon::BaseArray<cinema::BaseObject*> objects;
	const cinema::GeData listData = GetParameterData(tag, CYCLESOBJECT_INSTANCES_OBJECTS);
	if (const cinema::InExcludeData* const list = listData.GetCustomDataType<cinema::InExcludeData>())
	{
		for (maxon::Int32 i = 0; i < list->GetObjectCount(); ++i)
		{
			cinema::BaseList2D* const entry = list->ObjectFromIndex(_doc, i);
			if (entry && entry->IsInstanceOf(Obase) && entry != op)
			{
				objects.Append(static_cast<cinema::BaseObject*>(entry)) iferr_return;
			}
		}
	}
	if (objects.IsEmpty())
	{
		_notes.Append(FormatString("Object \"@\": its Cycles Object tag instances objects, but lists none; it renders as without the tag.", op->GetName())) iferr_return;
		return false;
	}
	if (inherited.instanceDepth >= MAX_INSTANCE_DEPTH)
		return true;

	// Where the copies go: one matrix per particle or per matrix, in the object's own axes.
	const maxon::Float64 scale = GetFloatParameter(tag, CYCLESOBJECT_INSTANCES_SCALE, 1.0);
	maxon::BaseArray<cinema::Matrix> placements;
	if (op->GetType() == Ofpgroup)
	{
		const cinema::ParticleGroupObject* const group = static_cast<const cinema::ParticleGroupObject*>(op);
		const maxon::Block<const cinema::Vector> positions = group->GetParticlePositionsR();
		const maxon::Block<const maxon::Quaternion<maxon::Float32>> alignments = group->GetParticleAlignmentsR();
		const maxon::Block<const maxon::Float32> radii = group->GetParticleRadiiR();
		const maxon::Bool useRadius = GetBoolParameter(tag, CYCLESOBJECT_INSTANCES_USE_RADIUS, false) && radii.GetCount() == positions.GetCount();
		placements.EnsureCapacity(positions.GetCount()) iferr_return;
		for (maxon::Int i = 0; i < positions.GetCount(); ++i)
		{
			cinema::Matrix m;
			if (alignments.GetCount() == positions.GetCount())
				SetRotation(m, alignments[i]);
			m.sqmat = m.sqmat * (scale * (useRadius ? maxon::Float64(radii[i]) : 1.0));
			m.off = positions[i];
			placements.Append(m) iferr_return;
		}
	}
	else
	{
		cinema::BaseTag* const moTag = op->GetTag(ID_MOTAGDATA);
		cinema::GetMoDataMessage message;
		if (moTag && moTag->Message(MSG_GET_MODATA, &message) && message.modata)
		{
			cinema::MoData* const data = message.modata;
			const cinema::MDArray<cinema::Matrix> matrixArray = data->GetMatrixArray(cinema::MODATA_MATRIX);
			const cinema::MDArray<maxon::Int32> flagArray = data->GetLongArray(cinema::MODATA_FLAGS);
			for (maxon::Int i = 0; i < data->GetCount(); ++i)
			{
				const maxon::Int32 flags = flagArray[i];
				if (!(flags & MOGENFLAG_CLONE_ON) || (flags & MOGENFLAG_DISABLE))
					continue;
				cinema::Matrix m = matrixArray[i];
				m.sqmat = m.sqmat * scale;
				placements.Append(m) iferr_return;
			}
		}
	}
	if (placements.IsEmpty())
		return true;

	// The listed objects take turns, as the children of a Cloner do: copy i shows object i modulo their number.
	for (maxon::Int k = 0; k < objects.GetCount(); ++k)
	{
		maxon::BaseArray<cinema::Matrix>* matrices = NewObj(maxon::BaseArray<cinema::Matrix>) iferr_return;
		iferr (_ownedMatrices.Append(matrices))
		{
			DeleteObj(matrices);
			return err;
		}
		for (maxon::Int i = k; i < placements.GetCount(); i += objects.GetCount())
		{
			matrices->Append(mg * placements[i]) iferr_return;
		}
		if (matrices->IsEmpty())
			continue;

		InstanceSet set;
		set.matrices = matrices;
		_instanceSets.Append(set) iferr_return;
		Inherited forObject = inherited;
		forObject.instanceSet = _instanceSets.GetCount() - 1;
		forObject.followable = false;
		NoteReference(inherited.ownerObject, objects[k]) iferr_return;
		forObject.hasColor = false;
		forObject.clones = nullptr;
		forObject.forceVisible = true;   // listed objects are usually hidden themselves
		++forObject.instanceDepth;
		// The object is walked once, with its axis at each copy's place.
		Walk(objects[k], cinema::Matrix(), forObject, path, true, MixKeys(MixKeys(key, objects[k]->GetGUID()), maxon::UInt64(k + 1)), true) iferr_return;
	}
	return true;
}

maxon::Result<void> SceneExporter::WalkScatter(cinema::BaseObject* op, const cinema::Matrix& mg, const Inherited& inherited, const maxon::String& path,
	maxon::UInt64 key, maxon::Bool inCache)
{
	iferr_scope;

	const maxon::String name = op->GetName();
	// Its copies follow from other objects (the surface, the effectors, the fields) that a step of the IPR does not
	// look at: only a whole export writes them right.
	NoteFixed(inherited.ownerObject) iferr_return;
	if (inherited.instanceSet != NOTOK)
	{
		_notes.Append(FormatString("Scatter \"@\": a Cycles Scatter inside a multi-instance is not exported.", name)) iferr_return;
		return maxon::OK;
	}
	const ScatterResult* const result = GetScatterResult(op);
	if (!result)
	{
		_notes.Append(FormatString(inCache ? "Scatter \"@\": a copy of a Cycles Scatter made by a generator is not exported."_s
			: "Scatter \"@\": its copies were not computed (the document was not evaluated); it renders nothing."_s, name)) iferr_return;
		return maxon::OK;
	}
	// The copies as the scene is now (a surface below the scatter in the Object Manager reaches it one evaluation late).
	ScatterCopies fresh;
	maxon::BaseArray<maxon::String> freshWarnings;
	const ScatterCopies* const current = GetCurrentScatterCopies(op, _doc, fresh, freshWarnings) iferr_return;
	for (const maxon::String& warning : current == &fresh ? freshWarnings : result->warnings)
	{
		_notes.Append(FormatString("Scatter \"@\": @", name, warning)) iferr_return;
	}
	for (const maxon::String& skipped : result->skipped)
	{
		_notes.Append(FormatString("Scatter \"@\": \"@\" below it is a light or a camera; it is not scattered.", name, skipped)) iferr_return;
	}
	if (inherited.instanceDepth >= MAX_INSTANCE_DEPTH)
		return maxon::OK;

	// Each source walked once, its axis at each copy that shows it; the copies' MoGraph colours as a multi-instance's.
	const ScatterCopies& copies = current ? *current : result->copies;
	for (maxon::Int k = 0; k < result->sources.GetCount(); ++k)
	{
		const ScatterSource& source = result->sources[k];
		maxon::BaseArray<cinema::Matrix>* matrices = NewObj(maxon::BaseArray<cinema::Matrix>) iferr_return;
		iferr (_ownedMatrices.Append(matrices))
		{
			DeleteObj(matrices);
			return err;
		}
		maxon::BaseArray<maxon::Color64>* colors = NewObj(maxon::BaseArray<maxon::Color64>) iferr_return;
		iferr (_ownedColors.Append(colors))
		{
			DeleteObj(colors);
			return err;
		}
		for (maxon::Int i = 0; i < copies.matrices.GetCount(); ++i)
		{
			if (copies.sources[i] != k)
				continue;
			matrices->Append(mg * copies.matrices[i]) iferr_return;
			const cinema::Vector& c = copies.colors[i];
			colors->Append(maxon::Color64(c.x, c.y, c.z)) iferr_return;
		}
		if (matrices->IsEmpty())
			continue;

		InstanceSet set;
		set.matrices = matrices;
		set.colors = colors;
		_instanceSets.Append(set) iferr_return;
		Inherited forSource = inherited;
		forSource.instanceSet = _instanceSets.GetCount() - 1;
		forSource.followable = false;
		forSource.hasColor = false;
		forSource.clones = nullptr;
		forSource.forceVisible = false;
		++forSource.instanceDepth;
		// A change below the child in the document makes the scatter's copies anew as well.
		for (const cinema::BaseObject* child = op->GetDown(); child; child = child->GetNext())
		{
			if (child->GetGUID() == source.guid)
			{
				NoteReference(inherited.ownerObject, child) iferr_return;
				break;
			}
		}
		// The clone is the scatter's, not the document's; it is keyed by the child's GUID, so its items keep
		// their ids when the clone is made anew.
		Walk(const_cast<cinema::BaseObject*>(source.clone), cinema::Matrix(), forSource, path, true, MixKeys(MixKeys(key, source.guid), maxon::UInt64(k + 1)), true)
			iferr_return;
	}
	return maxon::OK;
}

maxon::Result<void> SceneExporter::Walk(cinema::BaseObject* op, const cinema::Matrix& mg, Inherited inherited, const maxon::String& parentPath,
	maxon::Bool instanceRoot, maxon::UInt64 key, maxon::Bool inCache)
{
	iferr_scope;

	const maxon::String path = parentPath.IsEmpty() ? maxon::String(op->GetName()) : FormatString("@/@", parentPath, op->GetName());

	// The keys of Redshift's random nodes (Leaf::nameKey): on entry keyPath is the parent's.
	inherited.keyPath = JoinPath(inherited.keyPath, op->GetName());
	if (const cinema::BaseTag* const rsObject = op->GetTag(REDSHIFT_OBJECT_TAG))
	{
		if (GetBoolParameter(rsObject, REDSHIFT_OBJECT_OBJECTID_OVERRIDE, false))
			inherited.objectId = GetIntParameter(rsObject, REDSHIFT_OBJECT_OBJECTID_ID, 0);
	}

	// Only the object this was set for hands the clone colours to its children.
	const CloneColors* const clones = inherited.clones;
	inherited.clones = nullptr;
	if (!inCache)
	{
		inherited.owner = key;
		inherited.ownerObject = op;
	}

	// CollectOwners(): an object of the document that was not asked for is only passed through, for what
	// the objects below it inherit from it.
	const maxon::Bool passing = _partial && !inCache && !_only.Contains(op);

	const maxon::Int32 mode = op->GetRenderMode();
	if (inherited.forceVisible)
		inherited.visible = true;
	else if (mode == MODE_ON)
		inherited.visible = true;
	else if (mode == MODE_OFF)
		inherited.visible = false;
	inherited.forceVisible = false;
	const cinema::LayerData* const layer = op->GetLayerData(_doc);
	const maxon::Bool layerVisible = inherited.layerVisible && (!layer || layer->render);

	inherited.stack = ExtendStack(inherited.stack, op) iferr_return;

	// A Compositing tag counts for its object and everything below, until another one.
	if (const cinema::BaseTag* const compositing = op->GetTag(Tcompositing))
	{
		const maxon::Bool rays = GetBoolParameter(compositing, COMPOSITINGTAG_SEENBYRAYS, true);
		Visibility& visibility = inherited.visibility;
		visibility.camera = GetBoolParameter(compositing, COMPOSITINGTAG_SEENBYCAMERA, true);
		visibility.shadow = GetBoolParameter(compositing, COMPOSITINGTAG_CASTSHADOW, true);
		visibility.diffuse = GetBoolParameter(compositing, COMPOSITINGTAG_SEENBYGI, true);
		visibility.glossy = rays && GetBoolParameter(compositing, COMPOSITINGTAG_SEENBYREFLECTION, true);
		visibility.transmission = rays && GetBoolParameter(compositing, COMPOSITINGTAG_SEENBYREFRACTION, true) &&
			GetBoolParameter(compositing, COMPOSITINGTAG_SEENBYTRANSPARENCY, true);
		if (!GetBoolParameter(compositing, COMPOSITINGTAG_RECEIVESHADOW, true))
		{
			_notes.Append(FormatString("Object \"@\": \"Receive Shadows\" off has no counterpart in Cycles and is ignored.", op->GetName())) iferr_return;
		}
		// The Object Buffers: Blender has one pass index per object, shown by the Object Index pass.
		// The first enabled buffer's id becomes it; further ones are named (one mask per buffer, as
		// the Standard renderer makes, is no pass Blender has).
		static const cinema::Int32 BUFFER_ENABLE[12] = { COMPOSITINGTAG_ENABLECHN0, COMPOSITINGTAG_ENABLECHN1, COMPOSITINGTAG_ENABLECHN2,
			COMPOSITINGTAG_ENABLECHN3, COMPOSITINGTAG_ENABLECHN4, COMPOSITINGTAG_ENABLECHN5, COMPOSITINGTAG_ENABLECHN6, COMPOSITINGTAG_ENABLECHN7,
			COMPOSITINGTAG_ENABLECHN8, COMPOSITINGTAG_ENABLECHN9, COMPOSITINGTAG_ENABLECHN10, COMPOSITINGTAG_ENABLECHN11 };
		static const cinema::Int32 BUFFER_ID[12] = { COMPOSITINGTAG_IDCHN0, COMPOSITINGTAG_IDCHN1, COMPOSITINGTAG_IDCHN2, COMPOSITINGTAG_IDCHN3,
			COMPOSITINGTAG_IDCHN4, COMPOSITINGTAG_IDCHN5, COMPOSITINGTAG_IDCHN6, COMPOSITINGTAG_IDCHN7, COMPOSITINGTAG_IDCHN8, COMPOSITINGTAG_IDCHN9,
			COMPOSITINGTAG_IDCHN10, COMPOSITINGTAG_IDCHN11 };
		maxon::Int enabled = 0;
		inherited.visibility.passIndex = 0;
		for (maxon::Int i = 0; i < 12; ++i)
		{
			if (!GetBoolParameter(compositing, BUFFER_ENABLE[i], false))
				continue;
			if (enabled++ == 0)
				inherited.visibility.passIndex = maxon::Max(maxon::Int32(0), GetIntParameter(compositing, BUFFER_ID[i], 0));
		}
		if (enabled > 1)
		{
			_notes.Append(FormatString("Object \"@\": only the first of its @ Object Buffers becomes Blender's pass index (Object Index pass, @).",
				op->GetName(), enabled, inherited.visibility.passIndex)) iferr_return;
		}
	}

	// A Cycles Object tag overrides, tab by tab, for its object and everything below.
	for (const cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
	{
		if (tag->GetType() != ID_OBJECT_TAG)
			continue;
		if (GetBoolParameter(tag, CYCLESOBJECT_VISIBILITY_ENABLE, false))
		{
			Visibility& visibility = inherited.visibility;
			visibility.camera = GetBoolParameter(tag, CYCLESOBJECT_VISIBILITY_CAMERA, true);
			visibility.shadow = GetBoolParameter(tag, CYCLESOBJECT_VISIBILITY_SHADOW, true);
			visibility.diffuse = GetBoolParameter(tag, CYCLESOBJECT_VISIBILITY_DIFFUSE, true);
			visibility.glossy = GetBoolParameter(tag, CYCLESOBJECT_VISIBILITY_GLOSSY, true);
			visibility.transmission = GetBoolParameter(tag, CYCLESOBJECT_VISIBILITY_TRANSMISSION, true);
			visibility.volumeScatter = GetBoolParameter(tag, CYCLESOBJECT_VISIBILITY_VOLUME_SCATTER, true);
			visibility.holdout = GetBoolParameter(tag, CYCLESOBJECT_VISIBILITY_HOLDOUT, false);
			visibility.shadowCatcher = GetBoolParameter(tag, CYCLESOBJECT_VISIBILITY_SHADOW_CATCHER, false);
		}
		if (GetBoolParameter(tag, CYCLESOBJECT_LINKING_ENABLE, false))
			inherited.linkTag = tag;
	}
	// A Cycles Light tag works only on one of Cinema 4D's lights (lights/light_tag.h).
	if (FindLightTag(op) && !IsTaggableLight(op))
	{
		_notes.Append(FormatString(op->GetType() == ID_LIGHT_OBJECT ? "Object \"@\": a Cycles Light tag on a Cycles Light does nothing; the light has these settings itself."_s
			: "Object \"@\": a Cycles Light tag works on Cinema 4D's and Redshift's lights only; here it does nothing."_s, op->GetName())) iferr_return;
	}

	// The object a render instance refers to is drawn where the instance is, whatever hides the
	// original's parents and even when the original is itself the input of a generator. Its own
	// render dot still counts: Cinema 4D renders no instance of an object switched off that way.
	const maxon::Bool consumed = !instanceRoot && !inherited.sphereCache && op->GetBit(BIT_CONTROLOBJECT);
	const maxon::Bool renders = inherited.visible && layerVisible && !consumed;
	const maxon::Int32 type = op->GetType();
	// What an Instance object shows, in any of its modes, also follows the objects below the one it
	// refers to (GetReferenceRoots).
	if (_settings.noteFollowed && type == Oinstance && !passing)
	{
		NoteReference(inherited.ownerObject, static_cast<const cinema::InstanceObject*>(op)->GetReferenceObject(_doc)) iferr_return;
	}

	// What an object generates lives in its cache; a deformed copy, if any, replaces that.
	cinema::BaseObject* cache = nullptr;
	maxon::Bool cacheIsDeformed = false;

	// A particle group or Matrix object whose Cycles Object tag instances objects on it.
	maxon::Bool instanced = false;
	// (A Matrix object carries the "used by a generator" mark itself, as a Hair object does.)
	if (!passing && (type == Ofpgroup || type == Omgmatrix) && (type == Omgmatrix ? (inherited.visible && layerVisible) : renders) && inherited.instanceSet == NOTOK)
	{
		instanced = WalkTagInstances(op, mg, inherited, path, key) iferr_return;
	}

	if (passing)
	{
		// Only what the objects below inherit.
		if (type == Ovolumeset)
			inherited.volumeSet = op;
	}
	else if (instanced)
	{
		// Nothing else of it renders: the particles themselves are replaced by the objects.
	}
	else if (type == Olight || type == Orslight || type == ID_LIGHT_OBJECT)
	{
		// A Redshift light carries the "used by a generator" mark itself, like a Hair object. A light
		// whose green check is off renders nothing (a dome light included), and its check does not
		// reach the lights below it (measured with the Standard renderer and Redshift).
		const maxon::Bool shines = (type == Orslight ? (inherited.visible && layerVisible) : renders) && IsLightSwitchedOn(op);
		if (shines && type == Orslight && GetIntParameter(op, REDSHIFT_LIGHT_TYPE, 0) == REDSHIFT_LIGHT_TYPE_DOME)
		{
			// A dome light is the environment, not a lamp: a candidate for the world, in the walk's order.
			WorldSource source;
			source.kind = WORLDPART::DOME;
			source.roles.camera = source.roles.lighting = true;
			source.object = op;
			source.mg = mg;
			source.owner = inherited.ownerObject;
			_worlds.Append(std::move(source)) iferr_return;
			NoteFixed(inherited.ownerObject) iferr_return;
		}
		else if (shines && inherited.instanceSet == NOTOK)
		{
			Leaf leaf;
			leaf.kind = LEAF::LIGHT;
			leaf.object = op;
			leaf.mg = mg;
			leaf.path = path;
			leaf.key = key;
			leaf.ownerObject = inherited.ownerObject;
			_leaves.Append(std::move(leaf)) iferr_return;
		}
		else if (shines)
		{
			_notes.Append(FormatString("Light \"@\": lights inside a multi-instance are not exported.", op->GetName())) iferr_return;
		}
	}
	else if (type == ID_ENVIRONMENT_OBJECT)
	{
		// The plugin's environment: a candidate for the world, in the walk's order, when it renders and
		// its green check is on. Whether it is noted as fixed is decided in Write(), when the one used is known.
		if (renders && IsEnvironmentSwitchedOn(op))
		{
			WorldSource source;
			source.kind = WORLDPART::ENVIRONMENT;
			source.roles = GetEnvironmentRoles(op);
			source.object = op;
			source.mg = mg;
			source.owner = inherited.ownerObject;
			source.own = !inCache && !instanceRoot && key == ObjectKey(op) && inherited.followable;
			_worlds.Append(std::move(source)) iferr_return;
		}
	}
	else if (type == ID_FOG_OBJECT)
	{
		// The plugin's fog (objects/fog_object.h): no cache, a box of its own. Its green check counts
		// for it alone, as a light's does; its children are walked below as any.
		if (renders && IsFogSwitchedOn(op) && inherited.instanceSet != NOTOK)
		{
			_notes.Append(FormatString("Fog \"@\": a Cycles Fog inside a multi-instance is not exported.", op->GetName())) iferr_return;
		}
		else if (renders && IsFogSwitchedOn(op))
		{
			Leaf leaf;
			leaf.kind = LEAF::FOG;
			leaf.object = op;
			leaf.mg = mg;
			leaf.path = path;
			leaf.key = key;
			leaf.owner = inherited.owner;
			leaf.generated = inCache;
			leaf.ownerObject = inherited.ownerObject;
			leaf.tail = inherited.tail;
			leaf.followable = inherited.followable;
			leaf.linkTag = inherited.linkTag;
			leaf.visibility = inherited.visibility;
			_leaves.Append(std::move(leaf)) iferr_return;
		}
		// What the fog does not take from its own tags.
		for (const cinema::BaseTag* tag = op->GetFirstTag(); tag && renders && !passing; tag = tag->GetNext())
		{
			if (tag->IsInstanceOf(Ttexture))
			{
				_notes.Append(FormatString("Fog \"@\": its material is not used; a Cycles Fog renders with its own volume values.", op->GetName())) iferr_return;
				break;
			}
		}
		for (const cinema::BaseTag* tag = op->GetFirstTag(); tag && renders && !passing; tag = tag->GetNext())
		{
			if (tag->GetType() == ID_OBJECT_TAG && GetBoolParameter(tag, CYCLESOBJECT_VOLUME_ENABLE, false))
			{
				_notes.Append(FormatString("Fog \"@\": the Volume tab of its Cycles Object tag does nothing; the fog has these values itself.", op->GetName())) iferr_return;
				break;
			}
		}
	}
	else if (type == ID_PROXY_OBJECT)
	{
		// The plugin's proxy (objects/proxy_object.h): no cache; Blender shows the file it names at its place.
		// Its green check counts for it alone, as a fog's; its children are walked below as any. Inside a
		// multi-instance (and below a Cycles Scatter) it is shown once per copy.
		if (renders && IsProxySwitchedOn(op))
		{
			Leaf leaf;
			leaf.kind = LEAF::PROXY;
			leaf.object = op;
			leaf.mg = mg;
			leaf.path = path;
			leaf.key = key;
			leaf.owner = inherited.owner;
			leaf.generated = inCache;
			leaf.instanceSet = inherited.instanceSet;
			leaf.ownerObject = inherited.ownerObject;
			leaf.tail = inherited.tail;
			leaf.followable = inherited.followable;
			leaf.linkTag = inherited.linkTag;
			leaf.visibility = inherited.visibility;
			_leaves.Append(std::move(leaf)) iferr_return;
			// What the proxy does not take from its own tags.
			for (const cinema::BaseTag* tag = op->GetFirstTag(); tag && !passing; tag = tag->GetNext())
			{
				if (tag->IsInstanceOf(Ttexture))
				{
					_notes.Append(FormatString("Proxy \"@\": its material is not used; what a Cycles Proxy shows renders with the materials of its Blender file.", op->GetName())) iferr_return;
					break;
				}
			}
		}
	}
	else if (type == ID_SCATTER_OBJECT && IsScatterSwitchedOn(op))
	{
		// The plugin's scatter (objects/scatter_object.h): its children are its sources, shown at its copies. Like
		// every generator with a cache it carries the "used by a generator" mark itself, so that does not hide it.
		if (inherited.visible && layerVisible)
		{
			WalkScatter(op, mg, inherited, path, key, inCache) iferr_return;
		}
	}
	else if (type == Ovolume)
	{
		if (renders && inherited.instanceSet == NOTOK)
		{
			Leaf leaf;
			leaf.kind = LEAF::VOLUME;
			leaf.object = op;
			// A volume object's own matrix is its grid's transform (voxel size and position), which
			// a .vdb file holds as well: what counts is where the object's parent is (volume_export.h).
			leaf.mg = mg * ~op->GetMl();
			leaf.path = path;
			leaf.key = key;
			leaf.owner = inherited.owner;
			leaf.ownerObject = inherited.ownerObject;
			leaf.volumeSet = inherited.volumeSet;
			_leaves.Append(std::move(leaf)) iferr_return;
		}
	}
	else if (type == Ohair || type == Ofpgroup)
	{
		// Hairs are generated when they are written; what the Hair object holds in its cache
		// (polygons, if it is set to generate any) is left alone.
		// A Hair object carries the "used by a generator" mark itself (it is input to its own
		// hairs), so that mark does not hide it.
		const maxon::Bool shown = type == Ohair ? (inherited.visible && layerVisible) : renders;
		if (shown && inherited.instanceSet == NOTOK)
		{
			Leaf leaf;
			leaf.kind = type == Ohair ? LEAF::HAIR : LEAF::PARTICLES;
			leaf.object = op;
			leaf.mg = mg;
			leaf.stack = inherited.stack;
			leaf.path = path;
			leaf.key = key;
			leaf.ownerObject = inherited.ownerObject;
			_leaves.Append(std::move(leaf)) iferr_return;
		}
	}
	else if (type == Osky)
	{
		if (renders)
		{
			WorldSource source;
			source.kind = WORLDPART::SKY;
			source.roles.camera = source.roles.lighting = true;
			source.object = op;
			source.mg = mg;
			source.material = GetStackMaterial(inherited.stack);
			source.owner = inherited.ownerObject;
			_worlds.Append(std::move(source)) iferr_return;
		}
		if (renders)
		{
			NoteFixed(inherited.ownerObject) iferr_return;
		}
	}
	else if (type == Oinstance && GetIntParameter(op, INSTANCEOBJECT_RENDERINSTANCE_MODE, INSTANCEOBJECT_RENDERINSTANCE_MODE_NONE) != INSTANCEOBJECT_RENDERINSTANCE_MODE_NONE)
	{
		// Render instances and multi-instances have no geometry of their own.
		const cinema::InstanceObject* const instance = static_cast<const cinema::InstanceObject*>(op);
		cinema::BaseObject* const reference = const_cast<cinema::BaseObject*>(instance->GetReferenceObject(_doc));
		if (renders && reference && inherited.instanceDepth < MAX_INSTANCE_DEPTH)
		{
			Inherited forReference = inherited;
			forReference.layerVisible = layerVisible;
			forReference.followable = false;
			++forReference.instanceDepth;
			// Redshift varies its random nodes by the object a render instance shows, not by the
			// instance (measured: every render instance of one object, and every clone of a Cloner
			// in render-instance or multi-instance mode, gets one value): the key path is the
			// reference's own. A reference in the same tree as the instance (a Cloner's cache) is
			// found under the same path above that tree as the instance.
			{
				const cinema::BaseObject* rootOfReference = reference;
				while (rootOfReference->GetUp())
					rootOfReference = rootOfReference->GetUp();
				const cinema::BaseObject* rootOfInstance = op;
				while (rootOfInstance->GetUp())
					rootOfInstance = rootOfInstance->GetUp();
				maxon::String parentKey = TreePath(reference->GetUp());
				const maxon::String instanceTree = TreePath(op);
				const maxon::Int above = inherited.keyPath.GetLength() - instanceTree.GetLength();
				if (rootOfReference == rootOfInstance && above > 0 && inherited.keyPath.EndsWith(instanceTree))
					parentKey = JoinPath(inherited.keyPath.GetPart(0, above - 1), parentKey);
				forReference.keyPath = parentKey;
			}
			if (!instance->IsMultiInstance())
			{
				Walk(reference, mg, forReference, path, true, MixKeys(key, reference->GetGUID()), true) iferr_return;
			}
			else if (inherited.instanceSet != NOTOK)
			{
				_notes.Append(FormatString("Instance \"@\": a multi-instance inside a multi-instance is not exported.", op->GetName())) iferr_return;
			}
			else if (instance->GetInstanceMatrices() && !instance->GetInstanceMatrices()->IsEmpty())
			{
				// The reference is walked once, placed relative to a copy; each mesh found in it
				// becomes an instancer with one matrix per copy.
				InstanceSet set;
				set.matrices = instance->GetInstanceMatrices();
				const maxon::BaseArray<maxon::Color64>* const colors = instance->GetInstanceColors();
				if (colors && colors->GetCount() == set.matrices->GetCount())
					set.colors = colors;
				_instanceSets.Append(set) iferr_return;
				forReference.instanceSet = _instanceSets.GetCount() - 1;
				forReference.hasColor = false;
				Walk(reference, cinema::Matrix(), forReference, path, true, MixKeys(key, reference->GetGUID()), true) iferr_return;
			}
		}
	}
	else
	{
		if (type == Ovolumeset)
			inherited.volumeSet = op;
		cache = op->GetDeformCache();
		cacheIsDeformed = cache != nullptr;
		if (!cache)
			cache = op->GetCache();

		if (cache)
		{
			Inherited forCache = inherited;
			forCache.layerVisible = layerVisible;
			forCache.deformed = inherited.deformed || cacheIsDeformed;
			// A "Render Perfect" sphere in a render document prepared by Cinema 4D before 2026.2
			// (see Inherited::sphereCache; measured with 2024.5).
			forCache.sphereCache = type == Osphere && !consumed;
			// MoGraph's colour per clone travels with the clones.
			CloneColors cloneColors;
			if (type == Omgcloner && !cacheIsDeformed)
			{
				const maxon::Bool found = ReadCloneColors(op, cloneColors.colors, cloneColors.offsets) iferr_return;
				if (found)
					forCache.clones = &cloneColors;
			}
			// A deformed copy sits exactly where the object is; a generator's cache has its own
			// matrix relative to the generator.
			if (!cacheIsDeformed && inherited.followable)
			{
				forCache.tail = ExtendTail(inherited.tail, cache->GetMl()) iferr_return;
			}
			Walk(cache, cacheIsDeformed ? mg : mg * cache->GetMl(), forCache, path, false, MixKeys(key, cacheIsDeformed ? 1 : 2), true) iferr_return;
		}
		else if (renders && type == Opolygon)
		{
			AddMeshLeaf(op, mg, inherited, path, key, inCache) iferr_return;
		}
	}

	// A Cycles Scatter's children are its sources, shown only at its copies (WalkScatter), in every mode of the
	// walk: also while it is only passed through.
	if (type == ID_SCATTER_OBJECT && IsScatterSwitchedOn(op))
		return maxon::OK;

	// Children inherit the visibility and material, but not a layer: a layer hides only the
	// objects assigned to it.
	Inherited forChildren = inherited;
	forChildren.layerVisible = inherited.layerVisible;
	forChildren.sphereCache = false;
	maxon::Int childCount = 0;
	if (clones)
	{
		for (const cinema::BaseObject* child = op->GetDown(); child; child = child->GetNext())
			++childCount;
	}
	maxon::UInt64 childIndex = 16;
	maxon::Int position = 0;
	for (cinema::BaseObject* child = op->GetDown(); child; child = child->GetNext(), ++position)
	{
		// CollectOwners(): among the objects of the document only the way down to the ones asked for.
		if (_partial && !inCache && !_only.Contains(child) && !_onlyAbove.Contains(child))
			continue;
		Inherited forChild = forChildren;
		if (clones)
		{
			// The cache lists the clones in MoGraph's order. In multi-instance mode it holds one
			// real clone per kind in a group of its own instead; those are found by position.
			maxon::Int clone = NOTOK;
			if (childCount == clones->colors.GetCount())
			{
				clone = position;
			}
			else if (child->GetType() != Oinstance)
			{
				const cinema::Vector offset = child->GetMl().off;   // the groups in between sit at the generator's origin
				for (maxon::Int i = 0; i < clones->offsets.GetCount() && clone == NOTOK; ++i)
				{
					if ((clones->offsets[i] - offset).GetSquaredLength() < CLONE_MATCH_DISTANCE * CLONE_MATCH_DISTANCE)
						clone = i;
				}
				if (clone == NOTOK && child->GetType() == Onull)
					forChild.clones = clones;   // the group that holds the real clones
			}
			if (clone != NOTOK)
			{
				forChild.hasColor = true;
				forChild.color = clones->colors[clone];
			}
		}
		// An object of the document is the owner of what it shows; inside a cache the child's matrix
		// joins the tail.
		if (inCache && inherited.followable)
		{
			forChild.tail = ExtendTail(inherited.tail, child->GetMl()) iferr_return;
		}
		// (This product, from the top object's GetMl() down, is the one WalkMatrix() forms again.)
		Walk(child, mg * child->GetMl(), forChild, path, false, inCache ? MixKeys(key, childIndex++) : ObjectKey(child), inCache) iferr_return;
	}
	return maxon::OK;
}

//----------------------------------------------------------------------------------------
// What Write() remembers while it goes through the leaves.
//----------------------------------------------------------------------------------------
class SceneExporter::WriteState
{
public:
	WriteState(SceneWriter& w, cinema::BaseDocument* doc, maxon::Int32 bakeResolution) : writer(w), materials(w, doc, bakeResolution) { }

	SceneWriter& writer;
	MaterialExporter materials;
	/// Meshes written so far, by what they were made from (object or content, and the materials' layout).
	maxon::HashMap<maxon::UInt64, maxon::String> meshIds;
	/// The object of the document whose leaf was the first with each of those (it names the mesh).
	maxon::HashMap<maxon::UInt64, const cinema::BaseObject*> meshOwners;
	/// Number of mesh leaves each generator of the document produced.
	maxon::HashMap<maxon::UInt64, maxon::Int> ownerCounts;
};

maxon::Result<void> SceneExporter::Resolve(const Leaf& leaf, WriteState& state, Resolved& resolved)
{
	iferr_scope;

	const cinema::PolygonObject* const mesh = static_cast<const cinema::PolygonObject*>(leaf.object);
	const maxon::Int32 polygonCount = mesh->GetPolygonCount();

	// Which materials each polygon shows: the texture tags in order. A later one covers an
	// earlier one, on the polygons of its selection or everywhere when it has none. If it has
	// an alpha channel, what was there stays below it. Such a pile of tags, bottom first, is a
	// "combination"; each one in use becomes a material slot of the mesh.
	maxon::BaseArray<maxon::BaseArray<maxon::Int32>> combinations;
	combinations.Append() iferr_return;   // 0: no tag at all, the default material
	maxon::BaseArray<maxon::Int32> polygonSlots;
	polygonSlots.Resize(polygonCount) iferr_return;
	for (maxon::Int32& slot : polygonSlots)
		slot = 0;
	const cinema::BaseTag* coveringTag = nullptr;
	const maxon::Int entryCount = leaf.stack != NOTOK ? _stacks[leaf.stack].GetCount() : 0;
	for (maxon::Int32 e = 0; e < entryCount; ++e)
	{
		const TextureEntry& entry = _stacks[leaf.stack][e];
		const cinema::BaseSelect* selection = nullptr;
		if (entry.restriction.IsPopulated())
		{
			for (const cinema::BaseTag* tag = mesh->GetFirstTag(); tag && !selection; tag = tag->GetNext())
			{
				if (tag->GetType() == Tpolygonselection && tag->GetName() == entry.restriction)
					selection = static_cast<const cinema::SelectionTag*>(tag)->GetBaseSelect();
			}
			if (!selection)
				continue;   // a tag naming a selection the object does not have shows nowhere
		}
		else
		{
			coveringTag = entry.tag;
		}

		// What each combination becomes with this tag on top; worked out when first needed.
		const maxon::Bool keepsBelow = MaterialExporter::HasAlpha(entry.material);
		maxon::BaseArray<maxon::Int32> becomes;
		becomes.Resize(combinations.GetCount()) iferr_return;
		for (maxon::Int32& index : becomes)
			index = NOTOK;
		const maxon::Int32 segmentCount = selection ? selection->GetSegments() : 1;
		for (maxon::Int32 segment = 0; segment < segmentCount; ++segment)
		{
			maxon::Int32 from = 0;
			maxon::Int32 to = polygonCount - 1;
			if (selection && !selection->GetRange(segment, polygonCount, &from, &to))
				continue;
			for (maxon::Int32 i = from; i <= to && i < polygonCount; ++i)
			{
				const maxon::Int32 old = polygonSlots[i];
				if (becomes[old] == NOTOK)
				{
					maxon::BaseArray<maxon::Int32> pile;
					if (keepsBelow)
					{
						pile.CopyFrom(combinations[old]) iferr_return;
					}
					pile.Append(e) iferr_return;
					for (maxon::Int32 c = 0; c < combinations.GetCount() && becomes[old] == NOTOK; ++c)
					{
						if (combinations[c].IsEqual(pile))
							becomes[old] = c;
					}
					if (becomes[old] == NOTOK)
					{
						becomes[old] = maxon::Int32(combinations.GetCount());
						combinations.Append(std::move(pile)) iferr_return;
					}
				}
				polygonSlots[i] = becomes[old];
			}
		}
	}

	// Keep only the combinations some polygon uses; they are the slots.
	maxon::BaseArray<maxon::Int32> slotCombinations;
	{
		maxon::BaseArray<maxon::Int32> newIndex;
		newIndex.Resize(combinations.GetCount()) iferr_return;
		for (maxon::Int32& index : newIndex)
			index = NOTOK;
		for (maxon::Int32& slot : polygonSlots)
		{
			if (newIndex[slot] == NOTOK)
			{
				newIndex[slot] = maxon::Int32(slotCombinations.GetCount());
				slotCombinations.Append(slot) iferr_return;
			}
			slot = newIndex[slot];
		}
		if (slotCombinations.IsEmpty())
		{
			slotCombinations.Append(0) iferr_return;
		}
		if (slotCombinations.GetCount() <= 1)
			polygonSlots.Reset();
	}

	MeshMaterials meshMaterials;
	meshMaterials.slotCount = maxon::Int32(slotCombinations.GetCount());
	meshMaterials.polygonSlots = polygonSlots.IsEmpty() ? nullptr : polygonSlots.GetFirst();
	// A texture tag uses the UVW tag that follows it on the object, when there are several.
	if (coveringTag && coveringTag->GetObject() == leaf.object)
	{
		for (const cinema::BaseTag* tag = coveringTag->GetNext(); tag && !meshMaterials.renderUv; tag = tag->GetNext())
		{
			if (tag->IsInstanceOf(Tuvw))
				meshMaterials.renderUv = static_cast<const cinema::UVWTag*>(tag);
		}
	}

	// One polygon object reached several times (render instances), and equal copies inside a
	// generator that makes many, are one mesh. Comparing content costs a pass over the mesh, so
	// it is only done where copies are likely.
	const auto* const ownerCount = state.ownerCounts.Find(leaf.owner);
	const maxon::Bool compareContent = leaf.generated && (leaf.hasColor || leaf.instanceSet != NOTOK || (ownerCount && ownerCount->GetValue() >= BATCH_MINIMUM));
	maxon::UInt64 meshKey = compareContent ? HashMeshSource(mesh) : MixKeys(maxon::UInt64(reinterpret_cast<maxon::UInt>(leaf.object)), 0x6F626A);
	if (meshMaterials.polygonSlots)
		meshKey = HashBytes(reinterpret_cast<const maxon::Char*>(meshMaterials.polygonSlots), maxon::Int(polygonCount) * SIZEOF(maxon::Int32), meshKey);
	if (meshMaterials.renderUv)
	{
		maxon::Int uvIndex = 0;
		for (const cinema::BaseTag* tag = mesh->GetFirstTag(); tag && tag != meshMaterials.renderUv; tag = tag->GetNext())
			uvIndex += tag->IsInstanceOf(Tuvw) ? 1 : 0;
		meshKey = MixKeys(meshKey, maxon::UInt64(uvIndex) + 1);
	}

	if (const auto* const known = state.meshIds.Find(meshKey))
	{
		resolved.meshId = known->GetValue();
		// A mesh shared because two objects of the document made the same content: which of them names it
		// follows from the whole walk, so neither can be written again by itself.
		const auto* const first = state.meshOwners.Find(meshKey);
		if (compareContent && first && first->GetValue() != leaf.ownerObject)
		{
			NoteFixed(first->GetValue()) iferr_return;
			NoteFixed(leaf.ownerObject) iferr_return;
		}
	}
	else
	{
		const maxon::Float64 started = cinema::GeGetMilliSeconds();
		resolved.meshId = KeyId("mesh_", leaf.key);
		MeshMotion meshMotion;
		if (_motionOpen && _motionClose)
		{
			const auto* const open = _motionOpen->points.Find(leaf.key);
			const auto* const close = _motionClose->points.Find(leaf.key);
			if (open && close && open->GetValue().GetCount() == close->GetValue().GetCount())
			{
				meshMotion.open = open->GetValue().GetFirst();
				meshMotion.close = close->GetValue().GetFirst();
				meshMotion.count = maxon::Int32(open->GetValue().GetCount());
			}
		}
		// The IPR: a mesh made from what Blender's copy was made from is left as Blender holds it.
		const maxon::Bool reusable = (_settings.reuse || _settings.noteSources) && !meshMotion.open;
		const maxon::UInt64 signature = reusable ? MeshSignature(leaf, resolved.meshId, meshMaterials, true) : 0;
		maxon::Bool hasGeometry = reusable && _settings.reuse && state.writer.KeepMesh(*_settings.reuse, resolved.meshId, signature);
		if (hasGeometry)
		{
			++_stats.meshesKept;
		}
		else
		{
			hasGeometry = ExportMesh(state.writer, _coords, mesh, resolved.meshId, meshMaterials, meshMotion) iferr_return;
			// Read again: the export asked for the mesh's data, after which Cinema 4D 2027 gives some
			// tags of a generated mesh another data hash (SceneWriter::NoteMeshSource).
			if (hasGeometry && reusable)
				state.writer.NoteMeshSource(resolved.meshId, signature, MeshSignature(leaf, resolved.meshId, meshMaterials, false));
			++_stats.meshesExported;
		}
		if (!hasGeometry)
			resolved.meshId = maxon::String();
		state.meshIds.Insert(meshKey, resolved.meshId) iferr_return;
		state.meshOwners.Insert(meshKey, leaf.ownerObject) iferr_return;
		if (hasGeometry)
		{
			NoteOwned(leaf.ownerObject, resolved.meshId) iferr_return;
			NoteOwned(leaf.ownerObject, resolved.meshId + ".points"_s) iferr_return;
		}
		_stats.meshMilliseconds += cinema::GeGetMilliSeconds() - started;
	}
	if (resolved.meshId.IsEmpty())
		return maxon::OK;

	// The material items must be written before the object that refers to them.
	for (maxon::Int32 combination : slotCombinations)
	{
		maxon::BaseArray<MaterialLayer> layers;
		for (maxon::Int32 e : combinations[combination])
		{
			const TextureEntry& entry = _stacks[leaf.stack][e];
			MaterialLayer layer;
			layer.material = entry.material;
			layer.mapping = TextureMapping::Read(entry.tag);
			layers.Append(layer) iferr_return;
		}
		const maxon::String materialId = state.materials.GetMaterialId(layers) iferr_return;
		resolved.materialIds.Append(materialId) iferr_return;
		resolved.objectKeys = resolved.objectKeys || state.materials.ReadsObjectKeys(materialId);
	}
	return maxon::OK;
}

// The mesh items of a leaf follow from its polygon object (all that ExportMesh reads), the material
// slot of each polygon, the UVW set textures use, the document's unit and the mesh id. For an object
// of the document itself its identity stands for its content: its address, GUID and the dirty counts
// of the object and its tags changed with every edit measured, also without MSG_UPDATE and through
// undo and redo (docs/reference/c4d-sdk.md, "Skipping unchanged meshes in the IPR"). What a generator,
// a deformer or an instance makes can be made anew at the same address with the same counts
// (measured: an instance's copy after a redo), so its content is hashed in full.
//
// An object of the document that is animated, or carries a tag other than plain data, could change
// its points while it is evaluated without anything Cinema 4D records (Point Level Animation, an
// expression such as a Python or XPresso tag, a Pose Morph: none of them tried): it is hashed too.
static maxon::Bool IsIdentityEnough(const cinema::BaseObject* op)
{
	if (op->GetFirstCTrack())
		return false;
	for (const cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
	{
		switch (tag->GetType())
		{
			case Tpoint: case Tpolygon: case Tuvw: case Tnormal: case Tphong: case Tvertexmap: case Tvertexcolor: case Ttangent:
			case Tpolygonselection: case Tpointselection: case Tedgeselection: case Ttexture: case Tcompositing: case Tdisplay:
			case Tprotection: case Tannotation: case ID_OBJECT_TAG:
				break;
			default:
				return false;
		}
		if (tag->GetFirstCTrack())
			return false;
	}
	return true;
}

// What a generator or deformer made is hashed through Cinema 4D's own hashes of its arrays
// (MESHHASH::DATA_HASH): they changed with every change of the content measured (the kinds of change of
// tests/c4d/ipr_reuse.py and 55 more, 2024 and 2027), cost nothing per polygon, and do not ask for the
// data, which in Cinema 4D 2027 costs a generated mesh's tag its data and the view a redraw of the
// mesh. An object of the document that is hashed "when in doubt" is hashed byte by byte as before:
// whatever writes its points while it is evaluated might do so without Cinema 4D's hash noticing.
maxon::UInt64 SceneExporter::MeshSignature(const Leaf& leaf, const maxon::String& meshId, const MeshMaterials& materials, maxon::Bool count)
{
	const cinema::PolygonObject* const mesh = static_cast<const cinema::PolygonObject*>(leaf.object);
	const maxon::Bool own = !leaf.generated && !leaf.deformed && IsIdentityEnough(mesh);
	const maxon::Bool made = (leaf.generated || leaf.deformed) && !_settings.hashBytes;
	if (own && count)
		++_stats.meshesByIdentity;
	if (made && count)
		++_stats.meshesByDataHash;
	maxon::UInt64 hash = own ? MixKeys(HashMeshIdentity(mesh), 1) : made ? MixKeys(HashMeshContent(mesh, MESHHASH::DATA_HASH), 3) : MixKeys(HashMeshContent(mesh, MESHHASH::BYTES), 2);
	hash = MixKeys(hash, maxon::UInt64(meshId.GetHashCode()));
	hash = MixKeys(hash, maxon::UInt64(materials.slotCount));
	if (materials.polygonSlots)
		hash = HashBytes(reinterpret_cast<const maxon::Char*>(materials.polygonSlots), maxon::Int(mesh->GetPolygonCount()) * SIZEOF(maxon::Int32), hash);
	maxon::Int uvIndex = -1;
	if (materials.renderUv)
	{
		uvIndex = 0;
		for (const cinema::BaseTag* tag = mesh->GetFirstTag(); tag && tag != materials.renderUv; tag = tag->GetNext())
			uvIndex += tag->IsInstanceOf(Tuvw) ? 1 : 0;
	}
	hash = MixKeys(hash, maxon::UInt64(uvIndex + 2));
	const maxon::Float64 scale = _coords.GetScale();
	maxon::UInt64 scaleBits = 0;
	maxon::MemCopyType(reinterpret_cast<maxon::Char*>(&scaleBits), reinterpret_cast<const maxon::Char*>(&scale), 8);
	return MixKeys(hash, scaleBits);
}

maxon::Result<maxon::Bool> SceneExporter::GetLightLinking(const Leaf& light, const maxon::BaseArray<maxon::String>& itemIds, LightLinking& linking) const
{
	iferr_scope;

	// The light's own list (the Cycles Light keeps it under ids of its own, with the modes the
	// other way round).
	const maxon::Bool own = light.object->GetType() == ID_LIGHT_OBJECT;
	const cinema::GeData data = GetParameterData(light.object, own ? CYCLESLIGHT_LINKING_LIST : LIGHT_EXCLUSION_LIST);
	const cinema::InExcludeData* const list = data.GetCustomDataType<cinema::InExcludeData>();
	const maxon::Int32 count = list ? list->GetObjectCount() : 0;
	maxon::BaseArray<const cinema::BaseList2D*> listed;
	for (maxon::Int32 i = 0; i < count; ++i)
	{
		if (const cinema::BaseList2D* const entry = list->ObjectFromIndex(_doc, i))
		{
			listed.Append(entry) iferr_return;
		}
	}
	const maxon::Bool includes = own ? GetIntParameter(light.object, CYCLESLIGHT_LINKING_MODE, CYCLESLIGHT_LINKING_MODE_EXCLUDE) == CYCLESLIGHT_LINKING_MODE_INCLUDE
		: GetIntParameter(light.object, LIGHT_EXCLUSION_MODE, LIGHT_EXCLUSION_MODE_EXCLUDE) == LIGHT_EXCLUSION_MODE_INCLUDE;
	linking.include = !listed.IsEmpty() && includes;
	// A listed object stands for itself and everything below it.
	for (maxon::Int index = 0; index < _leaves.GetCount() && !listed.IsEmpty(); ++index)
	{
		if (index >= itemIds.GetCount() || itemIds[index].IsEmpty() || linking.itemIds.Contains(itemIds[index]))
			continue;
		for (const cinema::BaseObject* op = _leaves[index].ownerObject; op; op = op->GetUp())
		{
			if (listed.Contains(op))
			{
				linking.itemIds.Append(itemIds[index]) iferr_return;
				break;
			}
		}
	}

	// The objects whose Cycles Object tag keeps this light away. A light reaches an object only
	// when both agree: the light's own list and the object's tag.
	maxon::BaseArray<maxon::String> kept;
	for (maxon::Int index = 0; index < _leaves.GetCount(); ++index)
	{
		const cinema::BaseTag* const tag = _leaves[index].linkTag;
		if (!tag || index >= itemIds.GetCount() || itemIds[index].IsEmpty() || kept.Contains(itemIds[index]))
			continue;
		maxon::Bool names = false;
		const cinema::GeData tagData = GetParameterData(tag, CYCLESOBJECT_LINKING_LIGHTS);
		if (const cinema::InExcludeData* const lights = tagData.GetCustomDataType<cinema::InExcludeData>())
		{
			for (maxon::Int32 i = 0; i < lights->GetObjectCount() && !names; ++i)
				names = lights->ObjectFromIndex(_doc, i) == light.object;
		}
		const maxon::Bool include = GetIntParameter(tag, CYCLESOBJECT_LINKING_MODE, CYCLESOBJECT_LINKING_MODE_EXCLUDE) == CYCLESOBJECT_LINKING_MODE_INCLUDE;
		if (include != names)
		{
			kept.Append(itemIds[index]) iferr_return;
		}
	}
	if (listed.IsEmpty() && kept.IsEmpty())
		return false;

	if (linking.include)
	{
		// The light lights what it lists, less what the tags keep away.
		maxon::BaseArray<maxon::String> remaining;
		for (const maxon::String& itemId : linking.itemIds)
		{
			if (!kept.Contains(itemId))
			{
				remaining.Append(itemId) iferr_return;
			}
		}
		linking.itemIds = std::move(remaining);
	}
	else
	{
		for (const maxon::String& itemId : kept)
		{
			if (!linking.itemIds.Contains(itemId))
			{
				linking.itemIds.Append(itemId) iferr_return;
			}
		}
	}
	return true;
}

template <typename VISIBILITY> static void WriteVisibility(JsonWriter& json, const VISIBILITY& visibility)
{
	if (visibility.IsDefault())
		return;
	json.BeginObject("visibility");
	json.Bool("camera", visibility.camera);
	json.Bool("shadow", visibility.shadow);
	json.Bool("diffuse", visibility.diffuse);
	json.Bool("glossy", visibility.glossy);
	json.Bool("transmission", visibility.transmission);
	json.Bool("volume_scatter", visibility.volumeScatter);
	json.Bool("holdout", visibility.holdout);
	json.Bool("shadow_catcher", visibility.shadowCatcher);
	if (visibility.passIndex != 0)
		json.Int("pass_index", visibility.passIndex);
	json.EndObject();
}

static void WriteMaterialIds(JsonWriter& json, const maxon::BaseArray<maxon::String>& materialIds)
{
	json.BeginArray("materials");
	for (const maxon::String& materialId : materialIds)
		json.String(nullptr, materialId);
	json.EndArray();
}

// One `object` item. Write() and WriteObjectAlone() both come here, so that the two agree byte for byte.
// @param[in] motion  The matrices at shutter open and close (32 numbers), or nullptr.
static void WriteObjectItem(SceneWriter& writer, const Coords& coords, const cinema::Matrix& mg, const maxon::String& id, const maxon::String& meshId,
	const maxon::BaseArray<maxon::String>& materialIds, const ObjectVisibility& visibility, maxon::Bool objectKeys, maxon::Float64 nameKey, maxon::Int32 objectId,
	const maxon::Float64* motion)
{
	maxon::Float64 matrix[16];
	coords.ObjectMatrix(mg, matrix);
	JsonWriter& json = writer.BeginItem("object", id);
	json.String("mesh", meshId);
	json.Floats("matrix", matrix, 16);
	if (motion)
		json.Floats("motion", motion, 32);
	WriteMaterialIds(json, materialIds);
	WriteVisibility(json, visibility);
	if (objectKeys)
	{
		json.BeginObject("properties");
		json.Float(OBJECT_NAME_KEY, nameKey);
		json.Int(OBJECT_ID_KEY, objectId);
		json.EndObject();
	}
	writer.EndItem();
}

maxon::Result<void> SceneExporter::WriteObject(SceneWriter& writer, const Leaf& leaf, const Resolved& resolved)
{
	iferr_scope;
	const maxon::String id = KeyId("object_", leaf.key);
	maxon::Float64 motion[32];
	maxon::Bool moves = false;
	if (_motionOpen && _motionClose)
	{
		const auto* const open = _motionOpen->matrices.Find(leaf.key);
		const auto* const close = _motionClose->matrices.Find(leaf.key);
		if (open && close && (open->GetValue() != leaf.mg || close->GetValue() != leaf.mg))
		{
			_coords.ObjectMatrix(open->GetValue(), motion);
			_coords.ObjectMatrix(close->GetValue(), motion + 16);
			moves = true;
		}
	}
	WriteObjectItem(writer, _coords, leaf.mg, id, resolved.meshId, resolved.materialIds, leaf.visibility, resolved.objectKeys, leaf.nameKey, leaf.objectId,
		moves ? motion : nullptr);

	if (!_settings.noteFollowed)
		return maxon::OK;
	NoteOwned(leaf.ownerObject, id) iferr_return;
	if (!leaf.followable || !leaf.ownerObject || moves)
		return NoteUnfollowed(leaf.ownerObject);
	WrittenObject written;
	written.owner = leaf.ownerObject;
	written.id = id;
	written.meshId = resolved.meshId;
	written.materialIds.CopyFrom(resolved.materialIds) iferr_return;
	written.visibility = leaf.visibility;
	written.objectKeys = resolved.objectKeys;
	written.nameKey = leaf.nameKey;
	written.objectId = leaf.objectId;
	for (maxon::Int node = leaf.tail; node != NOTOK; node = _tails[node].parent)
	{
		written.tail.Append(_tails[node].ml) iferr_return;
	}
	// Collected from the leaf upwards; the walk multiplied from the owner downwards.
	for (maxon::Int i = 0, j = written.tail.GetCount() - 1; i < j; ++i, --j)
		std::swap(written.tail[i], written.tail[j]);
	_writtenObjects.Append(std::move(written)) iferr_return;
	return maxon::OK;
}

// The fields of a `proxy` item but its place (protocol/protocol.md). Write() and WriteObjectAlone() both come
// here, so that the two agree byte for byte.
static void WriteProxyFields(JsonWriter& json, const ProxySource& source, const ObjectVisibility& visibility)
{
	json.String("path", source.path);
	json.String("stamp", source.stamp);
	if (source.kind == CYCLESPROXY_TAKE_KIND_COLLECTION)
		json.String("collection", source.name);
	else if (source.kind == CYCLESPROXY_TAKE_KIND_SCENE)
		json.String("scene", source.name);
	json.Bool("lights", source.lights);
	json.Int("frame", source.frame);
	json.String("name", source.label);
	WriteVisibility(json, visibility);
}

// One `proxy` item with one place. A point of the file (Blender's axes, metres) lies at the object's
// (x, z, y) in metres, so the object's matrix as a mesh's object has it is the proxy's (coords.h).
static void WriteProxyItem(SceneWriter& writer, const Coords& coords, const cinema::Matrix& mg, const maxon::String& id, const ProxySource& source,
	const ObjectVisibility& visibility)
{
	maxon::Float64 matrix[16];
	coords.ObjectMatrix(mg, matrix);
	JsonWriter& json = writer.BeginItem("proxy", id);
	json.Floats("matrix", matrix, 16);
	WriteProxyFields(json, source, visibility);
	writer.EndItem();
}

maxon::Result<maxon::String> SceneExporter::WriteProxy(SceneWriter& writer, const Leaf& leaf)
{
	iferr_scope;
	ProxySource source;
	maxon::String problem;
	if (!ReadProxySource(leaf.object, _doc, source, problem))
	{
		writer.Warn(problem);
		return maxon::String();
	}
	const maxon::String id = KeyId("proxy_", leaf.key);
	NoteOwned(leaf.ownerObject, id) iferr_return;
	if (leaf.instanceSet != NOTOK)
	{
		// Inside a multi-instance: the file once per copy, as one item. The leaf's matrix is relative to a copy.
		const InstanceSet& set = _instanceSets[leaf.instanceSet];
		const maxon::Int count = set.matrices->GetCount();
		maxon::Int matricesBlob = 0;
		maxon::Float32* const matrices = writer.AllocFloats(count * 16, matricesBlob) iferr_return;
		for (maxon::Int i = 0; i < count; ++i)
		{
			maxon::Float64 matrix[16];
			_coords.ObjectMatrix((*set.matrices)[i] * leaf.mg, matrix);
			for (maxon::Int k = 0; k < 16; ++k)
				matrices[i * 16 + k] = maxon::Float32(matrix[k]);
		}
		JsonWriter& json = writer.BeginItem("proxy", id);
		json.Int("matrices", matricesBlob);
		WriteProxyFields(json, source, leaf.visibility);
		writer.EndItem();
		NoteUnfollowed(leaf.ownerObject) iferr_return;
		return id;
	}
	WriteProxyItem(writer, _coords, leaf.mg, id, source, leaf.visibility);
	if (!_settings.noteFollowed)
		return id;
	if (!leaf.followable || !leaf.ownerObject)
	{
		NoteUnfollowed(leaf.ownerObject) iferr_return;
		return id;
	}
	WrittenObject written;
	written.owner = leaf.ownerObject;
	written.id = id;
	written.visibility = leaf.visibility;
	written.proxy = true;
	written.source = std::move(source);
	for (maxon::Int node = leaf.tail; node != NOTOK; node = _tails[node].parent)
	{
		written.tail.Append(_tails[node].ml) iferr_return;
	}
	// Collected from the leaf upwards; the walk multiplied from the owner downwards.
	for (maxon::Int i = 0, j = written.tail.GetCount() - 1; i < j; ++i, --j)
		std::swap(written.tail[i], written.tail[j]);
	_writtenObjects.Append(std::move(written)) iferr_return;
	return id;
}

void SceneExporter::WriteObjectAlone(SceneWriter& writer, const Coords& coords, const WrittenObject& object, const cinema::Matrix& ownerMg)
{
	// The product in the walk's order: the owner's matrix times each matrix below it, one after the other.
	cinema::Matrix mg = ownerMg;
	for (const cinema::Matrix& ml : object.tail)
		mg = mg * ml;
	if (object.proxy)
	{
		WriteProxyItem(writer, coords, mg, object.id, object.source, object.visibility);
		return;
	}
	WriteObjectItem(writer, coords, mg, object.id, object.meshId, object.materialIds, object.visibility, object.objectKeys, object.nameKey, object.objectId, nullptr);
}

maxon::Result<void> SceneExporter::WriteInstancer(SceneWriter& writer, const maxon::String& id, const Resolved& resolved,
	const maxon::BaseArray<cinema::Matrix>& matrices, const maxon::BaseArray<cinema::Vector>* colors, const maxon::BaseArray<maxon::Vector2d>* keys,
	const Visibility& visibility, const maxon::BaseArray<InstancerPart>* parts)
{
	iferr_scope;

	const maxon::Int count = matrices.GetCount();
	maxon::Int matricesBlob = 0;
	maxon::Float32* const outMatrices = writer.AllocFloats(count * 16, matricesBlob) iferr_return;
	for (maxon::Int i = 0; i < count; ++i)
	{
		maxon::Float64 matrix[16];
		_coords.ObjectMatrix(matrices[i], matrix);
		for (maxon::Int k = 0; k < 16; ++k)
			outMatrices[i * 16 + k] = maxon::Float32(matrix[k]);
	}
	maxon::Int colorsBlob = NOTOK;
	if (colors && colors->GetCount() == count)
	{
		// MoGraph's colours are display colours: Cinema 4D's Color shader limits them to 0..1 and
		// reads them as sRGB (its default colour space), converting to the render space. Measured
		// against the Standard renderer; docs/reference/c4d-sdk.md, "MoGraph".
		maxon::BaseArray<cinema::Vector32> converted;
		converted.Resize(count) iferr_return;
		for (maxon::Int i = 0; i < count; ++i)
		{
			const cinema::Vector& color = (*colors)[i];
			converted[i] = cinema::Vector32(maxon::Float32(maxon::Clamp01(color.x)), maxon::Float32(maxon::Clamp01(color.y)), maxon::Float32(maxon::Clamp01(color.z)));
		}
		if (!_ocioTried)
		{
			_ocioTried = true;
			if (_colors.IsOcio())
			{
				iferr (_ocio = InitOcioConverter(_doc, nullptr))
					_ocio = nullptr;
			}
		}
		if (_ocio)
		{
			_ocio->TransformColors(converted.GetFirst(), count, cinema::COLORSPACETRANSFORMATION::OCIO_SRGB_TO_RENDERING);
		}
		else if (_colors.IsOcio())
		{
			// No converter could be made: at least the transfer curve is undone.
			for (cinema::Vector32& color : converted)
				color = cinema::Vector32(maxon::Float32(SrgbToLinear(color.x)), maxon::Float32(SrgbToLinear(color.y)), maxon::Float32(SrgbToLinear(color.z)));
		}
		else
		{
			// "Basic" colour management: a colour parameter like any other. With Linear Workflow
			// the sRGB curve is undone, without it the colour is used as it is (DocumentColors::Input;
			// assumed from the other colour parameters, not measured with clones).
			for (cinema::Vector32& color : converted)
				color = cinema::Vector32(_colors.Input(cinema::Vector(color)));
		}

		maxon::Float32* const outColors = writer.AllocFloats(count * 4, colorsBlob) iferr_return;
		for (maxon::Int i = 0; i < count; ++i)
		{
			outColors[i * 4 + 0] = converted[i].x;
			outColors[i * 4 + 1] = converted[i].y;
			outColors[i * 4 + 2] = converted[i].z;
			outColors[i * 4 + 3] = 1.0f;
		}
	}

	// The object keys of Redshift's random nodes, per copy (Leaf::nameKey).
	maxon::Int nameKeysBlob = NOTOK;
	maxon::Int objectIdsBlob = NOTOK;
	if (keys && keys->GetCount() == count)
	{
		maxon::Float32* const names = writer.AllocFloats(count, nameKeysBlob) iferr_return;
		maxon::Float32* const ids = writer.AllocFloats(count, objectIdsBlob) iferr_return;
		for (maxon::Int i = 0; i < count; ++i)
		{
			names[i] = maxon::Float32((*keys)[i].x);
			ids[i] = maxon::Float32((*keys)[i].y);
		}
	}

	JsonWriter& json = writer.BeginItem("instancer", id);
	if (parts)
	{
		// The meshes of one copy, each with its place in the copy (the conversion of a matrix is a change
		// of basis, so a copy's matrix times a part's is the matrix the part alone would be sent with).
		json.BeginArray("parts");
		for (const InstancerPart& part : *parts)
		{
			maxon::Float64 matrix[16];
			_coords.ObjectMatrix(part.ml, matrix);
			json.BeginObject();
			json.String("mesh", part.resolved->meshId);
			WriteMaterialIds(json, part.resolved->materialIds);
			json.Floats("matrix", matrix, 16);
			json.EndObject();
		}
		json.EndArray();
	}
	else
	{
		json.String("mesh", resolved.meshId);
		WriteMaterialIds(json, resolved.materialIds);
	}
	json.Int("matrices", matricesBlob);
	if (colorsBlob != NOTOK)
		json.Int("colors", colorsBlob);
	if (nameKeysBlob != NOTOK)
	{
		json.BeginArray("floats");
		json.BeginObject();
		json.String("name", maxon::String(OBJECT_NAME_KEY));
		json.Int("data", nameKeysBlob);
		json.EndObject();
		json.BeginObject();
		json.String("name", maxon::String(OBJECT_ID_KEY));
		json.Int("data", objectIdsBlob);
		json.EndObject();
		json.EndArray();
	}
	WriteVisibility(json, visibility);
	writer.EndItem();
	return maxon::OK;
}

maxon::Result<void> SceneExporter::Write(SceneWriter& writer)
{
	iferr_scope;

	for (const maxon::String& note : _notes)
		writer.Warn(note);

	WriteState state(writer, _doc, _settings.bakeResolution);
	state.materials.SetBlenderVersion(_settings.blenderVersion, _settings.blenderVersionText);
	if (_partial)
	{
		// Only meshes and the items that show them, with the materials the last whole export wrote, fog
		// boxes, whose material is their own, and proxies, which have none. A light that is an object of the document is the caller's
		// (WriteLightAlone); anything else is refused.
		state.materials.SetKnownIds(_settings.materials);
		_refused = !_worlds.IsEmpty();
		for (const Leaf& leaf : _leaves)
			_refused = _refused || (leaf.kind != LEAF::MESH && leaf.kind != LEAF::FOG && leaf.kind != LEAF::PROXY && !(leaf.kind == LEAF::LIGHT && leaf.key == leaf.object->GetGUID()));
		if (_refused)
			return maxon::OK;
	}
	for (const Leaf& leaf : _leaves)
	{
		if (leaf.kind != LEAF::MESH || !leaf.generated)
			continue;
		const auto* const known = state.ownerCounts.Find(leaf.owner);
		state.ownerCounts.Insert(leaf.owner, (known ? known->GetValue() : 0) + 1) iferr_return;
	}

	// Copies of one mesh with the same materials inside one generator. A multi-instance is such
	// a group by itself, and joins the real copies its generator made next to it.
	struct Group
	{
		Resolved resolved;
		maxon::BaseArray<maxon::Int> leaves;
		maxon::Int count = 0;              ///< copies, with every copy of a multi-instance counted
		maxon::Bool hasColor = false;
		maxon::Bool hasInstances = false;
	};
	maxon::BaseArray<Group> groups;
	maxon::HashMap<maxon::UInt64, maxon::Int> groupIndex;
	const auto addToGroup = [&](maxon::Int index, Resolved&& resolved) -> maxon::Result<void>
	{
		iferr_scope;
		const Leaf& leaf = _leaves[index];
		const InstanceSet* const set = leaf.instanceSet != NOTOK ? &_instanceSets[leaf.instanceSet] : nullptr;
		maxon::UInt64 groupKey = MixKeys(leaf.owner, maxon::UInt64(resolved.meshId.GetHashCode()));
		for (const maxon::String& materialId : resolved.materialIds)
			groupKey = MixKeys(groupKey, maxon::UInt64(materialId.GetHashCode()));
		maxon::Int groupPosition = NOTOK;
		if (const auto* const known = groupIndex.Find(groupKey))
		{
			groupPosition = known->GetValue();
		}
		else
		{
			Group group;
			group.resolved = std::move(resolved);
			groups.Append(std::move(group)) iferr_return;
			groupPosition = groups.GetCount() - 1;
			groupIndex.Insert(groupKey, groupPosition) iferr_return;
		}
		Group& group = groups[groupPosition];
		group.leaves.Append(index) iferr_return;
		group.count += set ? set->matrices->GetCount() : 1;
		group.hasColor = group.hasColor || leaf.hasColor || (set && set->colors);
		group.hasInstances = group.hasInstances || set;
		return maxon::OK;
	};

	// The meshes of one instance set (the copies of an object listed in a Cycles Object tag, of a Cycles
	// Scatter's source, of a multi-instance): an object made of several meshes is one instancer whose copies
	// each show all of them, where that can be (below).
	struct SetGroup
	{
		maxon::Int set = NOTOK;
		maxon::BaseArray<maxon::Int> leaves;
		maxon::BaseArray<Resolved> resolved;
	};
	maxon::BaseArray<SetGroup> setGroups;
	maxon::HashMap<maxon::Int, maxon::Int> setGroupIndex;
	maxon::BaseArray<maxon::Int> setLeafCounts;
	setLeafCounts.Resize(_instanceSets.GetCount()) iferr_return;
	for (maxon::Int& value : setLeafCounts)
		value = 0;
	for (const Leaf& leaf : _leaves)
	{
		if (leaf.instanceSet != NOTOK)
			++setLeafCounts[leaf.instanceSet];
	}

	// The item each leaf ends up in, for the lights' include and exclude lists. Lights are
	// written last, when all of these are known.
	maxon::BaseArray<maxon::String> itemIds;
	itemIds.Resize(_leaves.GetCount()) iferr_return;

	// The grids of one object of the document are one volume.
	maxon::BaseArray<VolumeSource> volumes;
	maxon::BaseArray<maxon::UInt64> volumeOwners;

	for (maxon::Int index = 0; index < _leaves.GetCount(); ++index)
	{
		const Leaf& leaf = _leaves[index];
		if (leaf.kind == LEAF::LIGHT)
			continue;
		if (leaf.kind == LEAF::HAIR)
		{
			// A texture tag on the Hair object itself: its material colours the hairs, and the hair material
			// keeps giving their shape (the last such tag counts, as on any object).
			maxon::String materialId;
			for (cinema::BaseTag* tag = leaf.object->GetFirstTag(); tag; tag = tag->GetNext())
			{
				if (tag->GetType() != Ttexture)
					continue;
				if (cinema::BaseMaterial* const material = static_cast<cinema::TextureTag*>(tag)->GetMaterial())
				{
					materialId = state.materials.GetMaterialId(material) iferr_return;
				}
			}
			maxon::Bool kept = false;
			const maxon::Bool written = ExportHair(writer, _coords, _doc, leaf.object, KeyId("hair_", leaf.key), materialId, _settings.reuse,
				_settings.reuse != nullptr || _settings.noteSources, &kept) iferr_return;
			_stats.hairsKept += kept ? 1 : 0;
			_stats.hairsGenerated += written && !kept ? 1 : 0;
			NoteFixed(leaf.ownerObject) iferr_return;
			continue;
		}
		if (leaf.kind == LEAF::PARTICLES)
		{
			maxon::String materialId;
			if (cinema::BaseMaterial* const material = GetStackMaterial(leaf.stack))
			{
				materialId = state.materials.GetMaterialId(material) iferr_return;
			}
			ExportParticles(writer, _coords, leaf.object, leaf.mg, KeyId("particles_", leaf.key), materialId) iferr_return;
			NoteFixed(leaf.ownerObject) iferr_return;
			continue;
		}
		if (leaf.kind == LEAF::FOG)
		{
			// With a noise: the noise's grid as a `volume`, which only a whole export writes (as other volumes).
			if (HasFogNoise(leaf.object))
			{
				if (_partial)
				{
					_refused = true;
					return maxon::OK;
				}
				const maxon::String volumeId = KeyId("fog_", leaf.key);
				const maxon::Bool written = ExportFogNoise(writer, _coords, _colors, _doc, leaf.object, leaf.mg, volumeId, volumeId + ".material"_s) iferr_return;
				if (written)
				{
					NoteOwned(leaf.ownerObject, volumeId) iferr_return;
					NoteOwned(leaf.ownerObject, volumeId + ".material"_s) iferr_return;
				}
				NoteFixed(leaf.ownerObject) iferr_return;
				continue;
			}
			// Its box, its material and the object that shows them; it moves like a mesh's object.
			Resolved resolved;
			resolved.meshId = KeyId("fog_", leaf.key);
			const maxon::String materialId = resolved.meshId + ".material"_s;
			const maxon::Bool written = ExportFog(writer, _coords, _colors, leaf.object, resolved.meshId, materialId) iferr_return;
			if (!written)
				continue;
			resolved.materialIds.Append(materialId) iferr_return;
			NoteOwned(leaf.ownerObject, resolved.meshId) iferr_return;
			NoteOwned(leaf.ownerObject, resolved.meshId + ".points"_s) iferr_return;
			NoteOwned(leaf.ownerObject, materialId) iferr_return;
			WriteObject(writer, leaf, resolved) iferr_return;
			itemIds[index] = KeyId("object_", leaf.key);
			continue;
		}
		if (leaf.kind == LEAF::PROXY)
		{
			itemIds[index] = WriteProxy(writer, leaf) iferr_return;
			continue;
		}
		if (leaf.kind == LEAF::VOLUME)
		{
			maxon::Int position = NOTOK;
			for (maxon::Int i = 0; i < volumeOwners.GetCount() && position == NOTOK; ++i)
			{
				if (volumeOwners[i] == leaf.owner && volumes[i].set == leaf.volumeSet)
					position = i;
			}
			if (position == NOTOK)
			{
				VolumeSource source;
				source.owner = leaf.ownerObject;
				source.set = leaf.volumeSet;
				source.mg = leaf.mg;
				volumes.Append(std::move(source)) iferr_return;
				volumeOwners.Append(leaf.owner) iferr_return;
				position = volumes.GetCount() - 1;
			}
			volumes[position].grids.Append(static_cast<const cinema::VolumeObject*>(leaf.object)) iferr_return;
			NoteFixed(leaf.ownerObject) iferr_return;
			continue;
		}

		Resolved resolved;
		Resolve(leaf, state, resolved) iferr_return;
		if (state.materials.HasUnknown())
		{
			// CollectOwners(): a material the last whole export did not write.
			_refused = true;
			return maxon::OK;
		}
		if (resolved.meshId.IsEmpty())
			continue;

		const InstanceSet* const set = leaf.instanceSet != NOTOK ? &_instanceSets[leaf.instanceSet] : nullptr;
		const auto* const ownerCount = state.ownerCounts.Find(leaf.owner);
		const maxon::Bool mayBatch = leaf.generated && (leaf.hasColor || set || (ownerCount && ownerCount->GetValue() >= BATCH_MINIMUM));
		if (!mayBatch)
		{
			WriteObject(writer, leaf, resolved) iferr_return;
			itemIds[index] = KeyId("object_", leaf.key);
			continue;
		}

		if (set && setLeafCounts[leaf.instanceSet] >= 2)
		{
			maxon::Int position = NOTOK;
			if (const auto* const known = setGroupIndex.Find(leaf.instanceSet))
			{
				position = known->GetValue();
			}
			else
			{
				SetGroup group;
				group.set = leaf.instanceSet;
				setGroups.Append(std::move(group)) iferr_return;
				position = setGroups.GetCount() - 1;
				setGroupIndex.Insert(leaf.instanceSet, position) iferr_return;
			}
			setGroups[position].leaves.Append(index) iferr_return;
			setGroups[position].resolved.Append(std::move(resolved)) iferr_return;
			continue;
		}
		addToGroup(index, std::move(resolved)) iferr_return;
	}

	for (SetGroup& group : setGroups)
	{
		// Together when every mesh is shown the same way and none of the materials reads a number per
		// object (those numbers are the copies' own in Blender, one set per instancer); and not for more
		// meshes than INSTANCER_PARTS_MAXIMUM, each of which is a node in Blender. Else as before: one
		// instancer per mesh and set of materials.
		const Leaf& first = _leaves[group.leaves[0]];
		maxon::Bool together = group.leaves.GetCount() >= 2 && group.leaves.GetCount() <= INSTANCER_PARTS_MAXIMUM;
		for (maxon::Int i = 0; i < group.leaves.GetCount() && together; ++i)
		{
			const Visibility& a = _leaves[group.leaves[i]].visibility;
			const Visibility& b = first.visibility;
			together = !group.resolved[i].objectKeys && a.camera == b.camera && a.shadow == b.shadow && a.diffuse == b.diffuse && a.glossy == b.glossy &&
				a.transmission == b.transmission && a.volumeScatter == b.volumeScatter && a.holdout == b.holdout && a.shadowCatcher == b.shadowCatcher &&
				a.passIndex == b.passIndex;
		}
		if (!together)
		{
			for (maxon::Int i = 0; i < group.leaves.GetCount(); ++i)
			{
				addToGroup(group.leaves[i], std::move(group.resolved[i])) iferr_return;
			}
			continue;
		}
		const InstanceSet& set = _instanceSets[group.set];
		maxon::BaseArray<InstancerPart> parts;
		parts.EnsureCapacity(group.leaves.GetCount()) iferr_return;
		for (maxon::Int i = 0; i < group.leaves.GetCount(); ++i)
		{
			InstancerPart part;
			part.resolved = &group.resolved[i];
			part.ml = _leaves[group.leaves[i]].mg;     // (a leaf's matrix is relative to a copy)
			parts.Append(part) iferr_return;
		}
		maxon::BaseArray<cinema::Vector> colors;
		if (set.colors)
		{
			colors.EnsureCapacity(set.colors->GetCount()) iferr_return;
			for (const maxon::Color64& color : *set.colors)
			{
				colors.Append(cinema::Vector(color.r, color.g, color.b)) iferr_return;
			}
		}
		const maxon::String instancerId = KeyId("instancer_", first.key);
		WriteInstancer(writer, instancerId, group.resolved[0], *set.matrices, set.colors ? &colors : nullptr, nullptr, first.visibility, &parts) iferr_return;
		NoteOwned(first.ownerObject, instancerId) iferr_return;
		for (maxon::Int index : group.leaves)
		{
			itemIds[index] = instancerId;
			NoteUnfollowed(_leaves[index].ownerObject) iferr_return;
		}
	}

	for (const Group& group : groups)
	{
		// Few copies without a colour of their own stay plain objects.
		if (!group.hasColor && !group.hasInstances && group.count < BATCH_MINIMUM)
		{
			for (maxon::Int index : group.leaves)
			{
				WriteObject(writer, _leaves[index], group.resolved) iferr_return;
				itemIds[index] = KeyId("object_", _leaves[index].key);
			}
			continue;
		}
		maxon::BaseArray<cinema::Matrix> matrices;
		maxon::BaseArray<cinema::Vector> colors;
		maxon::BaseArray<maxon::Vector2d> keys;
		matrices.EnsureCapacity(group.count) iferr_return;
		colors.EnsureCapacity(group.count) iferr_return;
		for (maxon::Int index : group.leaves)
		{
			const Leaf& leaf = _leaves[index];
			// The copies of a multi-instance share the keys of the one leaf they come from (Redshift
			// gives them one value, measured).
			const maxon::Vector2d key(leaf.nameKey, maxon::Float64(leaf.objectId));
			if (leaf.instanceSet == NOTOK)
			{
				matrices.Append(leaf.mg) iferr_return;
				colors.Append(leaf.hasColor ? leaf.color : cinema::Vector(0.5)) iferr_return;
				if (group.resolved.objectKeys)
				{
					keys.Append(key) iferr_return;
				}
				continue;
			}
			// A multi-instance: the mesh once per copy. The leaf's matrix is relative to a copy.
			const InstanceSet& set = _instanceSets[leaf.instanceSet];
			for (maxon::Int i = 0; i < set.matrices->GetCount(); ++i)
			{
				matrices.Append((*set.matrices)[i] * leaf.mg) iferr_return;
				colors.Append(set.colors ? cinema::Vector((*set.colors)[i].r, (*set.colors)[i].g, (*set.colors)[i].b) : cinema::Vector(0.5)) iferr_return;
				if (group.resolved.objectKeys)
				{
					keys.Append(key) iferr_return;
				}
			}
		}
		const maxon::String instancerId = KeyId("instancer_", _leaves[group.leaves[0]].key);
		WriteInstancer(writer, instancerId, group.resolved, matrices, group.hasColor ? &colors : nullptr, group.resolved.objectKeys ? &keys : nullptr,
			_leaves[group.leaves[0]].visibility) iferr_return;
		NoteOwned(_leaves[group.leaves[0]].ownerObject, instancerId) iferr_return;
		for (maxon::Int index : group.leaves)
		{
			itemIds[index] = instancerId;
			NoteUnfollowed(_leaves[index].ownerObject) iferr_return;
		}
	}
	if (_partial)
		return maxon::OK;

	_writtenLights.Reset();
	for (const Leaf& leaf : _leaves)
	{
		if (leaf.kind != LEAF::LIGHT)
			continue;
		LightLinking linking;
		const maxon::Bool linked = GetLightLinking(leaf, itemIds, linking) iferr_return;
		_lightLinking = _lightLinking || linked;
		const maxon::String id = KeyId("light_", leaf.key);
		ExportLight(writer, _coords, _colors, leaf.object, leaf.mg, id, _settings.lightMultiplier, linked ? &linking : nullptr,
			EeveeHidesLights(_settings)) iferr_return;
		// An object of the document is walked under its own GUID (Collect, Walk), with its matrix
		// formed from the local ones of its parents: such a light can be written again by itself.
		if (leaf.key == ObjectKey(leaf.object))
		{
			WrittenLight written;
			written.object = leaf.object;
			written.id = id;
			written.linked = linked;
			written.linking = std::move(linking);
			_writtenLights.Append(std::move(written)) iferr_return;
		}
		else
		{
			// A light a generator made (a Cloner's): written with the whole scene only.
			NoteFixed(leaf.ownerObject) iferr_return;
		}
	}

	for (maxon::Int i = 0; i < volumes.GetCount(); ++i)
	{
		ExportVolume(writer, _coords, _doc, KeyId("volume_", volumeOwners[i]), volumes[i]) iferr_return;
	}

	if (_camera && _settings.writeCamera)
	{
		const CameraValues camera = CameraValues::Read(_camera);
		cinema::Matrix cameraMotion[2];
		const maxon::Bool moves = _motionOpen && _motionClose && _motionOpen->hasCamera && _motionClose->hasCamera &&
			(_motionOpen->camera != camera.mg || _motionClose->camera != camera.mg);
		if (moves)
		{
			cameraMotion[0] = _motionOpen->camera;
			cameraMotion[1] = _motionClose->camera;
		}
		ExportCamera(writer, _coords, camera, _settings.depthOfField, _settings.eevee && _settings.blenderVersion < EEVEE_PANORAMA_VERSION, CameraFraming::Full(_settings.width, _settings.height, _settings.pixelAspect),
			moves ? cameraMotion : nullptr) iferr_return;
	}
	// The world, per role (the maintainer's rules of 2026-11-23 and 24: "The top environment wins", per role):
	// what the camera sees, what lights the scene and the world's fog each come from the highest Sky object,
	// Redshift dome light or Cycles Environment in the Object Manager that renders and asks for that role.
	// Without one for the camera and none for the light, the plain world of the render settings.
	_writtenWorld = WrittenWorld();
	const WorldSource* used[3] = { nullptr, nullptr, nullptr };
	for (const WorldSource& source : _worlds)
	{
		const maxon::Bool wants[3] = { source.roles.camera, source.roles.lighting, source.roles.fog };
		for (maxon::Int role = 0; role < 3; ++role)
		{
			if (!used[role] && wants[role])
				used[role] = &source;
		}
	}
	static const maxon::Char* const KIND_NAMES[] = { "", "Sky object", "Redshift dome light", "Cycles Environment" };
	maxon::Bool alone = used[0] || used[1];
	for (const WorldSource& source : _worlds)
	{
		const maxon::Bool isUsed = &source == used[0] || &source == used[1] || &source == used[2];
		// One that asks for a role and fills none is named; one that asks for none is passed over silently.
		if (!isUsed && (source.roles.camera || source.roles.lighting || source.roles.fog))
		{
			writer.Warn(FormatString("Environment: the @ \"@\" is not used: environments above it in the Object Manager fill each of its roles.",
				maxon::String(KIND_NAMES[maxon::Int(source.kind)]), source.object->GetName()));
		}
		// (A Material type's world is its node graph's: its images and its values only a whole export follows.)
		if (isUsed && (source.kind != WORLDPART::ENVIRONMENT || !source.own || GetIntParameter(source.object, CYCLESENV_TYPE, CYCLESENV_TYPE_SKY) == CYCLESENV_TYPE_MATERIAL))
			alone = false;
	}
	// The world can be written again by itself (WriteWorldAlone) when every role is a Cycles Environment of the
	// document or nothing; then those are followed and every other environment is the whole export's.
	for (const WorldSource& source : _worlds)
	{
		const maxon::Bool isUsed = &source == used[0] || &source == used[1] || &source == used[2];
		if (source.kind == WORLDPART::ENVIRONMENT && !(isUsed && alone))
		{
			NoteFixed(source.owner) iferr_return;
		}
	}
	WorldPart parts[3];
	for (maxon::Int role = 0; role < 3; ++role)
	{
		if (!used[role])
			continue;
		parts[role].kind = used[role]->kind;
		parts[role].object = used[role]->object;
		parts[role].mg = used[role]->mg;
		parts[role].material = used[role]->material;
		_writtenWorld.objects[role] = used[role]->object;
		_writtenWorld.roles[role] = used[role]->roles;
	}
	ExportWorldItem(state.materials, parts[0], parts[1], parts[2], _settings.worldColor, _settings.worldStrength, &_writtenWorld.images) iferr_return;
	_writtenWorld.alone = alone;
	if (_settings.noteFollowed)
	{
		state.materials.TakeIds(_materialIds) iferr_return;
	}
	return maxon::OK;
}

maxon::Result<maxon::String> SceneExporter::GetListing() const
{
	iferr_scope;

	maxon::String text;
	for (const Leaf& leaf : _leaves)
	{
		if (leaf.kind == LEAF::LIGHT)
		{
			if (leaf.object->GetType() == ID_LIGHT_OBJECT)
				text += FormatString("cycles light \"@\" type=@ pos=@\n", leaf.path, GetIntParameter(leaf.object, CYCLESLIGHT_TYPE, 0), VectorText(leaf.mg.off));
			else
				text += FormatString("light \"@\" type=@ pos=@\n", leaf.path, GetIntParameter(leaf.object, LIGHT_TYPE, 0), VectorText(leaf.mg.off));
			continue;
		}
		if (leaf.kind == LEAF::HAIR || leaf.kind == LEAF::PARTICLES)
		{
			text += FormatString("@ \"@\" pos=@\n", leaf.kind == LEAF::HAIR ? "hair"_s : "particles"_s, leaf.path, VectorText(leaf.mg.off));
			continue;
		}
		if (leaf.kind == LEAF::VOLUME)
		{
			text += FormatString("volume \"@\" pos=@\n", leaf.path, VectorText(leaf.mg.off));
			continue;
		}
		if (leaf.kind == LEAF::PROXY)
		{
			maxon::String extra;
			if (leaf.instanceSet != NOTOK)
				extra = FormatString(" instances=@", _instanceSets[leaf.instanceSet].matrices->GetCount());
			text += FormatString("proxy \"@\" pos=@ file=\"@\"@\n", leaf.path, VectorText(leaf.mg.off),
				leaf.object->GetDataInstanceRef().GetFilename(CYCLESPROXY_FILE).GetFileString(), extra);
			continue;
		}
		if (leaf.kind == LEAF::FOG)
		{
			text += FormatString("fog \"@\" pos=@ size=@\n", leaf.path, VectorText(leaf.mg.off),
				VectorText(GetVectorParameter(leaf.object, CYCLESFOG_SIZE, cinema::Vector(FOG_DEFAULT_SIZE))));
			continue;
		}
		const cinema::PolygonObject* const mesh = static_cast<const cinema::PolygonObject*>(leaf.object);
		const cinema::BaseMaterial* const material = GetStackMaterial(leaf.stack);
		maxon::String extra;
		if (leaf.instanceSet != NOTOK)
			extra = FormatString(" instances=@", _instanceSets[leaf.instanceSet].matrices->GetCount());
		text += FormatString("mesh \"@\" points=@ polygons=@ pos=@ size=@ material=\"@\"@\n", leaf.path, mesh->GetPointCount(), mesh->GetPolygonCount(),
			VectorText(leaf.mg.off), VectorText(mesh->GetRad() * 2.0), material ? material->GetName() : maxon::String(), extra);
	}
	// Every candidate for the world, the one used first (Write()).
	for (const WorldSource& source : _worlds)
	{
		if (source.kind == WORLDPART::SKY)
			text += FormatString("sky \"@\" material=\"@\"\n", source.object->GetName(), source.material ? source.material->GetName() : maxon::String());
		else if (source.kind == WORLDPART::DOME)
			text += FormatString("dome \"@\"\n", source.object->GetName());
		else
			text += FormatString("environment \"@\" type=@\n", source.object->GetName(), GetIntParameter(source.object, CYCLESENV_TYPE, 0));
	}
	if (_camera)
		text += FormatString("camera \"@\" pos=@\n", _camera->GetName(), VectorText(_camera->GetMg().off));
	for (const maxon::String& note : _notes)
		text += FormatString("note @\n", note);
	return text;
}

maxon::Result<maxon::String> SceneExporter::GetSourceListing() const
{
	iferr_scope;

	JsonWriter json;
	json.BeginObject();
	json.BeginArray("leaves");
	for (const Leaf& leaf : _leaves)
	{
		if (leaf.kind != LEAF::MESH)
			continue;
		const cinema::PolygonObject* const mesh = static_cast<const cinema::PolygonObject*>(leaf.object);
		json.BeginObject();
		json.String("path", leaf.path);
		json.Bool("generated", leaf.generated);
		json.Bool("deformed", leaf.deformed);
		json.String("key", maxon::String::HexToString(leaf.key, false));
		json.String("object", maxon::String::HexToString(maxon::UInt64(reinterpret_cast<maxon::UInt>(mesh)), false));
		json.String("identity", maxon::String::HexToString(HashMeshIdentity(mesh), false));
		// The data hash first: reading the data themselves changes it in Cinema 4D 2027 (mesh_export.h).
		json.String("data_hash", maxon::String::HexToString(HashMeshContent(mesh, MESHHASH::DATA_HASH), false));
		json.String("content", maxon::String::HexToString(HashMeshContent(mesh, MESHHASH::BYTES), false));
		json.String("data_hash_after", maxon::String::HexToString(HashMeshContent(mesh, MESHHASH::DATA_HASH), false));
		json.String("owner", maxon::String::HexToString(maxon::UInt64(reinterpret_cast<maxon::UInt>(leaf.ownerObject)), false));
		if (leaf.ownerObject)
		{
			const maxon::Float64 dirty[4] = { maxon::Float64(leaf.ownerObject->GetDirty(cinema::DIRTYFLAGS::DATA)),
				maxon::Float64(leaf.ownerObject->GetDirty(cinema::DIRTYFLAGS::MATRIX)), maxon::Float64(leaf.ownerObject->GetDirty(cinema::DIRTYFLAGS::CACHE)),
				maxon::Float64(leaf.ownerObject->GetDirty(cinema::DIRTYFLAGS::CHILDREN)) };
			json.Floats("owner_dirty", dirty, 4);
		}
		json.EndObject();
	}
	json.EndArray();
	json.EndObject();
	return json.GetText();
}

// --- the point under the mouse (the IPR's navigation) ----------------------------------------

// The nearest hit closer than `best` of the ray o + d t (t in document units along the world ray; o
// and d already in the mesh's own axes) with the mesh: its bounding box first, then every triangle
// (a quad as two, as Cinema 4D splits it), both sides (Moeller and Trumbore). Updates `best`.
static maxon::Bool HitMesh(const cinema::PolygonObject* mesh, const cinema::Vector& o, const cinema::Vector& d, maxon::Float64& best)
{
	const cinema::Vector centre = mesh->GetMp();
	const cinema::Vector half = mesh->GetRad();
	maxon::Float64 enter = 0.0;
	maxon::Float64 leave = best;
	for (maxon::Int axis = 0; axis < 3; ++axis)
	{
		const maxon::Float64 low = centre[axis] - half[axis] - 1e-6;
		const maxon::Float64 high = centre[axis] + half[axis] + 1e-6;
		if (maxon::Abs(d[axis]) < 1e-300)
		{
			if (o[axis] < low || o[axis] > high)
				return false;
			continue;
		}
		maxon::Float64 t0 = (low - o[axis]) / d[axis];
		maxon::Float64 t1 = (high - o[axis]) / d[axis];
		if (t0 > t1)
			std::swap(t0, t1);
		enter = maxon::Max(enter, t0);
		leave = maxon::Min(leave, t1);
		if (enter > leave)
			return false;
	}

	const cinema::Vector* const points = mesh->GetPointR();
	const cinema::CPolygon* const polygons = mesh->GetPolygonR();
	const maxon::Int32 pointCount = mesh->GetPointCount();
	if (!points || !polygons)
		return false;
	maxon::Bool hit = false;
	const auto triangle = [&](maxon::Int32 a, maxon::Int32 b, maxon::Int32 c)
	{
		if (a < 0 || b < 0 || c < 0 || a >= pointCount || b >= pointCount || c >= pointCount)
			return;
		const cinema::Vector e1 = points[b] - points[a];
		const cinema::Vector e2 = points[c] - points[a];
		const cinema::Vector p = Cross(d, e2);
		const maxon::Float64 det = Dot(e1, p);
		if (maxon::Abs(det) < 1e-300)
			return;
		const maxon::Float64 inverse = 1.0 / det;
		const cinema::Vector s = o - points[a];
		const maxon::Float64 u = Dot(s, p) * inverse;
		if (u < 0.0 || u > 1.0)
			return;
		const cinema::Vector q = Cross(s, e1);
		const maxon::Float64 v = Dot(d, q) * inverse;
		if (v < 0.0 || u + v > 1.0)
			return;
		const maxon::Float64 t = Dot(e2, q) * inverse;
		if (t > 1e-9 && t < best)
		{
			best = t;
			hit = true;
		}
	};
	const maxon::Int32 polygonCount = mesh->GetPolygonCount();
	for (maxon::Int32 i = 0; i < polygonCount; ++i)
	{
		const cinema::CPolygon& polygon = polygons[i];
		triangle(polygon.a, polygon.b, polygon.c);
		if (polygon.c != polygon.d)
			triangle(polygon.a, polygon.c, polygon.d);
	}
	return hit;
}

maxon::Bool SceneExporter::FindNearestHit(const cinema::Vector& origin, const cinema::Vector& direction, maxon::Float64 length,
	const maxon::Delegate<maxon::Bool(const cinema::BaseObject*)>& accept, maxon::Float64& distance) const
{
	maxon::Float64 best = length;
	maxon::Bool found = false;
	const auto test = [&](const cinema::PolygonObject* mesh, const cinema::Matrix& mg)
	{
		// The ray in the mesh's own axes; its parameter stays the distance along the world ray.
		const cinema::Matrix inverse = ~mg;
		if (HitMesh(mesh, inverse * origin, inverse.sqmat * direction, best))
			found = true;
	};
	for (const Leaf& leaf : _leaves)
	{
		if (leaf.kind != LEAF::MESH || !leaf.visibility.camera)
			continue;
		if (accept && !accept(leaf.ownerObject))
			continue;
		const cinema::PolygonObject* const mesh = static_cast<const cinema::PolygonObject*>(leaf.object);
		if (leaf.instanceSet == NOTOK)
		{
			test(mesh, leaf.mg);
			continue;
		}
		// A multi-instance: the mesh once per copy (the leaf's matrix is relative to a copy).
		for (const cinema::Matrix& copy : *_instanceSets[leaf.instanceSet].matrices)
			test(mesh, copy * leaf.mg);
	}
	distance = best;
	return found;
}

} // namespace cyclesrender
