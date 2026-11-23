// SPDX-License-Identifier: Apache-2.0
// The Cycles Scatter object (scatter_object.h). Reading order: Init (the defaults), what is greyed, when the
// copies are computed again (GetVirtualObjects), then the viewport (Draw, GetDimension); last, the copies as
// they are now for the exporter (GetCurrentScatterCopies).

#include "c4d.h"
#include "c4d_objectdata.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "customgui_field.h"
#include "customgui_inexclude.h"
#include "onull.h"

#include "ocyclesscatter.h"

#include "../ids.h"
#include "../scene/parameters.h"
#include "../scene/proxy_export.h"
#include "scatter_object.h"

namespace cyclesrender
{

maxon::Bool IsScatterSwitchedOn(const cinema::BaseObject* scatter)
{
	return scatter && scatter->GetDeformMode();
}

maxon::Bool IsNotScattered(const cinema::BaseObject* op)
{
	return op && (op->IsInstanceOf(Olight) || op->GetType() == Orslight || op->GetType() == ID_LIGHT_OBJECT || op->IsInstanceOf(Ocamera) ||
		op->GetType() == Orscamera);
}

// The bounds of the polygon objects in @p op's hierarchy, in the axes @p mg leads to.
static void GrowBounds(const cinema::BaseObject* op, const cinema::Matrix& mg, cinema::Vector& low, cinema::Vector& high, maxon::Bool& any)
{
	for (; op; op = op->GetNext())
	{
		const cinema::Matrix m = mg * op->GetMl();
		if (op->GetType() == Opolygon)
		{
			const cinema::PolygonObject* const mesh = static_cast<const cinema::PolygonObject*>(op);
			const cinema::Vector* const points = mesh->GetPointR();
			for (maxon::Int32 i = 0; points && i < mesh->GetPointCount(); ++i)
			{
				const cinema::Vector p = m * points[i];
				low = any ? cinema::Vector(maxon::Min(low.x, p.x), maxon::Min(low.y, p.y), maxon::Min(low.z, p.z)) : p;
				high = any ? cinema::Vector(maxon::Max(high.x, p.x), maxon::Max(high.y, p.y), maxon::Max(high.z, p.z)) : p;
				any = true;
			}
		}
		GrowBounds(op->GetDown(), m, low, high, any);
	}
}

class ScatterObject : public cinema::ObjectData
{
	INSTANCEOF(ScatterObject, cinema::ObjectData)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(ScatterObject); }

	~ScatterObject()
	{
		cinema::BaseObject::Free(_sourceRoot);
		FreeProxyCopies();
	}

	const ScatterResult* GetResult() const { return _built ? &_result : nullptr; }

	virtual cinema::Bool Init(cinema::GeListNode* node, cinema::Bool isCloneInit) override
	{
		cinema::BaseList2D* const item = static_cast<cinema::BaseList2D*>(node);
		if (!item)
			return false;
		if (isCloneInit)
			return true;
		cinema::BaseContainer& data = item->GetDataInstanceRef();
		data.SetInt32(CYCLESSCATTER_MODE, CYCLESSCATTER_MODE_SURFACE);
		data.SetInt32(CYCLESSCATTER_COUNT, 1000);
		data.SetInt32(CYCLESSCATTER_SEED, 12345);
		data.SetFloat(CYCLESSCATTER_POISSON_RADIUS, 20.0);
		data.SetInt32(CYCLESSCATTER_POISSON_SAMPLES, 500);
		data.SetInt32(CYCLESSCATTER_CHILD_ORDER, CYCLESSCATTER_CHILD_ORDER_RANDOM);
		data.SetFloat(CYCLESSCATTER_KEEP_AWAY, 0.0);
		data.SetBool(CYCLESSCATTER_REMOVE_COPIES, false);
		data.SetBool(CYCLESSCATTER_FILE_SEQUENCE, false);
		data.SetInt32(CYCLESSCATTER_FILE_FRAME_OFFSET, 0);
		// Upright, as grass, trees and rocks stand: the copies' Y is the up vector until Normal Align is raised.
		data.SetFloat(CYCLESSCATTER_NORMAL_ALIGN, 0.0);
		data.SetVector(CYCLESSCATTER_UP_VECTOR, cinema::Vector(0.0, 1.0, 0.0));
		data.SetFloat(CYCLESSCATTER_VERTEX_MAP_LIMIT, 0.0);
		data.SetFloat(CYCLESSCATTER_DENSITY_MIN, 0.0);
		data.SetFloat(CYCLESSCATTER_DENSITY_MAX, 1.0);
		data.SetFloat(CYCLESSCATTER_NORMAL_THRESHOLD, maxon::PI);
		data.SetFloat(CYCLESSCATTER_POSITION_DISTANCE, 100.0);
		data.SetFloat(CYCLESSCATTER_SCALE_MIN, 0.5);
		data.SetFloat(CYCLESSCATTER_SCALE_MAX, 1.5);
		data.SetFloat(CYCLESSCATTER_ROTATION_ANGLE, maxon::PI05);
		data.SetData(CYCLESSCATTER_EFFECTORS, cinema::GeData(CUSTOMDATATYPE_INEXCLUDE_LIST, cinema::DEFAULTVALUE));
		data.SetData(CYCLESSCATTER_FIELDS, cinema::GeData(CUSTOMDATATYPE_FIELDLIST, cinema::DEFAULTVALUE));
		data.SetInt32(CYCLESSCATTER_FIELDS_AFFECT, CYCLESSCATTER_FIELDS_AFFECT_DENSITY);
		data.SetInt32(CYCLESSCATTER_DISPLAY, CYCLESSCATTER_DISPLAY_POINTS);
		data.SetFloat(CYCLESSCATTER_DISPLAY_RATE, 1.0);
		data.SetInt32(CYCLESSCATTER_DISPLAY_COLOR_MODE, CYCLESSCATTER_DISPLAY_COLOR_COPY);
		data.SetVector(CYCLESSCATTER_DISPLAY_COLOR, cinema::Vector(0.45, 0.8, 0.4));
		return true;
	}

