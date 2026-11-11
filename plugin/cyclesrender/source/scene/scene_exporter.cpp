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
#include "tcompositing.h"
#include "tcyclesobject.h"
#include "ttexture.h"

#include "../ids.h"
#include "../materials/material_export.h"
#include "camera_export.h"
#include "curves_export.h"
#include "lib_hair.h"
#include "environment_export.h"
#include "light_export.h"
#include "mesh_export.h"
#include "parameters.h"
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

static maxon::String KeyId(const maxon::Char* prefix, maxon::UInt64 key)
{
	return maxon::String(prefix) + maxon::String::HexToString(key, false);
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

SceneExporter::SceneExporter(cinema::BaseDocument* doc, const ExportSettings& settings)
	: _doc(doc), _settings(settings), _coords(Coords::FromDocument(doc))
{
}

SceneExporter::~SceneExporter()
{
	if (_ocio)
		DeleteObj(_ocio);
	for (maxon::BaseArray<cinema::Matrix>*& matrices : _ownedMatrices)
		DeleteObj(matrices);
}

maxon::Result<void> SceneExporter::Collect()
{
	iferr_scope;

	if (!_doc)
		return maxon::NullptrError(MAXON_SOURCE_LOCATION);

	for (cinema::BaseObject* op = _doc->GetFirstObject(); op; op = op->GetNext())
	{
		Walk(op, op->GetMl(), Inherited(), maxon::String(), false, op->GetGUID(), false) iferr_return;
	}
	_camera = _settings.camera ? _settings.camera : FindRenderCamera(_doc);
	return maxon::OK;
}

maxon::Result<void> SceneExporter::GetMotionSample(MotionSample& sample) const
{
	iferr_scope;

	for (const Leaf& leaf : _leaves)
	{
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
		sample.camera = _camera->GetMg();
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
	leaf.hasColor = inherited.hasColor;
	leaf.color = inherited.color;
	leaf.instanceSet = inherited.instanceSet;
	leaf.ownerObject = inherited.ownerObject;
	leaf.linkTag = inherited.linkTag;
	leaf.visibility = inherited.visibility;
	_leaves.Append(std::move(leaf)) iferr_return;
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
		forObject.hasColor = false;
		forObject.clones = nullptr;
		forObject.forceVisible = true;   // listed objects are usually hidden themselves
		++forObject.instanceDepth;
		// The object is walked once, with its axis at each copy's place.
		Walk(objects[k], cinema::Matrix(), forObject, path, true, MixKeys(MixKeys(key, objects[k]->GetGUID()), maxon::UInt64(k + 1)), true) iferr_return;
	}
	return true;
}

maxon::Result<void> SceneExporter::Walk(cinema::BaseObject* op, const cinema::Matrix& mg, Inherited inherited, const maxon::String& parentPath,
	maxon::Bool instanceRoot, maxon::UInt64 key, maxon::Bool inCache)
{
	iferr_scope;

	const maxon::String path = parentPath.IsEmpty() ? maxon::String(op->GetName()) : FormatString("@/@", parentPath, op->GetName());

	// Only the object this was set for hands the clone colours to its children.
	const CloneColors* const clones = inherited.clones;
	inherited.clones = nullptr;
	if (!inCache)
	{
		inherited.owner = key;
		inherited.ownerObject = op;
	}

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

	// The object a render instance refers to is drawn where the instance is, whatever hides the
	// original's parents and even when the original is itself the input of a generator. Its own
	// render dot still counts: Cinema 4D renders no instance of an object switched off that way.
	const maxon::Bool consumed = !instanceRoot && op->GetBit(BIT_CONTROLOBJECT);
	const maxon::Bool renders = inherited.visible && layerVisible && !consumed;
	const maxon::Int32 type = op->GetType();

	// What an object generates lives in its cache; a deformed copy, if any, replaces that.
	cinema::BaseObject* cache = nullptr;
	maxon::Bool cacheIsDeformed = false;

	// A particle group or Matrix object whose Cycles Object tag instances objects on it.
	maxon::Bool instanced = false;
	// (A Matrix object carries the "used by a generator" mark itself, as a Hair object does.)
	if ((type == Ofpgroup || type == Omgmatrix) && (type == Omgmatrix ? (inherited.visible && layerVisible) : renders) && inherited.instanceSet == NOTOK)
	{
		instanced = WalkTagInstances(op, mg, inherited, path, key) iferr_return;
	}

	if (instanced)
	{
		// Nothing else of it renders: the particles themselves are replaced by the objects.
	}
	else if (type == Olight || type == Orslight)
	{
		// A Redshift light carries the "used by a generator" mark itself, like a Hair object.
		const maxon::Bool shines = type == Orslight ? (inherited.visible && layerVisible) : renders;
		if (shines && type == Orslight && GetIntParameter(op, REDSHIFT_LIGHT_TYPE, 0) == REDSHIFT_LIGHT_TYPE_DOME)
		{
			// A dome light is the environment, not a lamp.
			if (!_dome)
			{
				_dome = op;
				_domeMg = mg;
			}
		}
		else if (shines && inherited.instanceSet == NOTOK)
		{
			Leaf leaf;
			leaf.kind = LEAF::LIGHT;
			leaf.object = op;
			leaf.mg = mg;
			leaf.path = path;
			leaf.key = key;
			_leaves.Append(std::move(leaf)) iferr_return;
		}
		else if (shines)
		{
			_notes.Append(FormatString("Light \"@\": lights inside a multi-instance are not exported.", op->GetName())) iferr_return;
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
			_leaves.Append(std::move(leaf)) iferr_return;
		}
	}
	else if (type == Osky)
	{
		if (renders && !_sky)
		{
			_sky = op;
			_skyMg = mg;
			_skyMaterial = GetStackMaterial(inherited.stack);
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
			++forReference.instanceDepth;
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
			Walk(cache, cacheIsDeformed ? mg : mg * cache->GetMl(), forCache, path, false, MixKeys(key, cacheIsDeformed ? 1 : 2), true) iferr_return;
		}
		else if (renders && type == Opolygon)
		{
			AddMeshLeaf(op, mg, inherited, path, key, inCache) iferr_return;
		}
	}

	// Children inherit the visibility and material, but not a layer: a layer hides only the
	// objects assigned to it.
	Inherited forChildren = inherited;
	forChildren.layerVisible = inherited.layerVisible;
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
		Walk(child, mg * child->GetMl(), forChild, path, false, inCache ? MixKeys(key, childIndex++) : child->GetGUID(), inCache) iferr_return;
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
	}
	else
	{
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
		const maxon::Bool hasGeometry = ExportMesh(state.writer, _coords, mesh, resolved.meshId, meshMaterials, meshMotion) iferr_return;
		if (!hasGeometry)
			resolved.meshId = maxon::String();
		state.meshIds.Insert(meshKey, resolved.meshId) iferr_return;
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
	}
	return maxon::OK;
}

