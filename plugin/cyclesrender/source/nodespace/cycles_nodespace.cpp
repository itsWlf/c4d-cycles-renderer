// SPDX-License-Identifier: Apache-2.0
// The "Cycles" node space: Blender's shader nodes as Cinema 4D nodes.
//
// The nodes are a description, not a shader: nothing in Cinema 4D evaluates them. A material
// built from them is written to Blender node for node, link for link and value for value
// (materials/cycles_node_material.cpp). Every node comes from one table generated out of
// Blender itself (cycles_nodes.h, tools/make_nodes.py), so there is one node here for every
// shader node Blender has, with Blender's sockets, names and defaults.
//
// A node is made twice over, from the same table: its ports are built in code (CyclesNodeTemplate),
// and how it looks comes from a description database under the node's id (res/nodes/*.json, also
// written by tools/make_nodes.py): which ports show on the node, the foldable groups, the labels
// of the dropdowns, the folder in the Asset Browser.

#include "c4d.h"
#include "maxon/application.h"
#include "maxon/assets.h"
#include "maxon/category_asset.h"
#include "maxon/datadescription_string.h"
#include "maxon/datadescriptiondatabase.h"
#include "maxon/derived_nodeattributes.h"
#include "maxon/datadescriptiondefinitiondatabase.h"
#include "maxon/stringresource.h"
#include "maxon/datadescription_nodes.h"
#include "maxon/node_spaces.h"
#include "maxon/nodes_corenodes_base.h"
#include "maxon/nodeslib.h"
#include "maxon/nodesystem_class.h"
#include "maxon/nodetemplate.h"
#include "maxon/previewimageprovider.h"
#include "maxon/spinlock.h"
#include "maxon/splinemapper.h"

#include "../ids.h"
#include "../materials/material_preview.h"
#include "cycles_nodes.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
// The kind of node system the space's graphs are: it decides which nodes may be used in them.
//----------------------------------------------------------------------------------------
class CyclesNodeSystemClass : public maxon::Component<CyclesNodeSystemClass, maxon::nodes::NodeSystemClassInterface>
{
	MAXON_COMPONENT(FINAL, maxon::nodes::BaseCoreNodesNodeSystemClass().GetClass());

public:
	MAXON_METHOD maxon::Result<maxon::Bool> SupportsImpl(const maxon::nodes::NodeTemplate& templ) const
	{
		// Only the nodes below; none of Cinema 4D's own, which Blender would not know.
		return super.SupportsImpl(templ);
	}
};

// Header colours after Blender's node categories.
static maxon::Color TagColor(const maxon::Char* tag)
{
	const maxon::CString name(tag);
	if (name == "SHADER"_cs)
		return maxon::Color(0.17, 0.52, 0.27);
	if (name == "TEXTURE"_cs)
		return maxon::Color(0.75, 0.38, 0.12);
	if (name == "INPUT"_cs)
		return maxon::Color(0.55, 0.18, 0.26);
	if (name == "OUTPUT"_cs)
		return maxon::Color(0.42, 0.10, 0.13);
	if (name == "COLOR"_cs)
		return maxon::Color(0.50, 0.48, 0.12);
	if (name == "VECTOR"_cs)
		return maxon::Color(0.28, 0.27, 0.58);
	if (name == "CONVERTER"_cs)
		return maxon::Color(0.15, 0.42, 0.55);
	return maxon::Color(0.35);
}

static maxon::Result<maxon::Id> MakeId(const maxon::Char* text)
{
	iferr_scope;
	maxon::Id id;
	id.Init(maxon::String(text)) iferr_return;
	return id;
}

//----------------------------------------------------------------------------------------
// One node of the table. Its ports are created when the node is put into a graph.
//----------------------------------------------------------------------------------------
class CyclesNodeTemplate : public maxon::Component<CyclesNodeTemplate, maxon::nodes::NodeTemplateInterface>
{
	MAXON_COMPONENT(NORMAL, maxon::nodes::NodeTemplateBaseClass);

public:
	void SetNode(const CyclesNode* node) const { _node = node; }

	MAXON_METHOD maxon::Result<maxon::Bool> SupportsImpl(const maxon::nodes::NodeSystemClass& cls) const
	{
		return cls.GetClass() == CyclesNodeSystemClass::GetClass();
	}

