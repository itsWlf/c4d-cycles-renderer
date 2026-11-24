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
			{
				// (With the cone of its Angle: DrawSunAngle.)
				const maxon::Float64 r = maxon::Max(SYMBOL_SIZE, SUN_RAY_LENGTH * maxon::Sin(maxon::ClampValue(data.GetFloat(CYCLESLIGHT_ANGLE), 0.0, maxon::PI) * 0.5));
				*rad = cinema::Vector(r, r, SUN_RAY_LENGTH * 0.5);
				*mp = cinema::Vector(0.0, 0.0, SUN_RAY_LENGTH * 0.5);
				break;
			}
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
				DrawSunAngle(bd, data.GetFloat(CYCLESLIGHT_ANGLE));
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

	// Handles (0.3.0, F11 step 4), drawn by the base class's DRAWPASS::HANDLES and moved by Cinema 4D's
	// own handle dragging (undo and the Attribute Manager included), each on the shape as Draw() draws it
	// and moving along its axis (LightHandles):
	// - area: the size on the +X edge; for a Rectangle and an Ellipse the size Y on the +Y edge;
	// - point and spot: the radius on the +X side of its circle;
	// - spot: the cone's size on the +X side of its rim; with "Show Cone", the blend on the +Y side of the inner
	//   circle (on the rim while the blend is 0).
	// The sun has none: its angle is not drawn.
	virtual cinema::Int32 GetHandleCount(const cinema::BaseObject* op) const override
	{
		return LightHandles(op->GetDataInstanceRef(), nullptr);
	}

	virtual void GetHandle(cinema::BaseObject* op, cinema::Int32 i, cinema::HandleInfo& info) override
	{
		LightHandle handles[LIGHT_HANDLES_MAX];
		const maxon::Int32 count = LightHandles(op->GetDataInstanceRef(), handles);
		if (i < 0 || i >= count)
			return;
		info.position = handles[i].position;
		info.direction = handles[i].direction;
		info.type = cinema::HANDLECONSTRAINTTYPE::LINEAR;
	}

	virtual void SetHandle(cinema::BaseObject* op, cinema::Int32 i, cinema::Vector p, const cinema::HandleInfo& info) override
	{
		cinema::BaseContainer& data = op->GetDataInstanceRef();
		LightHandle handles[LIGHT_HANDLES_MAX];
		const maxon::Int32 count = LightHandles(data, handles);
		if (i < 0 || i >= count)
			return;
		// How far along its axis the handle now is, from the axis of the light (never below 0).
		const maxon::Float64 along = maxon::Max(0.0, Dot(p, handles[i].direction));
		switch (handles[i].parameter)
		{
			case CYCLESLIGHT_SIZE:
			case CYCLESLIGHT_SIZE_Y:
				data.SetFloat(handles[i].parameter, 2.0 * along);
				break;
			case CYCLESLIGHT_RADIUS:
				data.SetFloat(CYCLESLIGHT_RADIUS, along);
				break;
			case CYCLESLIGHT_SPOT_SIZE:
			{
				// The rim's radius at the cone's length gives the half angle (the description's 1 to 180 degrees).
				const maxon::Float64 size = 2.0 * maxon::ATan2(along, ConeLength(data));
				data.SetFloat(CYCLESLIGHT_SPOT_SIZE, maxon::ClampValue(size, maxon::DegToRad(1.0), maxon::PI));
				break;
			}
			case CYCLESLIGHT_SPOT_BLEND:
			{
				// The inverse of DrawCone's inner circle: the cosine of its angle, as a part of the cone's.
				const maxon::Float64 half = maxon::Min(data.GetFloat(CYCLESLIGHT_SPOT_SIZE), maxon::PI * 0.99) * 0.5;
				const maxon::Float64 cosOuter = maxon::Cos(half);
				const maxon::Float64 inner = maxon::Min(maxon::ATan2(along, ConeLength(data)), half);
				const maxon::Float64 blend = cosOuter < 1.0 ? (maxon::Cos(inner) - cosOuter) / (1.0 - cosOuter) : 0.0;
				data.SetFloat(CYCLESLIGHT_SPOT_BLEND, maxon::Clamp01(blend));
				break;
			}
			default:
				break;
		}
	}