	// What a distribution does not read is greyed: the Count but for Surface, the Poisson's settings but for it,
	// the file but for the CSV mode (where the surface and everything placed on it are greyed), a shader's range
	// while it has none, the display's settings while it draws nothing.
	virtual cinema::Bool GetDEnabling(const cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& t_data,
		cinema::DESCFLAGS_ENABLE flags, const cinema::BaseContainer* itemdesc) const override
	{
		const cinema::BaseList2D* const item = static_cast<const cinema::BaseList2D*>(node);
		const maxon::Int32 parameter = id.GetDepth() > 0 ? id[0].id : 0;
		if (!item)
			return SUPER::GetDEnabling(node, id, t_data, flags, itemdesc);
		const cinema::BaseContainer& data = item->GetDataInstanceRef();
		const cinema::BaseDocument* const doc = item->GetDocument();
		const maxon::Int32 mode = data.GetInt32(CYCLESSCATTER_MODE);
		const maxon::Bool file = mode == CYCLESSCATTER_MODE_FILE;
		const auto linked = [&data, doc](maxon::Int32 link) { return data.GetLink(link, doc) != nullptr; };
		// (The spacing radius counts for Vertices, Surface and Poisson: greyed only for a file, below.)
		switch (parameter)
		{
			case CYCLESSCATTER_COUNT: return mode == CYCLESSCATTER_MODE_SURFACE;
			case CYCLESSCATTER_POISSON_RADIUS:
			case CYCLESSCATTER_POISSON_SAMPLES: return mode == CYCLESSCATTER_MODE_POISSON;
			case CYCLESSCATTER_FILE:
			case CYCLESSCATTER_FILE_SEQUENCE: return file;
			case CYCLESSCATTER_FILE_FRAME_OFFSET: return file && data.GetBool(CYCLESSCATTER_FILE_SEQUENCE);
			case CYCLESSCATTER_REMOVE_COPIES: return mode == CYCLESSCATTER_MODE_SURFACE;
			case CYCLESSCATTER_SURFACE:
			case CYCLESSCATTER_KEEP_AWAY:
			case CYCLESSCATTER_NORMAL_ALIGN:
			case CYCLESSCATTER_UP_VECTOR:
			case CYCLESSCATTER_VERTEX_MAP:
			case CYCLESSCATTER_DENSITY_SHADER:
			case CYCLESSCATTER_NORMAL_THRESHOLD:
			case CYCLESSCATTER_POSITION_SHADER:
			case CYCLESSCATTER_SCALE_SHADER:
			case CYCLESSCATTER_ROTATION_SHADER: return !file;
			case CYCLESSCATTER_VERTEX_MAP_LIMIT: return !file && linked(CYCLESSCATTER_VERTEX_MAP);
			case CYCLESSCATTER_DENSITY_MIN:
			case CYCLESSCATTER_DENSITY_MAX: return !file && linked(CYCLESSCATTER_DENSITY_SHADER);
			case CYCLESSCATTER_POSITION_DISTANCE: return !file && linked(CYCLESSCATTER_POSITION_SHADER);
			case CYCLESSCATTER_SCALE_MIN:
			case CYCLESSCATTER_SCALE_MAX: return !file && linked(CYCLESSCATTER_SCALE_SHADER);
			case CYCLESSCATTER_ROTATION_ANGLE: return !file && linked(CYCLESSCATTER_ROTATION_SHADER);
			case CYCLESSCATTER_DISPLAY_RATE:
			case CYCLESSCATTER_DISPLAY_COLOR_MODE: return data.GetInt32(CYCLESSCATTER_DISPLAY) != CYCLESSCATTER_DISPLAY_NONE;
			case CYCLESSCATTER_DISPLAY_COLOR:
				return data.GetInt32(CYCLESSCATTER_DISPLAY) != CYCLESSCATTER_DISPLAY_NONE && data.GetInt32(CYCLESSCATTER_DISPLAY_COLOR_MODE) == CYCLESSCATTER_DISPLAY_COLOR_CUSTOM;
			default: break;
		}
		return SUPER::GetDEnabling(node, id, t_data, flags, itemdesc);
	}