maxon::Result<maxon::Bool> SceneExporter::GetLightLinking(const Leaf& light, const maxon::BaseArray<maxon::String>& itemIds, LightLinking& linking) const
{
	iferr_scope;

	// The light's own list.
	const cinema::GeData data = GetParameterData(light.object, LIGHT_EXCLUSION_LIST);
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
	linking.include = !listed.IsEmpty() && GetIntParameter(light.object, LIGHT_EXCLUSION_MODE, LIGHT_EXCLUSION_MODE_EXCLUDE) == LIGHT_EXCLUSION_MODE_INCLUDE;
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
	json.EndObject();
}

static void WriteMaterialIds(JsonWriter& json, const maxon::BaseArray<maxon::String>& materialIds)
{
	json.BeginArray("materials");
	for (const maxon::String& materialId : materialIds)
		json.String(nullptr, materialId);
	json.EndArray();
}

maxon::Result<void> SceneExporter::WriteObject(SceneWriter& writer, const Leaf& leaf, const Resolved& resolved)
{
	maxon::Float64 matrix[16];
	_coords.ObjectMatrix(leaf.mg, matrix);
	JsonWriter& json = writer.BeginItem("object", KeyId("object_", leaf.key));
	json.String("mesh", resolved.meshId);
	json.Floats("matrix", matrix, 16);
	if (_motionOpen && _motionClose)
	{
		const auto* const open = _motionOpen->matrices.Find(leaf.key);
		const auto* const close = _motionClose->matrices.Find(leaf.key);
		if (open && close && (open->GetValue() != leaf.mg || close->GetValue() != leaf.mg))
		{
			maxon::Float64 motion[32];
			_coords.ObjectMatrix(open->GetValue(), motion);
			_coords.ObjectMatrix(close->GetValue(), motion + 16);
			json.Floats("motion", motion, 32);
		}
	}
	WriteMaterialIds(json, resolved.materialIds);
	WriteVisibility(json, leaf.visibility);
	writer.EndItem();
	return maxon::OK;
}