	MAXON_METHOD maxon::Result<maxon::nodes::NodeSystem> InstantiateImpl(const maxon::nodes::InstantiationTrace& parent, const maxon::nodes::TemplateArguments& args) const
	{
		iferr_scope;
		if (!_node)
			return maxon::IllegalStateError(MAXON_SOURCE_LOCATION);

		maxon::nodes::MutableRoot root = parent.CreateNodeSystem() iferr_return;
		root.SetValue(maxon::NODE::BASE::NAME, maxon::String(_node->label)) iferr_return;
		root.SetValue(maxon::NODE::BASE::COLOR, TagColor(_node->tag)) iferr_return;
		// A preview is the whole material's ball, whatever the node: it belongs on the material's
		// thumbnail, not on every node.
		root.SetValue(maxon::NODE::BASE::DISPLAYPREVIEW, false) iferr_return;

		maxon::BaseArray<maxon::nodes::MutablePort> outputs;
		for (maxon::Int i = 0; i < _node->outputCount; ++i)
		{
			const CyclesSocket& socket = _node->outputs[i];
			const maxon::Id id = MakeId(socket.port) iferr_return;
			maxon::nodes::MutablePort port = root.GetOutputs().AddPort(id) iferr_return;
			SetSocketType(port, socket.kind) iferr_return;
			port.SetValue(maxon::NODE::BASE::NAME, maxon::String(socket.label)) iferr_return;
			// Its value would be computed by the renderer, not by Cinema 4D.
			port.SetValue(maxon::nodes::Dynamic, true) iferr_return;
			outputs.Append(port) iferr_return;
		}

		maxon::Int order = 0;
		for (maxon::Int i = 0; i < _node->inputCount; ++i)
		{
			const CyclesSocket& socket = _node->inputs[i];
			const maxon::Id id = MakeId(socket.port) iferr_return;
			maxon::nodes::MutablePort port = root.GetInputs().AddPort(id) iferr_return;
			SetSocketType(port, socket.kind) iferr_return;
			port.SetValue(maxon::NODE::BASE::NAME, maxon::String(socket.label)) iferr_return;
			switch (socket.kind)
			{
				case 'f': port.SetPortValue(maxon::Float(socket.value[0])) iferr_return; break;
				case 'c': port.SetPortValue(maxon::Color(socket.value[0], socket.value[1], socket.value[2])) iferr_return; break;
				case 'v': port.SetPortValue(maxon::Vector(socket.value[0], socket.value[1], socket.value[2])) iferr_return; break;
				case 'b': port.SetPortValue(socket.value[0] != 0.0) iferr_return; break;
				case 'i': port.SetPortValue(maxon::Int(socket.value[0])) iferr_return; break;
				case 't': port.SetPortValue(maxon::String(socket.text)) iferr_return; break;
				default: break;   // a shader input has no value of its own
			}
			for (const maxon::nodes::MutablePort& output : outputs)
			{
				port.Connect(output, maxon::Wires(maxon::Wires::DEPENDENCY)) iferr_return;
			}
			port.SetValue(maxon::nodes::ReqOrderIndex, ++order) iferr_return;
		}

		// The settings on the node itself: values only, nothing can be wired into them.
		for (maxon::Int i = 0; i < _node->propertyCount; ++i)
		{
			const CyclesProperty& property = _node->properties[i];
			const maxon::Id id = MakeId(property.port) iferr_return;
			maxon::nodes::MutablePort port = root.GetInputs().AddPort(id) iferr_return;
			port.SetValue(maxon::NODE::BASE::NAME, maxon::String(property.label)) iferr_return;
			switch (property.kind)
			{
				case 'e':
				{
					// "IDENTIFIER|Label;IDENTIFIER|Label": the options are Blender's identifiers.
					maxon::BaseArray<maxon::Tuple<maxon::Id, maxon::Data>> options;
					maxon::String(property.items).Split(";"_s, true, [&options](const maxon::String& item) -> maxon::Result<maxon::Bool>
						{
							iferr_scope;
							maxon::Int bar = 0;
							const maxon::String identifier = item.Find("|"_s, &bar) ? item.GetPart(0, bar) : item;
							maxon::Tuple<maxon::Id, maxon::Data> option;
							option.first.Init(identifier) iferr_return;
							option.second.Set(option.first) iferr_return;
							options.Append(std::move(option)) iferr_return;
							return true;
						}) iferr_return;
					// The options are in the description of the node; the port itself holds an id.
					port.SetType<maxon::Id>() iferr_return;
					const maxon::Id chosen = MakeId(property.text) iferr_return;
					port.SetPortValue(chosen) iferr_return;
					break;
				}
				case 'b': port.SetType<maxon::Bool>() iferr_return; port.SetPortValue(property.number != 0.0) iferr_return; break;
				case 'i': port.SetType<maxon::Int>() iferr_return; port.SetPortValue(maxon::Int(property.number)) iferr_return; break;
				case 'f': port.SetType<maxon::Float>() iferr_return; port.SetPortValue(maxon::Float(property.number)) iferr_return; break;
				case 'c':
				{
					const maxon::Vector value = GetCyclesPropertyVector(property);
					SetSocketType(port, 'c') iferr_return;
					port.SetPortValue(maxon::Color(value.x, value.y, value.z)) iferr_return;
					break;
				}
				case 'v':
				{
					SetSocketType(port, 'v') iferr_return;
					port.SetPortValue(GetCyclesPropertyVector(property)) iferr_return;
					break;
				}
				case 'u': port.SetType<maxon::Url>() iferr_return; break;
				case 'k':
				{
					port.SetType<maxon::SplineMapper>() iferr_return;
					const maxon::SplineMapper curve = CreateCyclesCurve() iferr_return;
					port.SetPortValue(curve) iferr_return;
					break;
				}
				default: port.SetType<maxon::String>() iferr_return; port.SetPortValue(maxon::String(property.text)) iferr_return; break;
			}
			port.SetValue(maxon::nodes::ConstantParameter, true) iferr_return;
			port.SetValue(maxon::nodes::ReqOrderIndex, ++order) iferr_return;
		}

		root.SetTemplate(self, args) iferr_return;
		return root.EndModification();
	}

private:
	static maxon::Result<void> SetSocketType(const maxon::nodes::MutablePort& port, maxon::Char kind)
	{
		switch (kind)
		{
			case 'f': return port.SetType<maxon::Float>();
			case 'v': return port.SetType<maxon::Vector>();
			case 'b': return port.SetType<maxon::Bool>();
			case 'i': return port.SetType<maxon::Int>();
			case 't': return port.SetType<maxon::String>();
			default: return port.SetType<maxon::Color>();   // colours, and shaders (closures)
		}
	}