	virtual cinema::BaseObject* GetVirtualObjects(cinema::BaseObject* op, const cinema::HierarchyHelp* hh) override
	{
		// The children as polygon objects, cloned only when one of them changed (which also marks them as the
		// generator's input, so that Cinema 4D shows them no more): one entry per child.
		cinema::BaseObject* const first = op->GetDown();
		cinema::Bool childrenDirty = false;
		maxon::BaseArray<cinema::BaseObject*> clones;
		cinema::BaseObject* const hierarchy = first ? op->GetAndCheckHierarchyClone(hh, first, cinema::HIERARCHYCLONEFLAGS::ASPOLY, &childrenDirty, nullptr,
			maxon::LIMIT<maxon::Int>::MAX, &clones, false) : nullptr;
		++_result.evaluations;
		// What the copies follow from outside the object (the surface, the effectors, ...). Looked at here: Cinema 4D
		// calls GetVirtualObjects at every evaluation, but never called CheckDirty for this object (measured: 0 calls
		// in 7 evaluations, 2027). The object does not implement GetAccessedObjects: with it, Cinema 4D called
		// GetVirtualObjects only when what it declared changed, and a surface below the scatter in the Object Manager,
		// evaluated after it, then never reached it (docs/reference/c4d-sdk.md, "The Cycles Scatter object").
		const maxon::UInt64 dependencies = ScatterDependencies(op, hh->GetDocument());
		// (The last child removed: nothing else may say so.)
		const maxon::Bool dirty = childrenDirty || (first == nullptr) != _cloneOfChild.IsEmpty() || op->CheckCache(hh) || op->IsDirty(cinema::DIRTYFLAGS::DATA) ||
			dependencies != _builtDependencies || !_built;
		if (!dirty)
			return hierarchy ? hierarchy : op->GetCache();
		_builtDependencies = dependencies;

		iferr (Build(op, hh, first, childrenDirty ? hierarchy : nullptr, clones))
		{
			_result.warnings.Reset();
			_result.warnings.Append(FormatString("its copies could not be computed: @", err.GetMessage())) iferr_ignore("Without the line the console says nothing.");
		}
		// The cache: an empty Null that draws nothing. The copies are no geometry in Cinema 4D (the viewport draws
		// them in Draw), and a new cache each time they are computed again moves the object's cache counter, which
		// is how the IPR sees that they changed.
		cinema::BaseObject* const null = cinema::BaseObject::Alloc(Onull);
		if (null)
			null->GetDataInstanceRef().SetInt32(NULLOBJECT_DISPLAY, NULLOBJECT_DISPLAY_NONE);
		return null;
	}

	virtual void GetDimension(const cinema::BaseObject* op, cinema::Vector* mp, cinema::Vector* rad) const override
	{
		*mp = (_boundsLow + _boundsHigh) * 0.5;
		*rad = (_boundsHigh - _boundsLow) * 0.5;
	}

