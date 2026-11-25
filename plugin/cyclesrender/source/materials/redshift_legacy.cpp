// SPDX-License-Identifier: Apache-2.0
// The legacy Redshift material "RS Shader Graph" (redshift_legacy.h): its Xpresso graph copied into a Redshift node graph of
// a material made for the purpose, which the node-material translation reads. Reading order: finding the graph, copying a
// node's parameters into ports (by name, converted to the port's type), the wires, then BuildRedshiftShaderGraph.
// Measured on RS_Parity_Realism and RS_Parity_Sweeps (internal/f13/probe_w17.py): every Redshift shader node says its class
// in parameter 1001 ("StandardMaterial", "TextureSampler", ...), its parameters from 10000 on carry the names of the node
// space's ports ("base_color", "refl_weight"; "tex0" is a Redshift file, its path read as the dome light's), an input port's
// main id is the id of its parameter, and an output port's name is the node space's output ("outColor").

#include "c4d.h"
#include "c4d_basematerial.h"
#include "c4d_graphview.h"
#include "drsfile.h"
#include "maxon/graph.h"
#include "maxon/hashmap.h"
#include "maxon/nodesgraph.h"
#include "maxon/url.h"

#include "../sdk_compat.h"
#include "../scene/parameters.h"
#include "../scene/redshift_ids.h"
#include "baked_files.h"
#include "material_export.h"
#include "redshift_legacy.h"
#include "redshift_material.h"

#define RS_NODE "com.redshift3d.redshift4c4d.nodes.core."