	mutable const CyclesNode* _node = nullptr;   // set once, right after the template is made
};

//----------------------------------------------------------------------------------------
// The picture of a material: Cinema 4D asks for it (the thumbnail in the Material Manager and
// in the Attribute Manager's header, 90 x 90; the big preview in the Attribute Manager, 120 x
// 120; the preview on the Material Output node) and gets a ball rendered by Blender
// (materials/material_preview.h). Initialize() runs while the graph may be read and only
// writes the material down; the render happens in ComputeIteration(), on a job thread.
//----------------------------------------------------------------------------------------
class CyclesPreviewProvider : public maxon::Component<CyclesPreviewProvider, maxon::nodes::PreviewImageProviderInterface>
{
	MAXON_COMPONENT(NORMAL);

public:
	MAXON_METHOD maxon::Result<void> Initialize(const maxon::DataDictionaryObjectRef& request, maxon::Int numThreads)
	{
		iferr_scope;
		_size = request.Get(maxon::nodes::PREVIEWIMAGEREQUEST::TEXTURESIZE, maxon::IntVector2d());
		_done = false;
		_result = nullptr;
		// The SDK says the flip of a request for a flipped picture "is performed automatically on
		// the provided buffer". In Cinema 4D 2027.0 it is not: the Attribute Manager's big preview
		// (120 x 120, the only request seen with the flag) showed the ball upside down while the
		// thumbnails (90 x 90, without it) were right. So the provider flips it itself.
		_flip = request.Get(maxon::nodes::PREVIEWIMAGEREQUEST::FLIPVERTICALLY, false);
		const maxon::GraphNode subject = request.Get(maxon::nodes::PREVIEWIMAGEREQUEST::SUBJECT, maxon::GraphNode());
		if (!subject.IsValid() || _size.x <= 0 || _size.y <= 0)
			return maxon::IllegalArgumentError(MAXON_SOURCE_LOCATION, "a preview was asked for without a node or a size"_s);
		_preview = NewObj(MaterialPreview) iferr_return;
		return _preview->Prepare(subject.GetGraph());
	}