	virtual cinema::DRAWRESULT Draw(cinema::BaseObject* op, cinema::DRAWPASS drawpass, cinema::BaseDraw* bd, cinema::BaseDrawHelp* bh) override
	{
		if (drawpass != cinema::DRAWPASS::OBJECT)
			return SUPER::Draw(op, drawpass, bd, bh);
		const maxon::Int32 display = GetIntParameter(op, CYCLESSCATTER_DISPLAY, CYCLESSCATTER_DISPLAY_POINTS);
		if (display == CYCLESSCATTER_DISPLAY_NONE || _drawCopies.IsEmpty() || !IsScatterSwitchedOn(op))
			return cinema::DRAWRESULT::SKIP;
		const maxon::Bool custom = GetIntParameter(op, CYCLESSCATTER_DISPLAY_COLOR_MODE, CYCLESSCATTER_DISPLAY_COLOR_COPY) == CYCLESSCATTER_DISPLAY_COLOR_CUSTOM;
		const cinema::Vector customColor = GetVectorParameter(op, CYCLESSCATTER_DISPLAY_COLOR, cinema::Vector(0.45, 0.8, 0.4));
		bd->SetMatrix_Matrix(op, bh->GetMg());
		if (display == CYCLESSCATTER_DISPLAY_POINTS)
		{
			bd->SetPen(customColor, SET_PEN_USE_PROFILE_COLOR);
			bd->SetPointSize(3.0);
			bd->DrawPointArray(maxon::Int32(_drawPoints.GetCount()), _drawPoints.GetFirst(), custom ? nullptr : _drawColors.GetFirst(), custom ? 0 : 3, nullptr);
			return cinema::DRAWRESULT::OK;
		}
		// Boxes: the bounds of the copy's source, turned and scaled as the copy; twelve lines each.
		for (const maxon::Int index : _drawCopies)
		{
			const cinema::Matrix& m = _result.copies.matrices[index];
			const maxon::Int32 source = _result.copies.sources[index];
			if (source < 0 || source >= _result.sources.GetCount())
				continue;
			const ScatterSource& box = _result.sources[source];
			bd->SetPen(custom ? customColor : ClampColor(_result.copies.colors[index]), SET_PEN_USE_PROFILE_COLOR);
			cinema::Vector corner[8];
			for (maxon::Int c = 0; c < 8; ++c)
			{
				const cinema::Vector sign((c & 1) ? 1.0 : -1.0, (c & 2) ? 1.0 : -1.0, (c & 4) ? 1.0 : -1.0);
				corner[c] = m * (box.boxCenter + cinema::Vector(sign.x * box.boxRadius.x, sign.y * box.boxRadius.y, sign.z * box.boxRadius.z));
			}
			for (maxon::Int c = 0; c < 8; ++c)
			{
				for (maxon::Int bit = 1; bit < 8; bit <<= 1)
				{
					if ((c & bit) == 0)
						bd->DrawLine(corner[c], corner[c | bit], 0);
				}
			}
		}
		return cinema::DRAWRESULT::OK;
	}

private:
	static cinema::Vector ClampColor(const cinema::Vector& c)
	{
		return cinema::Vector(maxon::Clamp01(c.x), maxon::Clamp01(c.y), maxon::Clamp01(c.z));
	}

