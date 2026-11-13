// SPDX-License-Identifier: Apache-2.0
// The buttons "Make Relative" and "Make Absolute" under the file of an Image Texture and an
// Environment Texture node: commands of the maxon command system.
//
// A node description can hold a button that runs a command (an entry with the command type
// "button" in its layout, as Maxon's own Image node has "Reload Image"; tools/make_nodes.py
// writes ours). Pressing it invokes the command registered under the entry's id in
// maxon::CommandClasses, with the node's graph and path in its data (ARGUMENTS::NODECORE).
// Both commands are registered in path_commands.cpp; they need no plugin id.
//
// Relative means relative to the folder of the document the material is in, stored as a Url of
// URLSCHEME_RELATIVE ("tex/wood.png", "../maps/wood.png"); the exporter joins it to the folder
// again (MaterialExporter::ResolveImageUrl). A document that was never saved has no folder: the
// commands then change nothing and say so in the console and the status bar.

#ifndef CYCLESRENDER_PATH_COMMANDS_H__
#define CYCLESRENDER_PATH_COMMANDS_H__

#include "c4d.h"
#include "maxon/graph.h"

namespace cyclesrender
{

/// The ids the two commands are registered under (and the description's buttons name).
#define CYCLES_COMMAND_PATH_RELATIVE "com.wolfnimations.cyclesrender.command.path.relative"
#define CYCLES_COMMAND_PATH_ABSOLUTE "com.wolfnimations.cyclesrender.command.path.absolute"

//----------------------------------------------------------------------------------------
/// What either button does to one Url, without a graph: @p url made relative to @p folder (or
/// absolute again, joined to it). Exposed for the IPR test hook.
/// @param[in] folder    The document's folder; empty for a document that was never saved.
/// @param[out] problem  Why nothing changed, in words for the user, or empty.
/// @return              The new Url, or @p url itself when nothing changed.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Url> ChangePathKind(const maxon::Url& url, const maxon::Url& folder, maxon::Bool relative, maxon::String& problem);

//----------------------------------------------------------------------------------------
/// Runs a button's command as pressing it does: through maxon::CommandClasses, with @p graph
/// and the path of @p node as its data. For the IPR test hook (a test cannot press it).
/// @return  What the command reported: "ok", "skip" or "break".
//----------------------------------------------------------------------------------------
maxon::Result<maxon::String> InvokePathCommand(maxon::Bool relative, const maxon::GraphModelRef& graph, const maxon::GraphNode& node);

} // namespace cyclesrender

#endif // CYCLESRENDER_PATH_COMMANDS_H__