	MAXON_METHOD maxon::Result<void> ComputeIteration(const maxon::JobRef& parentThread)
	{
		iferr_scope;
		if (_done || !_preview || parentThread.IsCancelled())
			return maxon::OK;

		// A square picture of the longer side, then cut or repeated to the size asked for.
		const maxon::Int side = maxon::ClampValue(maxon::Max(_size.x, _size.y), maxon::Int(32), maxon::Int(512));
		maxon::BaseArray<maxon::Float32> pixels;
		iferr (_preview->Render(side, pixels))
		{
			// No picture (no Blender, no bridge, or Blender refused the material): a dark one, and
			// the reason in the console, once for each reason.
			ReportOnce(err.GetMessage());
			pixels.Resize(side * side * 4) iferr_return;
			for (maxon::Float32& value : pixels)
				value = 0.02f;
		}
		_preview = nullptr;

		maxon::nodes::SamplesFloat32Ref samples = NewObj(maxon::nodes::SamplesFloat32) iferr_return;
		samples->Resize(_size.x * _size.y) iferr_return;
		for (maxon::Int y = 0; y < _size.y; ++y)
		{
			// The top row first; the bottom row first for a request for a flipped picture (Initialize()).
			const maxon::Int pictureY = _flip ? _size.y - 1 - y : y;
			const maxon::Int sourceY = maxon::ClampValue(pictureY * side / _size.y, maxon::Int(0), side - 1);
			for (maxon::Int x = 0; x < _size.x; ++x)
			{
				const maxon::Int sourceX = maxon::ClampValue(x * side / _size.x, maxon::Int(0), side - 1);
				const maxon::Float32* const source = pixels.GetFirst() + (sourceY * side + sourceX) * 4;
				// Blender's picture has the ball over nothing, as colour times alpha. Cinema 4D draws
				// a checkerboard where a thumbnail's alpha is below 1 (tried), so the ball is put
				// over a grey here.
				const maxon::Float32 rest = BACKGROUND_GREY * (1.0f - maxon::ClampValue(source[3], 0.0f, 1.0f));
				(*samples)[y * _size.x + x] = maxon::nodes::SampleFloat32(source[0] + rest, source[1] + rest, source[2] + rest, 1.0f);
			}
		}
		_result = std::move(samples);
		_done = true;
		return maxon::OK;
	}

	MAXON_METHOD maxon::Result<maxon::nodes::PreviewImageProviderOutput> TakeOutput()
	{
		iferr_scope;
		maxon::nodes::PreviewImageProviderOutput output;
		output._iterationCount = 1;
		output._currentIterationIndex = 0;
		output._isFinal = true;
		maxon::nodes::PreviewImageProviderOutputImage image;
		image._imageSize = _size;
		image._samplesFloat32Linear = _result;
		image._isFinal = true;
		output.SetResult(image) iferr_return;
		return output;
	}

private:
	// What shows around the ball, as a linear value: a grey a little lighter than Cinema 4D's panels.
	static constexpr maxon::Float32 BACKGROUND_GREY = 0.05f;

	static void ReportOnce(const maxon::String& message)
	{
		static maxon::Spinlock lock;
		static maxon::String last;
		maxon::ScopedLock guard(lock);
		if (message == last)
			return;
		last = message;
		ApplicationOutput("Cycles Render: a material preview could not be rendered. @", message);
	}

	maxon::IntVector2d _size;
	maxon::Bool _flip = false;	///< the request asks for the picture upside down (PREVIEWIMAGEREQUEST::FLIPVERTICALLY)
	maxon::StrongRef<MaterialPreview> _preview;
	maxon::nodes::SamplesFloat32Ref _result;
	maxon::Bool _done = false;
};

MAXON_COMPONENT_CLASS_REGISTER(CyclesPreviewProvider, "com.wolfnimations.cyclesrender.class.previewprovider");