private:
	static const maxon::Int32 LIGHT_HANDLES_MAX = 3;

	// One handle: where it sits (object space), the axis it moves along, and the parameter it sets.
	struct LightHandle
	{
		cinema::Vector position;
		cinema::Vector direction;
		maxon::Int32 parameter = 0;
	};

	static maxon::Float64 ConeLength(const cinema::BaseContainer& data)
	{
		return data.GetBool(CYCLESLIGHT_SHOW_CONE) ? SHOWN_CONE_LENGTH : CONE_LENGTH;
	}

	// The handles of the light as it is set now (GetHandleCount's comment); fills @p handles when given
	// (LIGHT_HANDLES_MAX entries) and returns how many there are.
	static maxon::Int32 LightHandles(const cinema::BaseContainer& data, LightHandle* handles)
	{
		maxon::Int32 count = 0;
		auto add = [&](const cinema::Vector& position, const cinema::Vector& direction, maxon::Int32 parameter)
		{
			if (handles)
				handles[count] = LightHandle { position, direction, parameter };
			++count;
		};
		const cinema::Vector xAxis(1.0, 0.0, 0.0);
		const cinema::Vector yAxis(0.0, 1.0, 0.0);
		switch (data.GetInt32(CYCLESLIGHT_TYPE, CYCLESLIGHT_TYPE_POINT))
		{
			case CYCLESLIGHT_TYPE_AREA:
			{
				const maxon::Int32 shape = data.GetInt32(CYCLESLIGHT_SHAPE, CYCLESLIGHT_SHAPE_SQUARE);
				const maxon::Float64 x = maxon::Max(0.0, data.GetFloat(CYCLESLIGHT_SIZE)) * 0.5;
				add(xAxis * x, xAxis, CYCLESLIGHT_SIZE);
				if (shape == CYCLESLIGHT_SHAPE_RECTANGLE || shape == CYCLESLIGHT_SHAPE_ELLIPSE)
					add(yAxis * (maxon::Max(0.0, data.GetFloat(CYCLESLIGHT_SIZE_Y)) * 0.5), yAxis, CYCLESLIGHT_SIZE_Y);
				break;
			}
			case CYCLESLIGHT_TYPE_SPOT:
			{
				add(xAxis * maxon::Max(0.0, data.GetFloat(CYCLESLIGHT_RADIUS)), xAxis, CYCLESLIGHT_RADIUS);
				// As DrawCone() draws the rim and the inner circle.
				const maxon::Float64 length = ConeLength(data);
				const maxon::Float64 half = maxon::Min(data.GetFloat(CYCLESLIGHT_SPOT_SIZE), maxon::PI * 0.99) * 0.5;
				add(cinema::Vector(length * maxon::Tan(half), 0.0, length), xAxis, CYCLESLIGHT_SPOT_SIZE);
				// (With no blend the inner circle is not drawn and the handle sits on the rim.)
				const maxon::Float64 blend = data.GetFloat(CYCLESLIGHT_SPOT_BLEND);
				if (data.GetBool(CYCLESLIGHT_SHOW_CONE))
				{
					const maxon::Float64 cosOuter = maxon::Cos(half);
					const maxon::Float64 inner = maxon::ACos(maxon::Clamp01(cosOuter + maxon::Clamp01(blend) * (1.0 - cosOuter)));
					add(cinema::Vector(0.0, length * maxon::Tan(inner), length), yAxis, CYCLESLIGHT_SPOT_BLEND);
				}
				break;
			}
			case CYCLESLIGHT_TYPE_SUN:
				break;
			default:
				add(xAxis * maxon::Max(0.0, data.GetFloat(CYCLESLIGHT_RADIUS)), xAxis, CYCLESLIGHT_RADIUS);
				break;
		}
		return count;
	}

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

	// A sun's Angle (how large the sun's disc is in the sky, which is what softens its shadows): a cone
	// around the ray, as wide as the angle, its edges as long as the ray, so that it stays in bounds up to
	// 180 degrees (a half sphere's rim). Nothing for an angle of 0.
	static void DrawSunAngle(cinema::BaseDraw* bd, maxon::Float64 angle)
	{
		const maxon::Float64 half = maxon::ClampValue(angle, 0.0, maxon::PI) * 0.5;
		if (half <= 0.0)
			return;
		const maxon::Float64 r = SUN_RAY_LENGTH * maxon::Sin(half);
		const maxon::Float64 z = SUN_RAY_LENGTH * maxon::Cos(half);
		cinema::Matrix rim;
		rim.off = cinema::Vector(0.0, 0.0, z);
		rim.sqmat.v1 = cinema::Vector(r, 0.0, 0.0);
		rim.sqmat.v2 = cinema::Vector(0.0, r, 0.0);
		rim.sqmat.v3 = cinema::Vector(0.0, 0.0, 1.0);
		bd->DrawCircle(rim);
		bd->DrawLine(cinema::Vector(0.0), cinema::Vector(r, 0.0, z), 0);
		bd->DrawLine(cinema::Vector(0.0), cinema::Vector(-r, 0.0, z), 0);
		bd->DrawLine(cinema::Vector(0.0), cinema::Vector(0.0, r, z), 0);
		bd->DrawLine(cinema::Vector(0.0), cinema::Vector(0.0, -r, z), 0);
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
	// OBJECT_HASDEFORMMODE gives the object the green "enabled" check in the Object Manager, as
	// Cinema 4D's own light has, without making it a generator (no cache is built). The
	// check is the object's deform mode, which every object keeps and which is on for an object
	// saved before the flag (measured; the exporter reads it: scene/light_export.h, IsLightSwitchedOn).
	const cinema::Bool registered = cinema::RegisterObjectPlugin(ID_LIGHT_OBJECT, cinema::GeLoadString(IDS_LIGHT_OBJECT_NAME), OBJECT_HASDEFORMMODE | PLUGINFLAG_HIDEPLUGINMENU, LightObject::Alloc,
		"Ocycleslight"_s, icon, 0, cinema::OBJECTCATEGORY::LIGHT);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
