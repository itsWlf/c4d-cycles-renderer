// SPDX-License-Identifier: Apache-2.0
// The Cycles Environment object (environment_object.h). Reading order: Init (the defaults, from the node
// table), the Attribute Manager (Blender's names and options; what shows for which type, what is greyed),
// the HDRI's two path buttons, then the drawing in the viewport.

#include "c4d.h"
#include "c4d_objectdata.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"

#include "ocyclesenvironment.h"

#include "../ids.h"
#include "../nodespace/path_commands.h"
#include "../scene/environment_export.h"
#include "../scene/parameters.h"
#include "environment_object.h"

namespace cyclesrender
{

// The size the object is drawn at, in document units: a small globe, as Cinema 4D's Sky is drawn small.
static const maxon::Float64 SYMBOL_RADIUS = 50.0;

// The settings of the Sky Texture that Blender shows for each kind of sky (node_shader_tex_sky.cc,
// node_shader_buts_tex_sky): the scattering skies (the first two options) have the sun disc, its size and
// intensity, elevation, rotation, altitude and the three densities; Preetham turbidity; Hosek / Wilkie
// turbidity and ground albedo. Preetham and Hosek / Wilkie take the sun's direction, which here is the
// elevation and rotation too (scene/environment_export.cpp), so those show for every sky.
static maxon::Bool IsSkySettingShown(maxon::Int32 id, const maxon::String& skyType, maxon::Bool sunDisc)
{
	const maxon::Bool scattering = skyType != "PREETHAM"_s && skyType != "HOSEK_WILKIE"_s;
	switch (id)
	{
		case CYCLESENV_SKY_TYPE:
		case CYCLESENV_SKY_SUN_ELEVATION:
		case CYCLESENV_SKY_SUN_ROTATION:
			return true;
		case CYCLESENV_SKY_SUN_DISC:
		case CYCLESENV_SKY_ALTITUDE:
		case CYCLESENV_SKY_AIR_DENSITY:
		case CYCLESENV_SKY_AEROSOL_DENSITY:
		case CYCLESENV_SKY_OZONE_DENSITY:
			return scattering;
		case CYCLESENV_SKY_SUN_SIZE:
		case CYCLESENV_SKY_SUN_INTENSITY:
			return scattering && sunDisc;
		case CYCLESENV_SKY_TURBIDITY:
			return !scattering;
		case CYCLESENV_SKY_GROUND_ALBEDO:
			return skyType == "HOSEK_WILKIE"_s;
	}
	return true;
}

class EnvironmentObject : public cinema::ObjectData
{
	INSTANCEOF(EnvironmentObject, cinema::ObjectData)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(EnvironmentObject); }