	// Computes everything again. @p hierarchy: a new clone of the children (a Null holding one clone per child,
	// @p clones), which the object keeps; null: the children did not change, and the clones made before serve,
	// unless there are none yet (a copy of the object, as in a render document), when they are made here.
	maxon::Result<void> Build(cinema::BaseObject* op, const cinema::HierarchyHelp* hh, cinema::BaseObject* first, cinema::BaseObject* hierarchy,
		const maxon::BaseArray<cinema::BaseObject*>& clones)
	{
		iferr_scope;
		maxon::Int childCount = 0;
		for (const cinema::BaseObject* child = first; child; child = child->GetNext())
			++childCount;

		if (hierarchy)
		{
			cinema::BaseObject::Free(_sourceRoot);
			_sourceRoot = hierarchy;
			_cloneOfChild.Reset();
			for (cinema::BaseObject* const clone : clones)
			{
				_cloneOfChild.Append(clone) iferr_return;
			}
		}
		else if (!first)
		{
			cinema::BaseObject::Free(_sourceRoot);
			_cloneOfChild.Reset();
		}
		else if (!_sourceRoot || _cloneOfChild.GetCount() != childCount)
		{
			cinema::BaseObject::Free(_sourceRoot);
			_cloneOfChild.Reset();
			_sourceRoot = cinema::BaseObject::Alloc(Onull);
			if (!_sourceRoot)
				return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
			for (cinema::BaseObject* child = first; child; child = child->GetNext())
			{
				cinema::BaseObject* const clone = op->GetHierarchyClone(hh, child, cinema::HIERARCHYCLONEFLAGS::ASPOLY, nullptr, nullptr);
				if (clone)
					clone->InsertUnderLast(_sourceRoot);
				_cloneOfChild.Append(clone) iferr_return;
			}
		}

		// The sources: the children but lights and cameras, each with its clone.
		FreeProxyCopies();
		const maxon::Int evaluations = _result.evaluations;
		const maxon::Int builds = _result.builds;
		_result = ScatterResult();
		_result.evaluations = evaluations;
		_result.builds = builds + 1;
		maxon::Int position = 0;
		for (const cinema::BaseObject* child = first; child; child = child->GetNext(), ++position)
		{
			if (IsNotScattered(child))
			{
				_result.skipped.Append(maxon::String(child->GetName())) iferr_return;
				continue;
			}
			const cinema::BaseObject* clone = position < _cloneOfChild.GetCount() ? _cloneOfChild[position] : nullptr;
			if (!clone && child->GetType() == ID_PROXY_OBJECT)
			{
				// A Cycles Proxy has no polygons to clone (measured: no clone comes back for it): a plain copy of
				// the object stands for it, from which the exporter reads its file and what it shows.
				cinema::BaseObject* copy = static_cast<cinema::BaseObject*>(child->GetClone(cinema::COPYFLAGS::NO_HIERARCHY, nullptr));
				if (copy)
				{
					iferr (_proxyCopies.Append(copy))
					{
						cinema::BaseObject::Free(copy);
						return err;
					}
				}
				clone = copy;
			}
			if (!clone)
				continue;
			ScatterSource source;
			source.clone = clone;
			source.guid = child->GetGUID();
			source.name = child->GetName();
			cinema::Vector low(0.0);
			cinema::Vector high(0.0);
			maxon::Bool any = false;
			// The clone's own matrix is the child's place below the scatter, which a copy replaces.
			if (clone->GetType() == ID_PROXY_OBJECT)
			{
				// The box a proxy is drawn as (scene/proxy_export.h).
				cinema::Vector centre;
				cinema::Vector radius;
				GetProxyBox(clone, hh->GetDocument(), centre, radius);
				low = centre - radius;
				high = centre + radius;
				any = true;
			}
			else if (clone->GetType() == Opolygon)
			{
				cinema::Vector cloneLow(0.0);
				cinema::Vector cloneHigh(0.0);
				const cinema::PolygonObject* const mesh = static_cast<const cinema::PolygonObject*>(clone);
				const cinema::Vector* const points = mesh->GetPointR();
				for (maxon::Int32 i = 0; points && i < mesh->GetPointCount(); ++i)
				{
					cloneLow = any ? cinema::Vector(maxon::Min(cloneLow.x, points[i].x), maxon::Min(cloneLow.y, points[i].y), maxon::Min(cloneLow.z, points[i].z)) : points[i];
					cloneHigh = any ? cinema::Vector(maxon::Max(cloneHigh.x, points[i].x), maxon::Max(cloneHigh.y, points[i].y), maxon::Max(cloneHigh.z, points[i].z)) : points[i];
					any = true;
				}
				low = cloneLow;
				high = cloneHigh;
			}
			GrowBounds(clone->GetDown(), cinema::Matrix(), low, high, any);
			source.boxCenter = (low + high) * 0.5;
			source.boxRadius = (high - low) * 0.5;
			_result.sources.Append(std::move(source)) iferr_return;
		}

		ComputeScatter(op, hh->GetDocument(), _result.sources.GetCount(), hh->GetThread(), _result.copies, _result.stats, _result.warnings) iferr_return;
		_result.dependencies = _builtDependencies;
		_built = true;
		PrepareDrawing(op) iferr_return;
		return maxon::OK;
	}