//----------------------------------------------------------------------------------------
// The space itself.
//----------------------------------------------------------------------------------------
class CyclesNodeSpace : public maxon::Component<CyclesNodeSpace, maxon::nodes::NodeSpaceInterface>
{
	MAXON_COMPONENT(NORMAL, maxon::nodes::NodeSpaceBaseClass);

public:
	// A new material: a Principled BSDF wired into a Material Output, as in Blender.
	static maxon::Result<void> CreateMaterialGraph(const maxon::nodes::NodesGraphModelRef& graph)
	{
		iferr_scope;
		const maxon::nodes::NodeTemplate shaderTemplate = maxon::nodes::NodesLib::LoadTemplate(maxon::AssetInterface::GetBuiltinRepository(), maxon::Id(CYCLES_PRINCIPLED_NODE_ID)) iferr_return;
		const maxon::GraphNode shader = graph.AddChild(maxon::Id("principled"), shaderTemplate) iferr_return;
		const maxon::nodes::NodeTemplate outputTemplate = maxon::nodes::NodesLib::LoadTemplate(maxon::AssetInterface::GetBuiltinRepository(), maxon::Id(CYCLES_OUTPUT_NODE_ID)) iferr_return;
		const maxon::GraphNode output = graph.AddChild(maxon::Id("output"), outputTemplate) iferr_return;

		const maxon::GraphNode shaderOutputs = shader.GetOutputs() iferr_return;
		const maxon::GraphNode from = shaderOutputs.FindChild(maxon::Id("out.bsdf")) iferr_return;
		const maxon::GraphNode outputInputs = output.GetInputs() iferr_return;
		const maxon::GraphNode to = outputInputs.FindChild(maxon::Id("in.surface")) iferr_return;
		from.Connect(to) iferr_return;
		return maxon::OK;
	}

	// Every preview of this space is the material's ball from Blender.
	static maxon::Result<void> ConfigurePreviewImageRequest(maxon::DataDictionaryObjectRef request)
	{
		iferr_scope;
		request.Set(maxon::nodes::PREVIEWIMAGEREQUEST::PROVIDER, CyclesPreviewProvider::GetClass()) iferr_return;
		return maxon::OK;
	}

	MAXON_METHOD maxon::Result<void> Init(maxon::DataDictionary spaceData)
	{
		iferr_scope;

		_class = CyclesNodeSystemClass::GetClass().Create() iferr_return;
		spaceData.Set(maxon::nodes::NODESPACE::CREATEMATERIALGRAPHFUNC, maxon::nodes::NODESPACE::CreateMaterialGraphFunc(CreateMaterialGraph)) iferr_return;
		spaceData.Set(maxon::nodes::NODESPACE::CONFIGUREPREVIEWIMAGEREQUESTFUNC,
			maxon::nodes::NODESPACE::ConfigurePreviewImageRequestFunc(ConfigurePreviewImageRequest)) iferr_return;

		maxon::BaseArray<maxon::Id> endNodes;
		endNodes.Append(maxon::Id(CYCLES_OUTPUT_NODE_ID)) iferr_return;
		spaceData.Set(maxon::nodes::NODESPACE::MATERIALENDNODEIDS, std::move(endNodes)) iferr_return;
		spaceData.Set(maxon::nodes::NODESPACE::NODESYSTEMCLASS, _class) iferr_return;
		// An image file dropped on the Node Editor (from the file browser or the Asset Browser)
		// becomes an Image Texture node naming it. Cinema 4D wants four ports of that node (SDK,
		// "Implementing Texture Nodes"): the colour output, the file, and the first and last
		// frame of a sequence, which the node has as hidden settings (tools/make_nodes.py).
		spaceData.Set(maxon::nodes::NODESPACE::IMAGENODEASSETID, maxon::Id(CYCLES_IMAGE_NODE_ID)) iferr_return;
		const auto portPath = [](const maxon::Char* port) -> maxon::Result<maxon::NodePath>
		{
			iferr_scope;
			maxon::InternedId id;
			id.Init(maxon::String(port)) iferr_return;
			return maxon::NodePath::Create(maxon::ToSingletonBlock<maxon::InternedId>(id));
		};
		maxon::nodes::NODESPACE::ImageNodePortTuple imagePorts;
		imagePorts.first = portPath("out.color") iferr_return;
		imagePorts.second = portPath("prop.image_file") iferr_return;
		imagePorts.third = portPath("prop.image_frame_start") iferr_return;
		imagePorts.fourth = portPath("prop.image_frame_end") iferr_return;
		spaceData.Set(maxon::nodes::NODESPACE::IMAGENODEPORTS, std::move(imagePorts)) iferr_return;
		// The Material Output is previewed as a material (a ball, not a flat picture).
		maxon::BaseArray<maxon::Id> previewNodes;
		previewNodes.Append(maxon::Id(CYCLES_OUTPUT_NODE_ID)) iferr_return;
		spaceData.Set(maxon::nodes::NODESPACE::MATERIALPREVIEWIDS, std::move(previewNodes)) iferr_return;

		super.Init(spaceData) iferr_return;
		return maxon::OK;
	}

private:
	maxon::nodes::NodeSystemClass _class;
};

