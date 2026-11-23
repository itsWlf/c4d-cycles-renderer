// SPDX-License-Identifier: Apache-2.0
// The Cycles Fog object (fog_object.h). Reading order: Init (the defaults), the Attribute Manager
// (the noise types, what is greyed), the box in the viewport, then the three handles of its size.

#include "c4d.h"
#include "c4d_objectdata.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "lib_noise.h"

#include "ocyclesfog.h"

#include "../ids.h"
#include "../scene/fog_export.h"
#include "fog_object.h"

namespace cyclesrender
{

class FogObject : public cinema::ObjectData
{
	INSTANCEOF(FogObject, cinema::ObjectData)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(FogObject); }

	virtual cinema::Bool Init(cinema::GeListNode* node, cinema::Bool isCloneInit) override
	{
		cinema::BaseList2D* const item = static_cast<cinema::BaseList2D*>(node);
		if (!item)
			return false;
		if (isCloneInit)
			return true;
		// Blender's Principled Volume as it is added, but for the density (fog_export.h,
		// FOG_DEFAULT_DENSITY), and Cinema 4D's Cube's size.
		cinema::BaseContainer& data = item->GetDataInstanceRef();
		data.SetVector(CYCLESFOG_SIZE, cinema::Vector(FOG_DEFAULT_SIZE));
		data.SetVector(CYCLESFOG_COLOR, cinema::Vector(0.5));
		data.SetFloat(CYCLESFOG_DENSITY, FOG_DEFAULT_DENSITY);
		data.SetFloat(CYCLESFOG_ANISOTROPY, 0.0);
		data.SetVector(CYCLESFOG_ABSORPTION_COLOR, cinema::Vector(0.0));
		data.SetFloat(CYCLESFOG_EMISSION_STRENGTH, 0.0);
		data.SetVector(CYCLESFOG_EMISSION_COLOR, cinema::Vector(1.0));
		data.SetFloat(CYCLESFOG_BLACKBODY_INTENSITY, 0.0);
		data.SetVector(CYCLESFOG_BLACKBODY_TINT, cinema::Vector(1.0));
		data.SetFloat(CYCLESFOG_TEMPERATURE, 1000.0);
		// The noise: off, with the Noise shader's defaults where it has the setting (type Noise,
		// octaves 5, seed 665, no clip, brightness or contrast, standing still), a scale of 1 m and a
		// grid of 64 cells along the longest edge (fog_export.h, FOG_DEFAULT_NOISE_RESOLUTION).
		data.SetBool(CYCLESFOG_NOISE_ENABLE, false);
		data.SetInt32(CYCLESFOG_NOISE_TYPE, cinema::NOISE_NOISE);
		data.SetInt32(CYCLESFOG_NOISE_SPACE, CYCLESFOG_NOISE_SPACE_OBJECT);
		data.SetFloat(CYCLESFOG_NOISE_SCALE, 100.0);
		data.SetVector(CYCLESFOG_NOISE_RELATIVE_SCALE, cinema::Vector(1.0));
		data.SetFloat(CYCLESFOG_NOISE_OCTAVES, 5.0);
		data.SetInt32(CYCLESFOG_NOISE_SEED, 665);
		data.SetBool(CYCLESFOG_NOISE_ABSOLUTE, false);
		data.SetFloat(CYCLESFOG_NOISE_LOW_CLIP, 0.0);
		data.SetFloat(CYCLESFOG_NOISE_HIGH_CLIP, 1.0);
		data.SetFloat(CYCLESFOG_NOISE_BRIGHTNESS, 0.0);
		data.SetFloat(CYCLESFOG_NOISE_CONTRAST, 0.0);
		data.SetFloat(CYCLESFOG_NOISE_SPEED, 0.0);
		data.SetInt32(CYCLESFOG_NOISE_RESOLUTION, FOG_DEFAULT_NOISE_RESOLUTION);
		return true;
	}

	// The noise types are Cinema 4D's own menu of them, as the Noise shader shows it.
	virtual cinema::Bool GetDDescription(const cinema::GeListNode* node, cinema::Description* description, cinema::DESCFLAGS_DESC& flags) const override
	{
		if (!node || !description || !description->LoadDescription(node->GetType()))
			return false;
		flags |= cinema::DESCFLAGS_DESC::LOADED;
		if (cinema::BaseContainer* const parameter = description->GetParameterI(CreateDescID(cinema::DescLevel(CYCLESFOG_NOISE_TYPE)), nullptr))
			parameter->SetContainer(cinema::DESC_CYCLE, cinema::C4DNoise::CreateMenuContainer(false));
		return SUPER::GetDDescription(node, description, flags);
	}

	// The noise's settings are greyed while it is off.
	virtual cinema::Bool GetDEnabling(const cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data,
		cinema::DESCFLAGS_ENABLE flags, const cinema::BaseContainer* itemdesc) const override
	{
		const cinema::BaseList2D* const item = static_cast<const cinema::BaseList2D*>(node);
		const maxon::Int32 parameter = id.GetDepth() > 0 ? id[0].id : 0;
		if (item && parameter > CYCLESFOG_NOISE_ENABLE && parameter <= CYCLESFOG_NOISE_RESOLUTION && !item->GetDataInstanceRef().GetBool(CYCLESFOG_NOISE_ENABLE))
			return false;
		return SUPER::GetDEnabling(node, id, data, flags, itemdesc);
	}