	virtual cinema::Bool Init(cinema::GeListNode* node, cinema::Bool isCloneInit) override
	{
		cinema::BaseList2D* const item = static_cast<cinema::BaseList2D*>(node);
		if (!item)
			return false;
		if (isCloneInit)
			return true;
		// A Sky Texture (the maintainer, 2026-11-23: "A new environment should start as a Sky Texture."), strength
		// 1, seen by the camera and lighting the scene; the Color type's colour that of Blender's own new world;
		// the nodes' settings with Blender's defaults as the node table has them; the World Fog off, with a
		// twentieth of the Cycles Fog's density (ENVIRONMENT_DEFAULT_FOG_DENSITY) and Blender's other defaults. A scene saved before the default
		// changed keeps the type it stored (a value missing from a saved object reads its Init default).
		cinema::BaseContainer& data = item->GetDataInstanceRef();
		data.SetInt32(CYCLESENV_TYPE, CYCLESENV_TYPE_SKY);
		data.SetFloat(CYCLESENV_STRENGTH, 1.0);
		data.SetBool(CYCLESENV_SEEN_BY_CAMERA, true);
		data.SetBool(CYCLESENV_LIGHTS_SCENE, true);
		data.SetVector(CYCLESENV_COLOR, cinema::Vector(0.05));
		data.SetBool(CYCLESENV_FOG_ENABLE, false);
		data.SetFloat(CYCLESENV_FOG_DENSITY, ENVIRONMENT_DEFAULT_FOG_DENSITY);
		data.SetVector(CYCLESENV_FOG_COLOR, cinema::Vector(0.5));
		data.SetFloat(CYCLESENV_FOG_ANISOTROPY, 0.0);
		data.SetVector(CYCLESENV_FOG_ABSORPTION_COLOR, cinema::Vector(0.0));
		data.SetFloat(CYCLESENV_FOG_EMISSION_STRENGTH, 0.0);
		data.SetVector(CYCLESENV_FOG_EMISSION_COLOR, cinema::Vector(1.0));
		for (const EnvironmentNodeSetting& setting : GetEnvironmentNodeSettings())
		{
			const CyclesProperty* const property = FindEnvironmentProperty(setting);
			if (!property)
				continue;
			if (property->kind == 'e')
			{
				maxon::Int32 index = 0;
				iferr (const maxon::BaseArray<EnvironmentOption> options = GetEnvironmentOptions(*property))
					continue;
				for (maxon::Int i = 0; i < options.GetCount(); ++i)
				{
					if (options[i].identifier == maxon::String(property->text))
						index = maxon::Int32(i);
				}
				data.SetInt32(setting.id, index);
			}
			else if (property->kind == 'b')
			{
				data.SetBool(setting.id, property->number != 0.0);
			}
			else
			{
				data.SetFloat(setting.id, property->number);
			}
		}
		return true;
	}

	virtual cinema::Bool GetDDescription(const cinema::GeListNode* node, cinema::Description* description, cinema::DESCFLAGS_DESC& flags) const override
	{
		if (!node || !description || !description->LoadDescription(node->GetType()))
			return false;
		flags |= cinema::DESCFLAGS_DESC::LOADED;
		const cinema::BaseContainer& data = static_cast<const cinema::BaseList2D*>(node)->GetDataInstanceRef();
		const maxon::Int32 type = data.GetInt32(CYCLESENV_TYPE, CYCLESENV_TYPE_SKY);
		SetHidden(description, CYCLESENV_COLOR, type != CYCLESENV_TYPE_COLOR);
		SetHidden(description, CYCLESENV_GROUP_SKY, type != CYCLESENV_TYPE_SKY);
		SetHidden(description, CYCLESENV_GROUP_HDRI, type != CYCLESENV_TYPE_HDRI);
		// The Material type's strength is its own Background's.
		SetHidden(description, CYCLESENV_STRENGTH, type == CYCLESENV_TYPE_MATERIAL);

		// Blender's names and options, from the node table.
		maxon::String skyType;
		for (const EnvironmentNodeSetting& setting : GetEnvironmentNodeSettings())
		{
			const CyclesProperty* const property = FindEnvironmentProperty(setting);
			cinema::BaseContainer* const parameter = description->GetParameterI(CreateDescID(cinema::DescLevel(setting.id)), nullptr);
			if (!property || !parameter)
				continue;
			maxon::String label(property->label);
			if (setting.id == CYCLESENV_SKY_ALTITUDE)
				label += " (m)"_s;
			parameter->SetString(cinema::DESC_NAME, label);
			parameter->SetString(cinema::DESC_SHORT_NAME, label);
			if (property->kind != 'e')
				continue;
			iferr (const maxon::BaseArray<EnvironmentOption> options = GetEnvironmentOptions(*property))
				continue;
			cinema::BaseContainer cycle;
			for (maxon::Int i = 0; i < options.GetCount(); ++i)
				cycle.SetString(cinema::Int32(i), options[i].label);
			parameter->SetContainer(cinema::DESC_CYCLE, cycle);
			if (setting.id == CYCLESENV_SKY_TYPE)
			{
				const maxon::Int32 index = data.GetInt32(CYCLESENV_SKY_TYPE, 0);
				if (index >= 0 && index < options.GetCount())
					skyType = options[index].identifier;
			}
		}
		const maxon::Bool sunDisc = data.GetBool(CYCLESENV_SKY_SUN_DISC, true);
		for (const EnvironmentNodeSetting& setting : GetEnvironmentNodeSettings())
		{
			if (setting.id >= CYCLESENV_SKY_TYPE && setting.id <= CYCLESENV_SKY_GROUND_ALBEDO)
				SetHidden(description, setting.id, !IsSkySettingShown(setting.id, skyType, sunDisc));
		}
		return SUPER::GetDDescription(node, description, flags);
	}

