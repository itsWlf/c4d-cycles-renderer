// SPDX-License-Identifier: Apache-2.0
// The buttons "Make Relative" and "Make Absolute" of the image nodes (path_commands.h). Reading
// order: ChangePathKind (the path arithmetic), then the command both buttons run.

#include "c4d.h"
#include "maxon/commandbase.h"
#include "maxon/configuration.h"
#include "maxon/datadescription_nodes.h"
#include "maxon/nodesgraph.h"

#include "path_commands.h"

// For finding out what a press of the button hands over (docs/reference/c4d-sdk.md).
MAXON_CONFIGURATION_BOOL(g_cyclesRenderLogPathCommands, false, maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: write to the console what the image nodes' path buttons receive.");

namespace cyclesrender
{

// The port of the file on both image nodes (tools/make_nodes.py).
static const maxon::Char* const FILE_PORT = "prop.image_file";

// Windows file names do not tell capitals from small letters.
static maxon::Bool SameName(const maxon::String& a, const maxon::String& b)
{
	return a.ToLower() == b.ToLower();
}

maxon::Result<maxon::Url> ChangePathKind(const maxon::Url& url, const maxon::Url& folder, maxon::Bool relative, maxon::String& problem)
{
	iferr_scope;
	problem = maxon::String();
	if (!url.IsPopulated())
	{
		problem = "the node names no file."_s;
		return url;
	}
	const maxon::Bool isRelative = url.GetScheme() == maxon::URLSCHEME_RELATIVE;
	if (!isRelative && url.GetScheme() != maxon::URLSCHEME_FILESYSTEM)
	{
		problem = FormatString("\"@\" is no file on a disk (a file of the Asset Browser?); its path cannot be changed.", url.ToString(nullptr));
		return url;
	}
	if (!folder.IsPopulated())
	{
		problem = "the document has never been saved, so it has no folder a path could be relative to. Save it first."_s;
		return url;
	}

	if (!relative)
	{
		if (!isRelative)
		{
			problem = "the path is absolute already."_s;
			return url;
		}
		maxon::Url joined = (folder + url) iferr_return;
		joined = joined.Normalize() iferr_return;
		return joined;
	}

	if (isRelative)
	{
		problem = "the path is relative already."_s;
		return url;
	}
	// Both as their parts: a drive ("C:") and folders, or a server and its shares.
	if (url.GetAuthority() != folder.GetAuthority())
	{
		problem = "the file is on another server than the document; its path stays absolute."_s;
		return url;
	}
	maxon::BaseArray<maxon::String> fileParts;
	url.GetComponents(fileParts) iferr_return;
	maxon::BaseArray<maxon::String> folderParts;
	folder.GetComponents(folderParts) iferr_return;
	if (fileParts.IsEmpty() || folderParts.IsEmpty() || !SameName(fileParts[0], folderParts[0]))
	{
		problem = "the file is on another drive than the document; its path stays absolute."_s;
		return url;
	}
	maxon::Int common = 0;
	while (common < folderParts.GetCount() && common < fileParts.GetCount() - 1 && SameName(fileParts[common], folderParts[common]))
		++common;
	maxon::String path;
	for (maxon::Int i = common; i < folderParts.GetCount(); ++i)
		path += "../"_s;
	for (maxon::Int i = common; i < fileParts.GetCount(); ++i)
		path += fileParts[i] + (i + 1 < fileParts.GetCount() ? "/"_s : maxon::String());
	// Without a drive letter a Url is relative (URLSCHEME_RELATIVE).
	maxon::Url result(path);
	if (result.GetScheme() != maxon::URLSCHEME_RELATIVE)
	{
		result.SetScheme(maxon::URLSCHEME_RELATIVE) iferr_return;
	}
	return result;
}

// What both buttons do: every image node the command's data names gets its file's path changed.
static maxon::Result<maxon::COMMANDRESULT> ExecutePathCommand(maxon::CommandDataRef& data, maxon::Bool relative)
{
	iferr_scope;
	const maxon::Char* const name = relative ? "Make Relative" : "Make Absolute";
	const maxon::GraphModelRef graph = data.Get(maxon::ARGUMENTS::NODECORE::GRAPHMODEL, maxon::GraphModelRef());
	maxon::BaseArray<maxon::NodePath> paths;
	const maxon::NodePath path = data.Get(maxon::ARGUMENTS::NODECORE::NODEPATH, maxon::NodePath());
	if (!path.IsEmpty())
	{
		paths.Append(path) iferr_return;
	}
	for (const maxon::NodePath& listed : data.Get(maxon::ARGUMENTS::NODECORE::NODEPATHLIST, maxon::BaseArray<maxon::NodePath>()))
	{
		paths.Append(listed) iferr_return;
	}
	// The SDK calls this one "mandatory for all button messages" (datadescription_nodes.h): the
	// port a button belongs to; its node is the one to change.
	if (paths.IsEmpty())
	{
		const maxon::NodePath port = data.Get(maxon::ARGUMENTS::NODECORE::INNERPORTPATH, maxon::NodePath());
		if (!port.IsEmpty())
		{
			paths.Append(port) iferr_return;
		}
	}
	if (g_cyclesRenderLogPathCommands)
		ApplicationOutput("Cycles Render: \"@\" pressed: graph @, @ node path(s).", maxon::String(name), graph ? "given"_s : "missing"_s, paths.GetCount());
	if (!graph || paths.IsEmpty())
	{
		// Cinema 4D did not hand over the node: nothing can be done, and the console says so.
		ApplicationOutput("Cycles Render: \"@\" got no node to work on (graph @, @ path(s)).", maxon::String(name), graph ? "given"_s : "missing"_s,
			paths.GetCount());
		return maxon::COMMANDRESULT::SKIP;
	}

	// The folder of the document the Node Editor shows the material of.
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	const cinema::Filename folderName = doc ? doc->GetDocumentPath() : cinema::Filename();
	const maxon::Url folder = folderName.IsPopulated() ? cinema::MaxonConvert(folderName, cinema::MAXONCONVERTMODE::NONE) : maxon::Url();

	maxon::Id portId;
	portId.Init(maxon::String(FILE_PORT)) iferr_return;
	maxon::COMMANDRESULT result = maxon::COMMANDRESULT::SKIP;
	for (const maxon::NodePath& nodePath : paths)
	{
		maxon::GraphNode node = graph.GetNode(nodePath);
		if (!node.IsValid())
			continue;
		if (node.GetKind() != maxon::NODE_KIND::NODE)
		{
			node = node.GetAncestor(maxon::NODE_KIND::NODE) iferr_return;
		}
		const maxon::GraphNode inputs = node.GetInputs() iferr_return;
		const maxon::GraphNode port = inputs.FindChild(portId) iferr_return;
		if (!port.IsValid())
			continue;
		const maxon::Url url = port.GetEffectivePortValue<maxon::Url>().GetValueOr(maxon::Url()) iferr_return;
		maxon::String problem;
		const maxon::Url changed = ChangePathKind(url, folder, relative, problem) iferr_return;
		if (problem.IsPopulated())
		{
			const maxon::String message = FormatString("Cycles Render: @: @", maxon::String(name), problem);
			ApplicationOutput("@", message);
			cinema::StatusSetText(message);
			continue;
		}
		maxon::GraphTransaction transaction = graph.BeginTransaction() iferr_return;
		port.SetPortValue(changed) iferr_return;
		transaction.Commit() iferr_return;
		const maxon::String done = FormatString("Cycles Render: @: @", maxon::String(name),
			changed.GetScheme() == maxon::URLSCHEME_RELATIVE ? changed.GetPath() : changed.GetSystemPath().GetValueOr(changed.GetPath()));
		ApplicationOutput("@", done);
		cinema::StatusSetText(done);
		result = maxon::COMMANDRESULT::OK;
	}
	if (result == maxon::COMMANDRESULT::OK)
		cinema::EventAdd();
	return result;
}

// Both buttons can always be pressed; what cannot be done is said when pressed.
static maxon::COMMANDSTATE PathCommandState(maxon::CommandDataRef& data)
{
	if (g_cyclesRenderLogPathCommands)
		ApplicationOutput("Cycles Render: a path button was asked for its state (graph @).", data.Get(maxon::ARGUMENTS::NODECORE::GRAPHMODEL, maxon::GraphModelRef()) ? "given"_s : "missing"_s);
	return maxon::COMMANDSTATE::ENABLED;
}

class MakeRelativeCommand : public maxon::Component<MakeRelativeCommand, maxon::CommandClassInterface>
{
	MAXON_COMPONENT();

public:
	MAXON_METHOD maxon::Result<maxon::COMMANDSTATE> GetState(maxon::CommandDataRef& data) const { return PathCommandState(data); }
	MAXON_METHOD maxon::Result<maxon::COMMANDRESULT> Execute(maxon::CommandDataRef& data) const { return ExecutePathCommand(data, true); }
};

class MakeAbsoluteCommand : public maxon::Component<MakeAbsoluteCommand, maxon::CommandClassInterface>
{
	MAXON_COMPONENT();

public:
	MAXON_METHOD maxon::Result<maxon::COMMANDSTATE> GetState(maxon::CommandDataRef& data) const { return PathCommandState(data); }
	MAXON_METHOD maxon::Result<maxon::COMMANDRESULT> Execute(maxon::CommandDataRef& data) const { return ExecutePathCommand(data, false); }
};

MAXON_COMPONENT_OBJECT_REGISTER(MakeRelativeCommand, maxon::CommandClasses, CYCLES_COMMAND_PATH_RELATIVE);
MAXON_COMPONENT_OBJECT_REGISTER(MakeAbsoluteCommand, maxon::CommandClasses, CYCLES_COMMAND_PATH_ABSOLUTE);

maxon::Result<maxon::String> InvokePathCommand(maxon::Bool relative, const maxon::GraphModelRef& graph, const maxon::GraphNode& node)
{
	iferr_scope;
	maxon::CommandDataRef data = maxon::CommandDataClasses::BASE().Create() iferr_return;
	data.Set(maxon::ARGUMENTS::NODECORE::GRAPHMODEL, graph) iferr_return;
	data.Set(maxon::ARGUMENTS::NODECORE::NODEPATH, node.GetPath()) iferr_return;
	const maxon::COMMANDRESULT result = data.Invoke(maxon::Id(relative ? CYCLES_COMMAND_PATH_RELATIVE : CYCLES_COMMAND_PATH_ABSOLUTE), false) iferr_return;
	return result == maxon::COMMANDRESULT::OK ? "ok"_s : result == maxon::COMMANDRESULT::SKIP ? "skip"_s : "break"_s;
}

} // namespace cyclesrender