	// The box around the origin: what framing, selection and the view's clipping go by.
	virtual void GetDimension(const cinema::BaseObject* op, cinema::Vector* mp, cinema::Vector* rad) const override
	{
		*mp = cinema::Vector(0.0);
		*rad = HalfSize(op);
	}

	virtual cinema::DRAWRESULT Draw(cinema::BaseObject* op, cinema::DRAWPASS drawpass, cinema::BaseDraw* bd, cinema::BaseDrawHelp* bh) override
	{
		// The handles' pass is the base class's: it draws what GetHandle() gives.
		if (drawpass != cinema::DRAWPASS::OBJECT && drawpass != cinema::DRAWPASS::HIGHLIGHTS)
			return SUPER::Draw(op, drawpass, bd, bh);
		if (drawpass == cinema::DRAWPASS::HIGHLIGHTS)
		{
			cinema::Vector color(0.0);
			if (!bd->GetHighlightPassColor(*bh, true, color))
				return cinema::DRAWRESULT::SKIP;
			bd->SetPen(color, SET_PEN_USE_PROFILE_COLOR);
		}
		else
		{
			bd->SetPen(bd->GetObjectColor(bh, op), SET_PEN_USE_PROFILE_COLOR);
		}
		bd->SetMatrix_Matrix(op, bh->GetMg());

		// The twelve edges: from each corner on the -X side along X, and the two squares at -X and +X.
		const cinema::Vector h = HalfSize(op);
		for (maxon::Int side = 0; side < 2; ++side)
		{
			const maxon::Float64 x = side == 0 ? -h.x : h.x;
			const cinema::Vector square[4] = { cinema::Vector(x, -h.y, -h.z), cinema::Vector(x, h.y, -h.z), cinema::Vector(x, h.y, h.z), cinema::Vector(x, -h.y, h.z) };
			for (maxon::Int i = 0; i < 4; ++i)
			{
				bd->DrawLine(square[i], square[(i + 1) % 4], 0);
				if (side == 0)
					bd->DrawLine(square[i], cinema::Vector(h.x, square[i].y, square[i].z), 0);
			}
		}
		return cinema::DRAWRESULT::OK;
	}

	// Three handles, on the +X, +Y and +Z faces, as Cinema 4D's Cube has: each moves along its axis and
	// sets that edge to twice its distance from the centre.
	virtual cinema::Int32 GetHandleCount(const cinema::BaseObject* op) const override
	{
		return 3;
	}

	virtual void GetHandle(cinema::BaseObject* op, cinema::Int32 i, cinema::HandleInfo& info) override
	{
		const cinema::Vector h = HalfSize(op);
		const cinema::Vector axis(i == 0 ? 1.0 : 0.0, i == 1 ? 1.0 : 0.0, i == 2 ? 1.0 : 0.0);
		info.position = cinema::Vector(axis.x * h.x, axis.y * h.y, axis.z * h.z);
		info.direction = axis;
		info.type = cinema::HANDLECONSTRAINTTYPE::LINEAR;
	}

	virtual void SetHandle(cinema::BaseObject* op, cinema::Int32 i, cinema::Vector p, const cinema::HandleInfo& info) override
	{
		cinema::BaseContainer& data = op->GetDataInstanceRef();
		cinema::Vector size = data.GetVector(CYCLESFOG_SIZE);
		const maxon::Float64 edge = maxon::Max(0.0, 2.0 * Dot(p, info.direction));
		if (i == 0)
			size.x = edge;
		else if (i == 1)
			size.y = edge;
		else
			size.z = edge;
		data.SetVector(CYCLESFOG_SIZE, size);
	}

private:
	static cinema::Vector HalfSize(const cinema::BaseObject* op)
	{
		const cinema::Vector size = op->GetDataInstanceRef().GetVector(CYCLESFOG_SIZE);
		return cinema::Vector(maxon::Max(0.0, size.x), maxon::Max(0.0, size.y), maxon::Max(0.0, size.z)) * 0.5;
	}
};

cinema::Bool RegisterFogObject()
{
	// An icon of its own (tools/make_icons.py, fog_object): the plugin's hexagon with a misty disc.
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("fog_object.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	// OBJECT_HASDEFORMMODE: the green "enabled" check, as the Cycles Light has (lights/light_object.cpp),
	// without making it a generator. Cinema 4D's own Environment object, which has a fog, is in the
	// scene category, so this is too.
	const cinema::Bool registered = cinema::RegisterObjectPlugin(ID_FOG_OBJECT, cinema::GeLoadString(IDS_FOG_OBJECT_NAME), OBJECT_HASDEFORMMODE, FogObject::Alloc,
		"Ocyclesfog"_s, icon, 0, cinema::OBJECTCATEGORY::SCENE);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
