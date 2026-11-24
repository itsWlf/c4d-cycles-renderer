// SPDX-License-Identifier: Apache-2.0
// The commands "Cycles Material" and "Cycles World Material" (cycles_material.h).

#include "c4d.h"
#include "c4d_basematerial.h"
#include "c4d_commanddata.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "maxon/graph.h"
#include "maxon/nodesgraph.h"
#include "maxon/nodeslib.h"

#include "../ids.h"
#include "../nodespace/cycles_nodes.h"
#include "cycles_material.h"

namespace cyclesrender
{

class CyclesMaterialCommand : public cinema::CommandData
{
public:
	virtual cinema::Bool Execute(cinema::BaseDocument* doc, cinema::GeDialog* parentManager) override
	{
		if (!doc)
			return false;
		// Only Cinema 4D's own material type (Mmaterial) can carry a node graph.
		cinema::BaseMaterial* material = cinema::BaseMaterial::Alloc(Mmaterial);
		if (!material)
			return false;
		material->SetName(cinema::GeLoadString(IDS_CYCLES_MATERIAL_NAME));
		cinema::NodeMaterial* const nodeMaterial = static_cast<cinema::NodeMaterial*>(material);
		iferr (nodeMaterial->CreateDefaultGraph(maxon::Id(CYCLES_NODESPACE_ID)))
		{
			ApplicationOutput("Cycles Render: the new material could not be given its Cycles node graph. @", err);
			cinema::BaseMaterial::Free(material);
			return false;
		}
		doc->StartUndo();
		doc->InsertMaterial(material);
		doc->AddUndo(cinema::UNDOTYPE::NEWOBJ, material);
		// Selected before EndUndo: after it, the selection is an undo step of its own, and the
		// first undo would only take the selection back.
		doc->SetActiveMaterial(material);
		doc->EndUndo();
		cinema::EventAdd();
		return true;
	}
};

// A node of the Cycles node table, added to @p model under @p id.
static maxon::Result<maxon::GraphNode> AddCyclesNode(const maxon::nodes::NodesGraphModelRef& model, const maxon::Char* id, const maxon::Char* asset)
{
	iferr_scope;
	const maxon::nodes::NodeTemplate nodeTemplate = maxon::nodes::NodesLib::LoadTemplate(maxon::AssetInterface::GetBuiltinRepository(), maxon::Id(asset)) iferr_return;
	return model.AddChild(maxon::Id(id), nodeTemplate);
}

// Wires the output @p from of @p fromNode into the input @p to of @p toNode.
static maxon::Result<void> Wire(const maxon::GraphNode& fromNode, const maxon::Char* from, const maxon::GraphNode& toNode, const maxon::Char* to)
{
	iferr_scope;
	const maxon::GraphNode outputs = fromNode.GetOutputs() iferr_return;
	const maxon::GraphNode output = outputs.FindChild(maxon::Id(from)) iferr_return;
	const maxon::GraphNode inputs = toNode.GetInputs() iferr_return;
	const maxon::GraphNode input = inputs.FindChild(maxon::Id(to)) iferr_return;
	if (!output.IsValid() || !input.IsValid())
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, FormatString("a port of the world material's nodes is missing (@ or @)", maxon::String(from), maxon::String(to)));
	output.Connect(input) iferr_return;
	return maxon::OK;
}

maxon::Result<cinema::BaseMaterial*> CreateCyclesWorldMaterial()
{
	iferr_scope;
	cinema::BaseMaterial* material = cinema::BaseMaterial::Alloc(Mmaterial);
	if (!material)
		return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
	finally
	{
		cinema::BaseMaterial::Free(material);
	};
	material->SetName(cinema::GeLoadString(IDS_CYCLES_WORLD_MATERIAL_NAME));
	cinema::NodeMaterial* const nodeMaterial = static_cast<cinema::NodeMaterial*>(material);
	const maxon::Id space(CYCLES_NODESPACE_ID);
	nodeMaterial->CreateEmptyGraph(space) iferr_return;
	const maxon::nodes::NodesGraphModelRef model = nodeMaterial->GetGraph(space) iferr_return;
	maxon::GraphTransaction transaction = model.BeginTransaction() iferr_return;
	// The Material Output every material of the space has (it stays unwired: a world shows nothing on an object).
	AddCyclesNode(model, "output", CYCLES_OUTPUT_NODE_ID) iferr_return;
	const maxon::GraphNode sky = AddCyclesNode(model, "sky", CYCLES_SKY_TEXTURE_NODE_ID) iferr_return;
	const maxon::GraphNode background = AddCyclesNode(model, "background", CYCLES_BACKGROUND_NODE_ID) iferr_return;
	const maxon::GraphNode world = AddCyclesNode(model, "world", CYCLES_WORLD_OUTPUT_NODE_ID) iferr_return;
	Wire(sky, "out.color", background, "in.color") iferr_return;
	Wire(background, "out.background", world, "in.surface") iferr_return;
	transaction.Commit() iferr_return;
	cinema::BaseMaterial* const made = material;
	material = nullptr;
	return made;
}

class CyclesWorldMaterialCommand : public cinema::CommandData
{
public:
	virtual cinema::Bool Execute(cinema::BaseDocument* doc, cinema::GeDialog* parentManager) override
	{
		if (!doc)
			return false;
		iferr (cinema::BaseMaterial* const material = CreateCyclesWorldMaterial())
		{
			ApplicationOutput("Cycles Render: the new world material could not be made. @", err);
			return false;
		}
		doc->StartUndo();
		doc->InsertMaterial(material);
		doc->AddUndo(cinema::UNDOTYPE::NEWOBJ, material);
		doc->SetActiveMaterial(material);
		doc->EndUndo();
		cinema::EventAdd();
		return true;
	}
};

cinema::Bool RegisterCyclesWorldMaterial()
{
	CyclesWorldMaterialCommand* const command = NewObjClear(CyclesWorldMaterialCommand);
	if (!command)
		return false;
	// The environment's icon: it is the world's material.
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("environment_object.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	const cinema::Bool registered = cinema::RegisterCommandPlugin(ID_CYCLES_WORLD_MATERIAL, cinema::GeLoadString(IDS_CYCLES_WORLD_MATERIAL_NAME), PLUGINFLAG_HIDEPLUGINMENU, icon,
		"Create a material of Cycles nodes set up as a world, for a Cycles Environment of the type Material"_s, command);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

cinema::Bool RegisterCyclesMaterial()
{
	CyclesMaterialCommand* const command = NewObjClear(CyclesMaterialCommand);
	if (!command)
		return false;
	// The plugin's icon without colour, as on the command "Cycles Native Material".
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("cyclesrender_grey.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	const cinema::Bool registered = cinema::RegisterCommandPlugin(ID_CYCLES_MATERIAL, cinema::GeLoadString(IDS_CYCLES_MATERIAL_NAME), PLUGINFLAG_HIDEPLUGINMENU, icon,
		"Create a material of Cycles nodes"_s, command);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
