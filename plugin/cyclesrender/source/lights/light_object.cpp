// SPDX-License-Identifier: Apache-2.0
// The Cycles Light object (light_object.h). Reading order: Init (the defaults), what the Attribute
// Manager shows, then how it draws itself in the viewport.

#include "c4d.h"
#include "c4d_objectdata.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"

#include "ocycleslight.h"

#include "../ids.h"
#include "light_object.h"
#include "light_settings.h"

namespace cyclesrender
{

// The viewport shows a light's place and direction at a size of its own (document units), as
// Cinema 4D draws its own lights: a point or sun as a small star, a spot's cone this long.
static const maxon::Float64 SYMBOL_SIZE = 20.0;
static const maxon::Float64 CONE_LENGTH = 100.0;
static const maxon::Float64 SHOWN_CONE_LENGTH = 400.0;
static const maxon::Float64 SUN_RAY_LENGTH = 200.0;

class LightObject : public cinema::ObjectData
{
	INSTANCEOF(LightObject, cinema::ObjectData)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(LightObject); }

	virtual cinema::Bool Init(cinema::GeListNode* node, cinema::Bool isCloneInit) override
	{
		cinema::BaseList2D* const item = static_cast<cinema::BaseList2D*>(node);
		if (!item)
			return false;
		if (isCloneInit)
			return true;
		cinema::BaseContainer& data = item->GetDataInstanceRef();
		InitLightSettings(data);
		data.SetInt32(CYCLESLIGHT_LINKING_MODE, CYCLESLIGHT_LINKING_MODE_EXCLUDE);
		return true;
	}

	virtual cinema::Bool GetDDescription(const cinema::GeListNode* node, cinema::Description* description, cinema::DESCFLAGS_DESC& flags) const override
	{
		if (!node || !description || !description->LoadDescription(node->GetType()))
			return false;
		flags |= cinema::DESCFLAGS_DESC::LOADED;
		ShowLightSettings(static_cast<const cinema::BaseList2D*>(node)->GetDataInstanceRef(), description);
		return SUPER::GetDDescription(node, description, flags);
	}

	virtual cinema::Bool GetDEnabling(const cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data,
		cinema::DESCFLAGS_ENABLE flags, const cinema::BaseContainer* itemdesc) const override
	{
		const cinema::BaseList2D* const item = static_cast<const cinema::BaseList2D*>(node);
		if (item && id.GetDepth() > 0 && !IsLightSettingActive(item->GetDataInstanceRef(), id[0].id))
			return false;
		return SUPER::GetDEnabling(node, id, data, flags, itemdesc);
	}

	virtual void GetDimension(const cinema::BaseObject* op, cinema::Vector* mp, cinema::Vector* rad) const override
	{
		const cinema::BaseContainer& data = op->GetDataInstanceRef();
		*mp = cinema::Vector(0.0);
		switch (data.GetInt32(CYCLESLIGHT_TYPE, CYCLESLIGHT_TYPE_POINT))
		{
			case CYCLESLIGHT_TYPE_AREA:
			{
				const maxon::Float64 x = data.GetFloat(CYCLESLIGHT_SIZE) * 0.5;
				const maxon::Bool two = data.GetInt32(CYCLESLIGHT_SHAPE) == CYCLESLIGHT_SHAPE_RECTANGLE || data.GetInt32(CYCLESLIGHT_SHAPE) == CYCLESLIGHT_SHAPE_ELLIPSE;
				const maxon::Float64 y = two ? data.GetFloat(CYCLESLIGHT_SIZE_Y) * 0.5 : x;
				*rad = cinema::Vector(x, y, maxon::Max(x, y) * 0.5);
				*mp = cinema::Vector(0.0, 0.0, rad->z);
				break;
			}
			case CYCLESLIGHT_TYPE_SPOT:
			{
				const maxon::Float64 length = data.GetBool(CYCLESLIGHT_SHOW_CONE) ? SHOWN_CONE_LENGTH : CONE_LENGTH;
				const maxon::Float64 r = length * maxon::Tan(maxon::Min(data.GetFloat(CYCLESLIGHT_SPOT_SIZE), maxon::PI * 0.99) * 0.5);
				*rad = cinema::Vector(r, r, length * 0.5);
				*mp = cinema::Vector(0.0, 0.0, length * 0.5);
				break;
			}
			case CYCLESLIGHT_TYPE_SUN:
				*rad = cinema::Vector(SYMBOL_SIZE, SYMBOL_SIZE, SUN_RAY_LENGTH * 0.5);
				*mp = cinema::Vector(0.0, 0.0, SUN_RAY_LENGTH * 0.5);
				break;
			default:
				*rad = cinema::Vector(maxon::Max(SYMBOL_SIZE, data.GetFloat(CYCLESLIGHT_RADIUS)));
				break;
		}
	}