	// What Draw needs: the copies drawn (the display rate's share, spread evenly over all of them: copy i is
	// drawn when the fraction of i times the golden ratio is below the rate), their places and colours; and
	// the bounds.
	maxon::Result<void> PrepareDrawing(const cinema::BaseObject* op)
	{
		iferr_scope;
		_drawCopies.Reset();
		_drawPoints.Reset();
		_drawColors.Reset();
		const maxon::Float64 rate = maxon::Clamp01(GetFloatParameter(op, CYCLESSCATTER_DISPLAY_RATE, 1.0));
		const ScatterCopies& copies = _result.copies;
		maxon::Float64 radius = 0.0;
		for (const ScatterSource& source : _result.sources)
			radius = maxon::Max(radius, source.boxRadius.GetLength());
		_boundsLow = _boundsHigh = cinema::Vector(0.0);
		for (maxon::Int i = 0; i < copies.matrices.GetCount(); ++i)
		{
			const cinema::Vector p = copies.matrices[i].off;
			const maxon::Float64 size = radius * maxon::Max(copies.matrices[i].sqmat.v1.GetLength(), maxon::Max(copies.matrices[i].sqmat.v2.GetLength(), copies.matrices[i].sqmat.v3.GetLength()));
			const cinema::Vector low = p - cinema::Vector(size);
			const cinema::Vector high = p + cinema::Vector(size);
			_boundsLow = i == 0 ? low : cinema::Vector(maxon::Min(_boundsLow.x, low.x), maxon::Min(_boundsLow.y, low.y), maxon::Min(_boundsLow.z, low.z));
			_boundsHigh = i == 0 ? high : cinema::Vector(maxon::Max(_boundsHigh.x, high.x), maxon::Max(_boundsHigh.y, high.y), maxon::Max(_boundsHigh.z, high.z));
			const maxon::Float64 spread = maxon::Float64(i) * 0.6180339887498949;
			if (rate < 1.0 && spread - maxon::Floor(spread) >= rate)
				continue;
			_drawCopies.Append(i) iferr_return;
			_drawPoints.Append(cinema::Vector32(p)) iferr_return;
			const cinema::Vector c = ClampColor(copies.colors[i]);
			_drawColors.Append(maxon::Float32(c.x)) iferr_return;
			_drawColors.Append(maxon::Float32(c.y)) iferr_return;
			_drawColors.Append(maxon::Float32(c.z)) iferr_return;
		}
		return maxon::OK;
	}

	void FreeProxyCopies()
	{
		for (cinema::BaseObject*& copy : _proxyCopies)
			cinema::BaseObject::Free(copy);
		_proxyCopies.Reset();
	}

	cinema::BaseObject* _sourceRoot = nullptr;                  ///< the clones of the children (owned)
	maxon::BaseArray<cinema::BaseObject*> _proxyCopies;         ///< the copies that stand for Cycles Proxy children (owned; made anew at every Build)
	maxon::BaseArray<cinema::BaseObject*> _cloneOfChild;        ///< per child, its clone in _sourceRoot (null: none)
	ScatterResult _result;
	maxon::BaseArray<maxon::Int> _drawCopies;
	maxon::BaseArray<cinema::Vector32> _drawPoints;
	maxon::BaseArray<maxon::Float32> _drawColors;
	cinema::Vector _boundsLow = cinema::Vector(0.0);
	cinema::Vector _boundsHigh = cinema::Vector(0.0);
	maxon::UInt64 _builtDependencies = 0;     ///< what the copies were computed from
	maxon::Bool _built = false;
};

const ScatterResult* GetScatterResult(const cinema::BaseObject* scatter)
{
	if (!scatter || scatter->GetType() != ID_SCATTER_OBJECT)
		return nullptr;
	const ScatterObject* const data = scatter->GetNodeData<ScatterObject>();
	return data ? data->GetResult() : nullptr;
}

maxon::Result<const ScatterCopies*> GetCurrentScatterCopies(cinema::BaseObject* scatter, const cinema::BaseDocument* doc, ScatterCopies& fresh,
	maxon::BaseArray<maxon::String>& warnings)
{
	iferr_scope;
	const ScatterResult* const result = GetScatterResult(scatter);
	if (!result)
		return nullptr;
	if (ScatterDependencies(scatter, doc) == result->dependencies)
		return &result->copies;
	ScatterStats stats;
	ComputeScatter(scatter, doc, result->sources.GetCount(), nullptr, fresh, stats, warnings) iferr_return;
	return &fresh;
}

cinema::Bool RegisterScatterObject()
{
	// An icon of its own (tools/make_icons.py, scatter_object): the plugin's hexagon with strewn stones.
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("scatter_object.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	// A generator whose children are its input, as MoGraph's Cloner: they are hidden and it has the green check.
	const cinema::Bool registered = cinema::RegisterObjectPlugin(ID_SCATTER_OBJECT, cinema::GeLoadString(IDS_SCATTER_OBJECT_NAME), OBJECT_GENERATOR | OBJECT_INPUT,
		ScatterObject::Alloc, "Ocyclesscatter"_s, icon, 0, cinema::OBJECTCATEGORY::GENERATOR);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