MAXON_COMPONENT_CLASS_REGISTER(CyclesNodeSystemClass, "com.wolfnimations.cyclesrender.class.nodesystemclass");
MAXON_COMPONENT_CLASS_REGISTER(CyclesNodeTemplate, "com.wolfnimations.cyclesrender.class.nodetemplate");
// The class carries the id of the space: Cinema 4D reports the active space by this id, and it is
// the key of a material's graph in scene files.
MAXON_COMPONENT_OBJECT_REGISTER(CyclesNodeSpace, CYCLES_NODESPACE_ID);

// Called whenever a port's description is needed: a socket shows on the node only while the node's
// settings are those Blender shows it for (Mix: the colour inputs for the data type "Color").
static maxon::Result<void> DescribePort(maxon::DataDictionary& description, const maxon::GraphNode& port, const maxon::Id& category)
{
	iferr_scope;
	if (category != maxon::DATADESCRIPTION_CATEGORY_DATA)
		return maxon::OK;
	const maxon::NODE_KIND kind = port.GetKind();
	if (kind != maxon::NODE_KIND::INPORT && kind != maxon::NODE_KIND::OUTPORT)
		return maxon::OK;
	const maxon::GraphNode node = port.GetAncestor(maxon::NODE_KIND::NODE) iferr_return;
	const maxon::IdAndVersion asset = node.GetValue<decltype(maxon::nodes::AssetId)>().GetOrDefault() iferr_return;
	const CyclesNode* const definition = FindCyclesNode(asset.Get<0>());
	if (!definition)
		return maxon::OK;

	const maxon::CString portName = port.GetId().ToCString();
	const maxon::Bool output = kind == maxon::NODE_KIND::OUTPORT;
	const CyclesSocket* const sockets = output ? definition->outputs : definition->inputs;
	const maxon::Int count = output ? definition->outputCount : definition->inputCount;
	for (maxon::Int i = 0; i < count; ++i)
	{
		if (portName != maxon::CString(sockets[i].port) || !sockets[i].show[0])
			continue;
		const maxon::Bool shown = IsCyclesSocketShown(node, *definition, sockets[i]) iferr_return;
		description.Set(maxon::NODE::ATTRIBUTE::HIDEPORTINNODEGRAPH, !shown) iferr_return;
		break;
	}
	return maxon::OK;
}

static maxon::BaseArray<maxon::GenericData> g_registeredNodes;
static maxon::GenericData g_registeredSpace;
static maxon::Bool g_registeredDatabase = false;

// The folders of the nodes in the Asset Browser ("Cycles Nodes" and Blender's menus below it).
// The built-in repository takes no new assets (StoreAsset: unsupported operation); it asks
// loaders like this one for what it holds.
static maxon::Result<maxon::Bool> LoadNodeFolders(const maxon::Block<const maxon::Id>& types, const maxon::Id& aid, maxon::ASSET_FIND_MODE findMode,
	const maxon::BuiltinAssetReceiver& receiver)
{
	iferr_scope;
	if (!types.IsEmpty())
	{
		maxon::Bool wanted = false;
		for (const maxon::Id& type : types)
			wanted = wanted || type == maxon::AssetTypes::Category().GetId();
		if (!wanted)
			return true;
	}
	for (const CyclesNodeFolder& folder : GetCyclesNodeFolders())
	{
		const maxon::Id id = MakeId(folder.id) iferr_return;
		if (!aid.IsEmpty() && id != aid)
		{
			const maxon::Bool prefix = (findMode & maxon::ASSET_FIND_MODE::PREFIX) && id.ToString().StartsWith(aid.ToString());
			if (!prefix)
				continue;
		}
		maxon::CategoryAsset category = maxon::CategoryAssetInterface::Create() iferr_return;
		category.SetId({ id, maxon::Id() }, 0, maxon::AssetInterface::GetBuiltinRepository()) iferr_return;
		maxon::DataDictionary meta;
		meta.Set(maxon::OBJECT::BASE::NAME, maxon::String(folder.label)) iferr_return;
		if (folder.parent)
		{
			const maxon::Id parent = MakeId(folder.parent) iferr_return;
			meta.Set(maxon::ASSETMETADATA::Category, parent) iferr_return;
		}
		const maxon::Bool goOn = receiver(category, meta, maxon::Url()) iferr_return;
		if (!goOn)
			return false;
	}
	return true;
}