	virtual cinema::DRAWRESULT Draw(cinema::BaseObject* op, cinema::DRAWPASS drawpass, cinema::BaseDraw* bd, cinema::BaseDrawHelp* bh) override
	{
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

		const cinema::BaseContainer& data = op->GetDataInstanceRef();
		switch (data.GetInt32(CYCLESLIGHT_TYPE, CYCLESLIGHT_TYPE_POINT))
		{
			case CYCLESLIGHT_TYPE_SUN:
				DrawStar(bd);
				// The direction the sun shines in, with an arrow head.
				bd->DrawLine(cinema::Vector(0.0), cinema::Vector(0.0, 0.0, SUN_RAY_LENGTH), 0);
				bd->DrawLine(cinema::Vector(0.0, 0.0, SUN_RAY_LENGTH), cinema::Vector(SYMBOL_SIZE * 0.5, 0.0, SUN_RAY_LENGTH - SYMBOL_SIZE), 0);
				bd->DrawLine(cinema::Vector(0.0, 0.0, SUN_RAY_LENGTH), cinema::Vector(-SYMBOL_SIZE * 0.5, 0.0, SUN_RAY_LENGTH - SYMBOL_SIZE), 0);
				break;
			case CYCLESLIGHT_TYPE_SPOT:
				DrawStar(bd);
				DrawRadius(bd, data.GetFloat(CYCLESLIGHT_RADIUS));
				DrawCone(bd, data.GetFloat(CYCLESLIGHT_SPOT_SIZE), data.GetFloat(CYCLESLIGHT_SPOT_BLEND), data.GetBool(CYCLESLIGHT_SHOW_CONE));
				break;
			case CYCLESLIGHT_TYPE_AREA:
				DrawArea(bd, data);
				break;
			default:
				DrawStar(bd);
				DrawRadius(bd, data.GetFloat(CYCLESLIGHT_RADIUS));
				break;
		}
		return cinema::DRAWRESULT::OK;
	}

private:
	// Three short crossing lines: where a point, spot or sun sits.
	static void DrawStar(cinema::BaseDraw* bd)
	{
		const maxon::Float64 s = SYMBOL_SIZE * 0.5;
		bd->DrawLine(cinema::Vector(-s, 0.0, 0.0), cinema::Vector(s, 0.0, 0.0), 0);
		bd->DrawLine(cinema::Vector(0.0, -s, 0.0), cinema::Vector(0.0, s, 0.0), 0);
		bd->DrawLine(cinema::Vector(0.0, 0.0, -s), cinema::Vector(0.0, 0.0, s), 0);
	}

	// The radius of a point or spot (its soft shadows) as three circles, when it has one.
	static void DrawRadius(cinema::BaseDraw* bd, maxon::Float64 radius)
	{
		if (radius <= 0.0)
			return;
		cinema::Matrix m;
		m.sqmat.v1 = cinema::Vector(radius, 0.0, 0.0);
		m.sqmat.v2 = cinema::Vector(0.0, radius, 0.0);
		m.sqmat.v3 = cinema::Vector(0.0, 0.0, radius);
		bd->DrawCircle(m);
		maxon::Swap(m.sqmat.v2, m.sqmat.v3);
		bd->DrawCircle(m);
		maxon::Swap(m.sqmat.v1, m.sqmat.v3);
		bd->DrawCircle(m);
	}

