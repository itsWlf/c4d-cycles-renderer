// SPDX-License-Identifier: Apache-2.0
// The command "Cycles Material" (cycles_material.h).

#include "c4d.h"
#include "c4d_basematerial.h"
#include "c4d_commanddata.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"

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

cinema::Bool RegisterCyclesMaterial()
{
	CyclesMaterialCommand* const command = NewObjClear(CyclesMaterialCommand);
	if (!command)
		return false;
	// The plugin's icon without colour, as on the command "Cycles Native Material".
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("cyclesrender_grey.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	const cinema::Bool registered = cinema::RegisterCommandPlugin(ID_CYCLES_MATERIAL, cinema::GeLoadString(IDS_CYCLES_MATERIAL_NAME), 0, icon,
		"Create a material of Cycles nodes"_s, command);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