MAXON_DECLARATION_REGISTER(maxon::BuiltinAssetLoadersNew, "com.wolfnimations.cyclesrender.nodefolders")
{
	return maxon::BuiltinAssetLoaderNew(LoadNodeFolders);
}

static maxon::Result<void> InitializeCyclesNodeSpace()
{
	iferr_scope_handler
	{
		err.DiagOutput();
		return err;
	};

	// How the nodes look: the description database next to the plugin.
	const maxon::Url nodesUrl = maxon::g_maxon.GetUrl().GetDirectory().Append("res"_s).Append("nodes"_s) iferr_return;
	maxon::DataDescriptionDefinitionDatabaseInterface::RegisterDatabaseWithUrl(maxon::Id(CYCLES_NODES_DATABASE_ID), nodesUrl) iferr_return;
	g_registeredDatabase = true;

	// The nodes.
	for (const CyclesNode& node : GetCyclesNodes())
	{
		const maxon::Id id = MakeId(node.asset) iferr_return;
		maxon::nodes::NodeTemplate nodeTemplate = CyclesNodeTemplate::GetClass().Create() iferr_return;
		CyclesNodeTemplate::Get(nodeTemplate)->SetNode(&node);
		nodeTemplate = nodeTemplate.SetId({ id, maxon::Id() }, 0, maxon::AssetInterface::GetBuiltinRepository()) iferr_return;
		maxon::GenericData registered = maxon::nodes::BuiltinNodes::Register(id, nodeTemplate) iferr_return;
		g_registeredNodes.Append(std::move(registered)) iferr_return;
		maxon::GenericData provider = maxon::nodes::PortDescriptionProviders::Register(id, maxon::nodes::PortDescriptionProvider(DescribePort)) iferr_return;
		g_registeredNodes.Append(std::move(provider)) iferr_return;
	}

	// The space, bound to the renderer: with Cycles Render chosen, new materials are made in it.
	maxon::DataDictionary spaceData;
	spaceData.Set(maxon::nodes::NODESPACE::SPACEID, maxon::Id(CYCLES_NODESPACE_ID)) iferr_return;
	spaceData.Set(maxon::nodes::NODESPACE::SPACENAME, "Cycles"_s) iferr_return;
	maxon::BaseArray<maxon::Int> renderers;
	renderers.Append(maxon::Int(ID_CYCLES_VIDEOPOST)) iferr_return;
	spaceData.Set(maxon::nodes::NODESPACE::RENDERERS, std::move(renderers)) iferr_return;
	spaceData.Set(maxon::nodes::NODESPACE::SPACEKIND, maxon::nodes::NODESPACE::KIND::MATERIAL) iferr_return;

	const maxon::nodes::NodeSpaceRef space = CyclesNodeSpace::CreateInit(spaceData) iferr_return;
	g_registeredSpace = maxon::nodes::MaterialNodeSpaces::Register(maxon::Id(CYCLES_NODESPACE_ID), space) iferr_return;
	return maxon::OK;
}

static void FreeCyclesNodeSpace()
{
	g_registeredSpace = maxon::GenericData();
	g_registeredNodes.Reset();
	if (g_registeredDatabase)
	{
		iferr (maxon::DataDescriptionDefinitionDatabaseInterface::UnregisterDatabase(maxon::Id(CYCLES_NODES_DATABASE_ID)))
			err.DiagOutput();
		g_registeredDatabase = false;
	}
}

MAXON_INITIALIZATION(InitializeCyclesNodeSpace, FreeCyclesNodeSpace);

} // namespace cyclesrender