	// Greyed: the Sky Texture's own sun while a light places it; the World Fog's values while it is off.
	virtual cinema::Bool GetDEnabling(const cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data,
		cinema::DESCFLAGS_ENABLE flags, const cinema::BaseContainer* itemdesc) const override
	{
		const cinema::BaseObject* const op = static_cast<const cinema::BaseObject*>(node);
		const maxon::Int32 parameter = id.GetDepth() > 0 ? id[0].id : 0;
		if (op && (parameter == CYCLESENV_SKY_SUN_ELEVATION || parameter == CYCLESENV_SKY_SUN_ROTATION))
		{
			maxon::Bool notALight = false;
			if (GetEnvironmentSunLight(op, op->GetDocument(), notALight))
				return false;
		}
		if (op && parameter > CYCLESENV_FOG_ENABLE && parameter <= CYCLESENV_FOG_EMISSION_COLOR && !op->GetDataInstanceRef().GetBool(CYCLESENV_FOG_ENABLE))
			return false;
		return SUPER::GetDEnabling(node, id, data, flags, itemdesc);
	}

	// The HDRI's "Make Relative" and "Make Absolute": as the image nodes' buttons (nodespace/path_commands.h),
	// relative to the document's folder, in one undo step.
	virtual cinema::Bool Message(cinema::GeListNode* node, cinema::Int32 type, void* data) override
	{
		if (type == MSG_DESCRIPTION_COMMAND && data)
		{
			const cinema::DescriptionCommand* const command = static_cast<const cinema::DescriptionCommand*>(data);
			const maxon::Int32 id = command->_descId[0].id;
			if (id == CYCLESENV_HDRI_MAKE_RELATIVE || id == CYCLESENV_HDRI_MAKE_ABSOLUTE)
			{
				ChangeHdriPath(static_cast<cinema::BaseObject*>(node), id == CYCLESENV_HDRI_MAKE_RELATIVE);
				return true;
			}
		}
		return SUPER::Message(node, type, data);
	}