namespace cyclesrender
{

// The node of the node space for a legacy shader class, and the output node.
static const maxon::Char* const RS_OUTPUT_NODE = "com.redshift3d.redshift4c4d.node.output";

// An Xpresso node of the graph and the node of the copy that stands for it.
struct CopiedNode
{
	cinema::GvNode* source = nullptr;
	maxon::GraphNode node;
	maxon::String asset;                                       ///< the node space's id, "<RS_NODE><class>", or RS_OUTPUT_NODE
	maxon::HashMap<maxon::Int32, maxon::String> names;         ///< parameter id -> port name
};

// The root of the material's Xpresso graph: the first node of its branch of graph-view nodes.
static maxon::Result<cinema::GvNode*> GraphRoot(cinema::BaseMaterial* material)
{
	iferr_scope;
	cinema::GvNode* root = nullptr;
	material->GetBranchInfo([&root](const cinema::BranchInfo& info) -> maxon::Result<maxon::Bool>
		{
			if (info.id != ID_GV_NODEDATA || !info.head)
				return true;
			cinema::GeListNode* const first = info.head->GetFirst();
			if (first && first->IsInstanceOf(ID_GV_NODEDATA))
				root = static_cast<cinema::GvNode*>(first);
			return root == nullptr;
		}, cinema::GETBRANCHINFO::NONE) iferr_return;
	return root;
}

// What a port of the copy holds by default, to know what to convert a parameter's value into.
static const maxon::Int KIND_NONE = 0;
static const maxon::Int KIND_FLOAT = 1;
static const maxon::Int KIND_FLOAT32 = 2;
static const maxon::Int KIND_INT = 3;
static const maxon::Int KIND_BOOL = 4;
static const maxon::Int KIND_COLOR = 5;
static const maxon::Int KIND_COLORA = 6;
static const maxon::Int KIND_VECTOR = 7;
static const maxon::Int KIND_VECTOR2D = 8;
static const maxon::Int KIND_STRING = 9;

template <typename T> static maxon::Bool Holds(const maxon::GraphNode& port)
{
	ifnoerr (const maxon::Opt<const T&> value = port.GetPortValue<T>())
	{
		return value.HasValue();
	}
	return false;
}

static maxon::Int KindOf(const maxon::GraphNode& port)
{
	if (Holds<maxon::Float>(port))
		return KIND_FLOAT;
	if (Holds<maxon::Float32>(port))
		return KIND_FLOAT32;
	if (Holds<maxon::Int>(port))
		return KIND_INT;
	if (Holds<maxon::Bool>(port))
		return KIND_BOOL;
	if (Holds<maxon::Color>(port))
		return KIND_COLOR;
	if (Holds<maxon::ColorA>(port))
		return KIND_COLORA;
	if (Holds<maxon::Vector>(port))
		return KIND_VECTOR;
	if (Holds<maxon::Vector2d>(port))
		return KIND_VECTOR2D;
	if (Holds<maxon::String>(port))
		return KIND_STRING;
	return KIND_NONE;
}

// A parameter's value set on a port of the copy, converted into the port's type. False when the two do not fit.
static maxon::Result<maxon::Bool> CopyValue(const maxon::GraphNode& port, const cinema::GeData& data)
{
	iferr_scope;
	const maxon::Int kind = KindOf(port);
	maxon::Float64 number = 0.0;
	cinema::Vector vector;
	if (data.GetType() == cinema::DA_REAL)
	{
		number = data.GetFloat();
		vector = cinema::Vector(number);
	}
	else if (data.GetType() == cinema::DA_LONG)
	{
		number = maxon::Float64(data.GetInt32());
		vector = cinema::Vector(number);
	}
	else if (data.GetType() == cinema::DA_VECTOR)
	{
		vector = data.GetVector();
		number = vector.x;
	}
	else if (data.GetType() == cinema::DA_STRING && kind == KIND_STRING)
	{
		port.SetPortValue(data.GetString()) iferr_return;
		return true;
	}
	else
	{
		return false;
	}
	if (kind == KIND_FLOAT)
	{
		port.SetPortValue(maxon::Float(number)) iferr_return;
	}
	else if (kind == KIND_FLOAT32)
	{
		port.SetPortValue(maxon::Float32(number)) iferr_return;
	}
	else if (kind == KIND_INT)
	{
		port.SetPortValue(maxon::Int(maxon::Floor(number + 0.5))) iferr_return;
	}
	else if (kind == KIND_BOOL)
	{
		port.SetPortValue(maxon::Bool(number != 0.0)) iferr_return;
	}
	else if (kind == KIND_COLOR)
	{
		port.SetPortValue(maxon::Color(vector.x, vector.y, vector.z)) iferr_return;
	}
	else if (kind == KIND_COLORA)
	{
		port.SetPortValue(maxon::ColorA(vector.x, vector.y, vector.z, 1.0)) iferr_return;
	}
	else if (kind == KIND_VECTOR)
	{
		port.SetPortValue(maxon::Vector(vector.x, vector.y, vector.z)) iferr_return;
	}
	else if (kind == KIND_VECTOR2D)
	{
		port.SetPortValue(maxon::Vector2d(vector.x, vector.y)) iferr_return;
	}
	else
	{
		return false;
	}
	return true;
}

// The input port of @p asset named @p name (the node space's names; "tex0_colorspace" is the "colorspace" of the "tex0" bundle).
static maxon::Result<maxon::GraphNode> InputOf(const CopiedNode& copied, const maxon::String& name)
{
	iferr_scope;
	const maxon::GraphNode inputs = copied.node.GetInputs() iferr_return;
	maxon::Id id;
	if (name == "tex0_colorspace"_s)
	{
		id.Init(copied.asset + ".tex0"_s) iferr_return;
		const maxon::GraphNode bundle = inputs.FindChild(id) iferr_return;
		if (!bundle.IsValid())
			return maxon::GraphNode();
		return bundle.FindChild(maxon::Id("colorspace"));
	}
	id.Init(copied.asset + "."_s + name) iferr_return;
	return inputs.FindChild(id);
}

// The file of a Texture node's "tex0" (a Redshift file: its path a string inside it, read as the dome light's).
static maxon::String TexturePath(cinema::GvNode* node, maxon::Int32 parameter)
{
	cinema::GeData data;
	if (node->GetParameter(CreateDescID(cinema::DescLevel(parameter, REDSHIFT_FILE_TYPE, 0), cinema::DescLevel(REDSHIFT_FILE_PATH, cinema::DTYPE_STRING, 0)),
		data, cinema::DESCFLAGS_GET::NONE) && data.GetType() == cinema::DA_STRING)
		return data.GetString();
	return maxon::String();
}

// Sets the file of the copy's Texture node: a path as a Url, as the node space keeps it.
static maxon::Result<void> SetTextureFile(const CopiedNode& copied, const maxon::String& path)
{
	iferr_scope;
	const maxon::GraphNode inputs = copied.node.GetInputs() iferr_return;
	const maxon::GraphNode bundle = inputs.FindChild(maxon::Id(RS_NODE "texturesampler.tex0")) iferr_return;
	if (!bundle.IsValid())
		return maxon::OK;
	const maxon::GraphNode pathPort = bundle.FindChild(maxon::Id("path")) iferr_return;
	if (!pathPort.IsValid())
		return maxon::OK;
	maxon::Url url;
	if (path.Find("://"_s, nullptr))
		url = maxon::Url(path);
	else
		url = cinema::MaxonConvert(cinema::Filename(path), cinema::MAXONCONVERTMODE::NONE);
	pathPort.SetPortValue(url) iferr_return;
	return maxon::OK;
}

// Copies the parameters of a Redshift shader node into the copy's node, and keeps their names by id for the wires.
static maxon::Result<void> CopyParameters(CopiedNode& copied)
{
	iferr_scope;
	cinema::AutoAlloc<cinema::Description> description;
	if (!description || !copied.source->GetDescription(description, cinema::DESCFLAGS_DESC::NONE))
		return maxon::OK;
	void* handle = description->BrowseInit();
	const cinema::BaseContainer* settings = nullptr;
	cinema::DescID id;
	cinema::DescID group;
	while (description->GetNext(handle, &settings, id, group))
	{
		if (!settings || id.GetDepth() < 1)
			continue;
		const maxon::Int32 parameter = id[0].id;
		if (parameter < 10000 || parameter >= 50000)
			continue;
		const maxon::String name = settings->GetString(cinema::DESC_NAME).ToLower();
		copied.names.Insert(parameter, name) iferr_return;
		if (name == "tex0"_s)
		{
			const maxon::String path = TexturePath(copied.source, parameter);
			if (path.IsPopulated())
			{
				SetTextureFile(copied, path) iferr_return;
			}
			continue;
		}
		const maxon::GraphNode port = InputOf(copied, name) iferr_return;
		if (!port.IsValid())
			continue;
		cinema::GeData data;
		if (!copied.source->GetParameter(CreateDescID(cinema::DescLevel(parameter)), data, cinema::DESCFLAGS_GET::NONE))
			continue;
		CopyValue(port, data) iferr_ignore("a parameter the port does not take keeps the port's default");
	}
	description->BrowseFree(handle);
	return maxon::OK;
}

// Every node of the Xpresso graph below @p node, depth first (groups are walked into, not copied).
static void Collect(cinema::GvNode* node, maxon::BaseArray<cinema::GvNode*>& found)
{
	for (; node; node = node->GetNext())
	{
		iferr (found.Append(node))
			return;
		Collect(node->GetDown(), found);
	}
}

// Copies the graph's nodes into @p model; the Xpresso nodes that are not copied are named.
static maxon::Result<void> CopyNodes(MaterialExporter& exporter, cinema::BaseMaterial* material, cinema::GvNode* root, const maxon::nodes::NodesGraphModelRef& model,
	maxon::BaseArray<CopiedNode>& copied, maxon::BaseArray<cinema::GvNode*>& shaders)
{
	iferr_scope;
	maxon::BaseArray<cinema::GvNode*> nodes;
	Collect(root->GetDown(), nodes);
	for (cinema::GvNode* node : nodes)
	{
		const maxon::Int32 op = node->GetOperatorID();
		maxon::String asset;
		if (op == REDSHIFT_GV_OUTPUT)
		{
			asset = maxon::String(RS_OUTPUT_NODE);
		}
		else if (op == REDSHIFT_GV_SHADER)
		{
			const cinema::GeData kind = GetParameterData(node, REDSHIFT_GV_SHADER_CLASS);
			if (kind.GetType() != cinema::DA_STRING || kind.GetString().IsEmpty())
				continue;
			asset = maxon::String(RS_NODE);
			asset += kind.GetString().ToLower();
		}
		else if (op == REDSHIFT_GV_C4D_SHADER)
		{
			shaders.Append(node) iferr_return;
			continue;
		}
		else
		{
			if (!node->GetDown())
				exporter.Warn(FormatString("Material \"@\": its Xpresso node \"@\" is not a Redshift shader node and is not translated.", material->GetName(), node->GetName()));
			continue;
		}
		maxon::Id assetId;
		assetId.Init(asset) iferr_return;
		CopiedNode entry;
		entry.source = node;
		entry.asset = asset;
		iferr (entry.node = model.AddChild(maxon::Id(), assetId))
		{
			exporter.Warn(FormatString("Material \"@\": its \"@\" node (@) has no node in Redshift's node space and is not translated.", material->GetName(), node->GetName(),
				asset));
			continue;
		}
		if (!entry.node.IsValid())
			continue;
		if (op == REDSHIFT_GV_SHADER)
		{
			CopyParameters(entry) iferr_return;
		}
		copied.Append(std::move(entry)) iferr_return;
	}
	return maxon::OK;
}

// The copy of @p node, or nullptr.
static CopiedNode* CopyOf(maxon::BaseArray<CopiedNode>& copied, const cinema::GvNode* node)
{
	for (CopiedNode& entry : copied)
	{
		if (entry.source == node)
			return &entry;
	}
	return nullptr;
}

// The wires: each input of a copied node wired to an output of another copied node, the same in the copy. A Cinema 4D shader
// wired into a Texture node's file is baked (as a classic material's shader is) and the Texture node reads that file as RAW.
static maxon::Result<void> CopyWires(MaterialExporter& exporter, cinema::BaseMaterial* material, maxon::BaseArray<CopiedNode>& copied)
{
	iferr_scope;
	for (CopiedNode& target : copied)
	{
		for (maxon::Int32 index = 0; index < target.source->GetInPortCount(); ++index)
		{
			cinema::GvPort* const in = target.source->GetInPort(index);
			cinema::GvNode* from = nullptr;
			cinema::GvPort* out = nullptr;
			if (!in || !in->GetIncomingSource(from, out) || !from || !out)
				continue;
			maxon::String name;
			if (target.asset == maxon::String(RS_OUTPUT_NODE))
				name = in->GetName(target.source).ToLower();
			else if (const maxon::String* known = target.names.FindValue(in->GetMainID()))
				name = *known;
			if (name.IsEmpty())
				continue;
			if (from->GetOperatorID() == REDSHIFT_GV_C4D_SHADER && name == "tex0"_s)
			{
				// Redshift samples the shader into a picture of the node's Width x Height; it is baked here at the export's resolution.
				cinema::BaseShader* const shader = static_cast<cinema::BaseShader*>(GetParameterData(from, REDSHIFT_GV_C4D_SHADER_LINK).GetLink(exporter.GetDocument()));
				if (!shader)
					continue;
				const maxon::String image = exporter.GetBakedImageId(shader) iferr_return;
				maxon::String path;
				if (image.IsPopulated())
				{
					path = FindBakedFile(image, exporter.GetDocument(), exporter.GetWriter()) iferr_return;
				}
				if (path.IsEmpty())
				{
					exporter.Warn(FormatString("Material \"@\": the Cinema 4D shader \"@\" could not be baked; its Texture node reads no file.", material->GetName(), from->GetName()));
					continue;
				}
				SetTextureFile(target, path) iferr_return;
				const maxon::GraphNode space = InputOf(target, "tex0_colorspace"_s) iferr_return;
				if (space.IsValid())
				{
					// A baked shader holds the render space's values already.
					space.SetPortValue("RS_INPUT_COLORSPACE_RAW"_s) iferr_return;
				}
				continue;
			}
			CopiedNode* const source = CopyOf(copied, from);
			if (!source)
				continue;
			maxon::Id outId;
			outId.Init(FormatString("@.@", source->asset, out->GetName(from).ToLower())) iferr_return;
			const maxon::GraphNode outputs = source->node.GetOutputs() iferr_return;
			const maxon::GraphNode outPort = outputs.FindChild(outId) iferr_return;
			const maxon::GraphNode inPort = InputOf(target, name) iferr_return;
			if (!outPort.IsValid() || !inPort.IsValid())
			{
				exporter.Warn(FormatString("Material \"@\": the wire from \"@\" into \"@\" of \"@\" is not translated.", material->GetName(), from->GetName(), name,
					target.source->GetName()));
				continue;
			}
			outPort.Connect(inPort) iferr_return;
		}
	}
	return maxon::OK;
}

maxon::Result<maxon::Bool> BuildRedshiftShaderGraph(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, NodeOutput& surface)
{
	iferr_scope;
	if (!material || material->GetType() != REDSHIFT_SHADER_GRAPH_MATERIAL)
		return false;
	cinema::GvNode* const root = GraphRoot(material) iferr_return;
	if (!root)
		return false;

	// A material of the node space that stands for this one while it is translated; never inserted into a document.
	cinema::BaseMaterial* stand = cinema::BaseMaterial::Alloc(Mmaterial);
	if (!stand)
		return false;
	finally
	{
		cinema::BaseMaterial::Free(stand);
	};
	stand->SetName(material->GetName());
	cinema::NodeMaterial* const nodes = static_cast<cinema::NodeMaterial*>(stand);
	const maxon::Id space("com.redshift3d.redshift4c4d.class.nodespace");
	nodes->CreateEmptyGraph(space) iferr_return;
	const maxon::nodes::NodesGraphModelRef model = nodes->GetGraph(space) iferr_return;
	if (!model)
		return false;
	{
		maxon::GraphTransaction transaction = model.BeginTransaction() iferr_return;
		maxon::BaseArray<CopiedNode> copied;
		maxon::BaseArray<cinema::GvNode*> shaders;
		CopyNodes(exporter, material, root, model, copied, shaders) iferr_return;
		CopyWires(exporter, material, copied) iferr_return;
		transaction.Commit() iferr_return;
	}
	return BuildRedshiftSurface(exporter, graph, stand, surface);
}

} // namespace cyclesrender