	// A spot's cone along +Z: four edges and the rim; with "Show Cone" longer, and with the circle
	// where the blend begins.
	static void DrawCone(cinema::BaseDraw* bd, maxon::Float64 spotSize, maxon::Float64 blend, maxon::Bool shown)
	{
		const maxon::Float64 length = shown ? SHOWN_CONE_LENGTH : CONE_LENGTH;
		const maxon::Float64 half = maxon::Min(spotSize, maxon::PI * 0.99) * 0.5;
		const maxon::Float64 r = length * maxon::Tan(half);
		cinema::Matrix rim;
		rim.off = cinema::Vector(0.0, 0.0, length);
		rim.sqmat.v1 = cinema::Vector(r, 0.0, 0.0);
		rim.sqmat.v2 = cinema::Vector(0.0, r, 0.0);
		rim.sqmat.v3 = cinema::Vector(0.0, 0.0, 1.0);
		bd->DrawCircle(rim);
		bd->DrawLine(cinema::Vector(0.0), cinema::Vector(r, 0.0, length), 0);
		bd->DrawLine(cinema::Vector(0.0), cinema::Vector(-r, 0.0, length), 0);
		bd->DrawLine(cinema::Vector(0.0), cinema::Vector(0.0, r, length), 0);
		bd->DrawLine(cinema::Vector(0.0), cinema::Vector(0.0, -r, length), 0);
		if (shown && blend > 0.0)
		{
			// Blender's blend is the part of the cone, in the cosine of the angle from the axis, over
			// which the light fades (light_export.cpp, SpotBlend); the circle shows where it begins.
			const maxon::Float64 cosOuter = maxon::Cos(half);
			const maxon::Float64 inner = maxon::ACos(maxon::Clamp01(cosOuter + maxon::Clamp01(blend) * (1.0 - cosOuter)));
			const maxon::Float64 ri = length * maxon::Tan(inner);
			rim.sqmat.v1 = cinema::Vector(ri, 0.0, 0.0);
			rim.sqmat.v2 = cinema::Vector(0.0, ri, 0.0);
			bd->DrawCircle(rim);
		}
	}

	// An area light's outline in its XY plane, and a short line along +Z, the side it shines to.
	static void DrawArea(cinema::BaseDraw* bd, const cinema::BaseContainer& data)
	{
		const maxon::Int32 shape = data.GetInt32(CYCLESLIGHT_SHAPE, CYCLESLIGHT_SHAPE_SQUARE);
		const maxon::Float64 x = data.GetFloat(CYCLESLIGHT_SIZE) * 0.5;
		const maxon::Bool two = shape == CYCLESLIGHT_SHAPE_RECTANGLE || shape == CYCLESLIGHT_SHAPE_ELLIPSE;
		const maxon::Float64 y = two ? data.GetFloat(CYCLESLIGHT_SIZE_Y) * 0.5 : x;
		if (shape == CYCLESLIGHT_SHAPE_DISK || shape == CYCLESLIGHT_SHAPE_ELLIPSE)
		{
			cinema::Matrix m;
			m.sqmat.v1 = cinema::Vector(x, 0.0, 0.0);
			m.sqmat.v2 = cinema::Vector(0.0, y, 0.0);
			m.sqmat.v3 = cinema::Vector(0.0, 0.0, 1.0);
			bd->DrawCircle(m);
		}
		else
		{
			const cinema::Vector corners[4] = { cinema::Vector(-x, -y, 0.0), cinema::Vector(x, -y, 0.0), cinema::Vector(x, y, 0.0), cinema::Vector(-x, y, 0.0) };
			for (maxon::Int i = 0; i < 4; ++i)
				bd->DrawLine(corners[i], corners[(i + 1) % 4], 0);
		}
		bd->DrawLine(cinema::Vector(0.0), cinema::Vector(0.0, 0.0, maxon::Max(SYMBOL_SIZE, maxon::Max(x, y))), 0);
	}
};

cinema::Bool RegisterLightObject()
{
	// A yellow icon of its own (tools/make_icons.py, light_object): the plugin's hexagon, dark
	// yellow, with a lighter circle.
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("light_object.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	const cinema::Bool registered = cinema::RegisterObjectPlugin(ID_LIGHT_OBJECT, cinema::GeLoadString(IDS_LIGHT_OBJECT_NAME), 0, LightObject::Alloc,
		"Ocycleslight"_s, icon, 0, cinema::OBJECTCATEGORY::LIGHT);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