	virtual void GetDimension(const cinema::BaseObject* op, cinema::Vector* mp, cinema::Vector* rad) const override
	{
		*mp = cinema::Vector(0.0);
		*rad = cinema::Vector(SYMBOL_RADIUS);
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
		const cinema::Matrix mg = bh->GetMg();
		bd->SetMatrix_Matrix(op, mg);

		// A globe of three circles, and its +Z axis twice as long: which way the environment is turned.
		cinema::Matrix m;
		m.sqmat.v1 = cinema::Vector(SYMBOL_RADIUS, 0.0, 0.0);
		m.sqmat.v2 = cinema::Vector(0.0, SYMBOL_RADIUS, 0.0);
		m.sqmat.v3 = cinema::Vector(0.0, 0.0, SYMBOL_RADIUS);
		bd->DrawCircle(m);
		maxon::Swap(m.sqmat.v2, m.sqmat.v3);
		bd->DrawCircle(m);
		maxon::Swap(m.sqmat.v1, m.sqmat.v3);
		bd->DrawCircle(m);
		bd->DrawLine(cinema::Vector(0.0), cinema::Vector(0.0, 0.0, 2.0 * SYMBOL_RADIUS), 0);

		// The Sky Texture: a line towards the sun, drawn in the object's axes (the export's sun, which a linked
		// light places in the world, brought into them).
		const cinema::BaseContainer& data = op->GetDataInstanceRef();
		if (data.GetInt32(CYCLESENV_TYPE, CYCLESENV_TYPE_SKY) == CYCLESENV_TYPE_SKY)
		{
			cinema::Matrix axes = mg;
			axes.off = cinema::Vector(0.0);
			axes.sqmat.v1 = axes.sqmat.v1.GetNormalized();
			axes.sqmat.v2 = axes.sqmat.v2.GetNormalized();
			axes.sqmat.v3 = axes.sqmat.v3.GetNormalized();
			const cinema::Vector world = GetEnvironmentSunDirection(op, mg, op->GetDocument());
			const cinema::Vector sun = (~axes * world).GetNormalized();
			bd->DrawLine(sun * SYMBOL_RADIUS, sun * (3.0 * SYMBOL_RADIUS), 0);
			cinema::Matrix disc;
			disc.off = sun * (3.0 * SYMBOL_RADIUS);
			const cinema::Vector side = (maxon::Abs(sun.y) < 0.99 ? Cross(sun, cinema::Vector(0.0, 1.0, 0.0)) : cinema::Vector(1.0, 0.0, 0.0)).GetNormalized();
			disc.sqmat.v1 = side * (0.2 * SYMBOL_RADIUS);
			disc.sqmat.v2 = Cross(sun, side).GetNormalized() * (0.2 * SYMBOL_RADIUS);
			disc.sqmat.v3 = sun;
			bd->DrawCircle(disc);
		}
		return cinema::DRAWRESULT::OK;
	}

private:
	static void ChangeHdriPath(cinema::BaseObject* op, maxon::Bool relative)
	{
		cinema::BaseDocument* const doc = op ? op->GetDocument() : nullptr;
		if (!op)
			return;
		const cinema::Filename file = op->GetDataInstanceRef().GetFilename(CYCLESENV_HDRI_FILE);
		maxon::String problem;
		cinema::Filename changed;
		iferr (changed = ChangeFilenameKind(file, doc, relative, problem))
			problem = err.GetMessage();
		if (problem.IsEmpty())
		{
			if (doc)
			{
				doc->StartUndo();
				doc->AddUndo(cinema::UNDOTYPE::CHANGE_SMALL, op);
			}
			op->SetParameter(CreateDescID(cinema::DescLevel(CYCLESENV_HDRI_FILE)), changed, cinema::DESCFLAGS_SET::USERINTERACTION);
			if (doc)
				doc->EndUndo();
			cinema::EventAdd();
			return;
		}
		const maxon::String text = FormatString("Cycles Render: the HDRI of \"@\" was not made @: @", op->GetName(), relative ? "relative"_s : "absolute"_s, problem);
		ApplicationOutput("@", text);
		cinema::StatusSetText(text);
	}

	static void SetHidden(cinema::Description* description, maxon::Int32 id, maxon::Bool hidden)
	{
		if (cinema::BaseContainer* const parameter = description->GetParameterI(CreateDescID(cinema::DescLevel(id)), nullptr))
			parameter->SetBool(cinema::DESC_HIDE, hidden);
	}
};

cinema::Bool RegisterEnvironmentObject()
{
	// An icon of its own (tools/make_icons.py, environment_object).
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("environment_object.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	// OBJECT_HASDEFORMMODE: the green "enabled" check, as the Cycles Light and Fog have. The scene
	// category, as Cinema 4D's Sky.
	const cinema::Bool registered = cinema::RegisterObjectPlugin(ID_ENVIRONMENT_OBJECT, cinema::GeLoadString(IDS_ENVIRONMENT_OBJECT_NAME), OBJECT_HASDEFORMMODE,
		EnvironmentObject::Alloc, "Ocyclesenvironment"_s, icon, 0, cinema::OBJECTCATEGORY::SCENE);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