maxon::Result<void> SceneExporter::WriteInstancer(SceneWriter& writer, const maxon::String& id, const Resolved& resolved,
	const maxon::BaseArray<cinema::Matrix>& matrices, const maxon::BaseArray<cinema::Vector>* colors, const Visibility& visibility)
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
			cinema::GeData management;
			if (_doc->GetParameter(ConstDescIDLevel(cinema::DOCUMENT_COLOR_MANAGEMENT), management, cinema::DESCFLAGS_GET::NONE) &&
				management.GetInt32() == cinema::DOCUMENT_COLOR_MANAGEMENT_OCIO)
			{
				iferr (_ocio = cinema::OcioConverter::Init(_doc, nullptr))
					_ocio = nullptr;
			}
		}
		if (_ocio)
		{
			_ocio->TransformColors(converted.GetFirst(), count, cinema::COLORSPACETRANSFORMATION::OCIO_SRGB_TO_RENDERING);
		}
		else
		{
			// Without OCIO the render space is linear sRGB: only the transfer curve is undone.
			for (cinema::Vector32& color : converted)
			{
				for (maxon::Int channel = 0; channel < 3; ++channel)
				{
					const maxon::Float32 value = color[channel];
					color[channel] = value <= 0.04045f ? value / 12.92f : maxon::Pow((value + 0.055f) / 1.055f, 2.4f);
				}
			}
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

	JsonWriter& json = writer.BeginItem("instancer", id);
	json.String("mesh", resolved.meshId);
	WriteMaterialIds(json, resolved.materialIds);
	json.Int("matrices", matricesBlob);
	if (colorsBlob != NOTOK)
		json.Int("colors", colorsBlob);
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
			ExportHair(writer, _coords, _doc, leaf.object, KeyId("hair_", leaf.key)) iferr_return;
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
			continue;
		}

		Resolved resolved;
		Resolve(leaf, state, resolved) iferr_return;
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
		matrices.EnsureCapacity(group.count) iferr_return;
		colors.EnsureCapacity(group.count) iferr_return;
		for (maxon::Int index : group.leaves)
		{
			const Leaf& leaf = _leaves[index];
			if (leaf.instanceSet == NOTOK)
			{
				matrices.Append(leaf.mg) iferr_return;
				colors.Append(leaf.hasColor ? leaf.color : cinema::Vector(0.5)) iferr_return;
				continue;
			}
			// A multi-instance: the mesh once per copy. The leaf's matrix is relative to a copy.
			const InstanceSet& set = _instanceSets[leaf.instanceSet];
			for (maxon::Int i = 0; i < set.matrices->GetCount(); ++i)
			{
				matrices.Append((*set.matrices)[i] * leaf.mg) iferr_return;
				colors.Append(set.colors ? cinema::Vector((*set.colors)[i].r, (*set.colors)[i].g, (*set.colors)[i].b) : cinema::Vector(0.5)) iferr_return;
			}
		}
		const maxon::String instancerId = KeyId("instancer_", _leaves[group.leaves[0]].key);
		WriteInstancer(writer, instancerId, group.resolved, matrices, group.hasColor ? &colors : nullptr, _leaves[group.leaves[0]].visibility) iferr_return;
		for (maxon::Int index : group.leaves)
			itemIds[index] = instancerId;
	}

	for (const Leaf& leaf : _leaves)
	{
		if (leaf.kind != LEAF::LIGHT)
			continue;
		LightLinking linking;
		const maxon::Bool linked = GetLightLinking(leaf, itemIds, linking) iferr_return;
		ExportLight(writer, _coords, leaf.object, leaf.mg, KeyId("light_", leaf.key), _settings.lightMultiplier, linked ? &linking : nullptr) iferr_return;
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
		ExportCamera(writer, _coords, camera, _settings.depthOfField, CameraFraming::Full(_settings.width, _settings.height), moves ? cameraMotion : nullptr) iferr_return;
	}
	// A Sky object comes first; then a Redshift dome light; then the plain world of the render settings.
	if (!_sky && _dome)
	{
		ExportDomeWorld(state.materials, _dome, _domeMg) iferr_return;
	}
	else
	{
		ExportWorld(state.materials, _sky, _skyMg, _skyMaterial, _settings.worldColor, _settings.worldStrength) iferr_return;
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
		const cinema::PolygonObject* const mesh = static_cast<const cinema::PolygonObject*>(leaf.object);
		const cinema::BaseMaterial* const material = GetStackMaterial(leaf.stack);
		maxon::String extra;
		if (leaf.instanceSet != NOTOK)
			extra = FormatString(" instances=@", _instanceSets[leaf.instanceSet].matrices->GetCount());
		text += FormatString("mesh \"@\" points=@ polygons=@ pos=@ size=@ material=\"@\"@\n", leaf.path, mesh->GetPointCount(), mesh->GetPolygonCount(),
			VectorText(leaf.mg.off), VectorText(mesh->GetRad() * 2.0), material ? material->GetName() : maxon::String(), extra);
	}
	if (_sky)
		text += FormatString("sky \"@\" material=\"@\"\n", _sky->GetName(), _skyMaterial ? _skyMaterial->GetName() : maxon::String());
	if (_camera)
		text += FormatString("camera \"@\" pos=@\n", _camera->GetName(), VectorText(_camera->GetMg().off));
	for (const maxon::String& note : _notes)
		text += FormatString("note @\n", note);
	return text;
}

} // namespace cyclesrender
