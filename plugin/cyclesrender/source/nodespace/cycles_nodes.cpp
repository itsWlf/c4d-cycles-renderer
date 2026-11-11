// SPDX-License-Identifier: Apache-2.0
// The table of Blender's shader nodes, and the questions asked of it (cycles_nodes.h).

#include "c4d.h"

#include "cycles_nodes.h"

namespace cyclesrender
{

#include "cycles_nodes_table.h"

maxon::Block<const CyclesNode> GetCyclesNodes()
{
	return maxon::Block<const CyclesNode>(CYCLES_NODES, SIZEOF(CYCLES_NODES) / SIZEOF(CYCLES_NODES[0]));
}

maxon::Block<const CyclesNodeFolder> GetCyclesNodeFolders()
{
	return maxon::Block<const CyclesNodeFolder>(CYCLES_NODE_FOLDERS, SIZEOF(CYCLES_NODE_FOLDERS) / SIZEOF(CYCLES_NODE_FOLDERS[0]));
}

maxon::Vector GetCyclesPropertyVector(const CyclesProperty& property)
{
	maxon::Vector result;
	maxon::Int index = 0;
	iferr (maxon::String(property.text).Split(" "_s, true, [&result, &index](const maxon::String& part) -> maxon::Result<maxon::Bool>
		{
			iferr_scope;
			if (index < 3)
			{
				result[index] = part.ToFloat() iferr_return;
				++index;
			}
			return true;
		}))
	{
	}
	return result;
}

maxon::Result<maxon::Bool> IsCyclesSocketShown(const maxon::GraphNode& node, const CyclesNode& definition, const CyclesSocket& socket)
{
	iferr_scope;
	if (!socket.show || !socket.show[0])
		return true;
	if (socket.show[0] == '!')
		return false;

	const maxon::GraphNode inputs = node.GetInputs() iferr_return;
	maxon::Bool shown = true;
	maxon::String(socket.show).Split("&"_s, true, [&](const maxon::String& clause) -> maxon::Result<maxon::Bool>
		{
			iferr_scope;
			maxon::Int equals = 0;
			if (!clause.Find("="_s, &equals))
				return true;
			const maxon::String portName = clause.GetPart(0, equals);
			const maxon::String allowed = "|"_s + clause.GetPart(equals + 1, clause.GetLength() - equals - 1) + "|"_s;

			// The setting's value now, or its default while the port has none.
			maxon::String value;
			for (maxon::Int i = 0; i < definition.propertyCount; ++i)
			{
				const CyclesProperty& property = definition.properties[i];
				if (portName != maxon::String(property.port))
					continue;
				maxon::Id portId;
				portId.Init(portName) iferr_return;
				const maxon::GraphNode port = inputs.FindChild(portId) iferr_return;
				if (property.kind == 'b')
				{
					maxon::Bool on = property.number != 0.0;
					if (port.IsValid())
					{
						on = port.GetEffectivePortValue<maxon::Bool>().GetValueOr(on) iferr_return;
					}
					value = on ? "1"_s : "0"_s;
				}
				else
				{
					maxon::Id chosen;
					chosen.Init(maxon::String(property.text)) iferr_return;
					if (port.IsValid())
					{
						chosen = port.GetEffectivePortValue<maxon::Id>().GetValueOr(chosen) iferr_return;
					}
					value = chosen.ToString();
				}
				break;
			}
			if (!allowed.Find("|"_s + value + "|"_s, nullptr))
			{
				shown = false;
				return false;
			}
			return true;
		}) iferr_return;
	return shown;
}

const CyclesNode* FindCyclesNode(const maxon::Id& asset)
{
	const maxon::CString wanted = asset.ToCString();
	for (const CyclesNode& node : CYCLES_NODES)
	{
		if (wanted == maxon::CString(node.asset))
			return &node;
	}
	return nullptr;
}

} // namespace cyclesrender
