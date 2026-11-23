// SPDX-License-Identifier: Apache-2.0
// What a Cycles Proxy object (objects/proxy_object.h) shows: the Blender file it names, found on disk,
// and what a Blender said that file holds.
//
// The plugin never opens a .blend file. It sends Blender the file's path (the `proxy` scene item,
// protocol/protocol.md), and what it shows the user about the file (the scenes and collections to
// choose from, the box in the viewport) it has asked a Blender for (the `blend_info` operation), as
// the Preferences page asks one for its devices (prefs/devices.h).

#ifndef CYCLESRENDER_PROXY_EXPORT_H__
#define CYCLESRENDER_PROXY_EXPORT_H__

#include "c4d.h"

namespace cyclesrender
{

/// The edge of the box a proxy is drawn as while no Blender has measured its file, in metres: it
/// stands on the object's origin, as a Blender scene stands on its floor.
static const maxon::Float64 PROXY_DEFAULT_BOX = 2.0;

/// Whether @p proxy is switched on by its green check in the Object Manager (the object's deform
/// mode, as for the Cycles Fog). The check counts for the proxy itself only, not for the objects below it.
maxon::Bool IsProxySwitchedOn(const cinema::BaseObject* proxy);

/// What a `proxy` item says besides its place: everything of a Cycles Proxy that Blender needs.
struct ProxySource
{
	maxon::String path;          ///< the .blend file, absolute, as found on disk
	maxon::String stamp;         ///< that file's date (ProxyFileStamp): another one makes Blender link the file anew
	maxon::Int32 kind = 0;       ///< CYCLESPROXY_TAKE_KIND_...
	maxon::String name;          ///< the scene's or collection's name in the file
	maxon::Bool lights = true;   ///< the file's lights are taken
	maxon::Int32 frame = 0;      ///< the frame the file's animation is at: the document's plus the object's offset
	maxon::String label;         ///< the object's name, for messages
};

//----------------------------------------------------------------------------------------
/// Reads what @p proxy shows at the document's time. The file is looked for as a bitmap's is: as it
/// is named, next to the document, in its "tex" folder, in the search paths (GenerateTexturePath).
/// @param[out] problem  Why there is nothing to show, in a sentence for the console (no file chosen,
///                      the file not found), or empty.
/// @return              False when there is nothing to show.
//----------------------------------------------------------------------------------------
maxon::Bool ReadProxySource(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc, ProxySource& source, maxon::String& problem);

/// The date a file was last written, as text ("20261124-101112"), or empty when it cannot be read.
maxon::String ProxyFileStamp(const cinema::Filename& file);

/// One scene or collection of a Blender file, as a Blender described it (ReadProxyFile).
struct ProxyContentEntry
{
	maxon::Bool collection = false;   ///< a collection; else a scene
	maxon::String name;
	maxon::Int32 objects = 0;         ///< the objects with a size it shows
	maxon::Bool hasBox = false;       ///< false when it shows nothing with a size
	cinema::Vector boxMin;            ///< the box around them, in Blender's axes (Z up) and metres
	cinema::Vector boxMax;
};

/// What the object holds of its file: the entries, scenes first, each kind in Blender's order (by name).
struct ProxyContent
{
	maxon::BaseArray<ProxyContentEntry> entries;
	maxon::String file;      ///< the file it was read from, absolute; empty: never read
	maxon::String stamp;     ///< that file's date then
	maxon::String blender;   ///< the Blender that read it
};

/// The content stored in @p proxy (empty when its file was never read).
maxon::Result<ProxyContent> GetProxyContent(const cinema::BaseObject* proxy);

/// How the stored content stands to the file the object names now.
enum class PROXYCONTENT
{
	NONE,         ///< never read
	CURRENT,      ///< read from this file as it is on disk
	CHANGED,      ///< the file was written since
	OTHER_FILE,   ///< read from another file than the one named now
	FILE_MISSING  ///< the file named now is not there
} MAXON_ENUM_LIST(PROXYCONTENT);
PROXYCONTENT GetProxyContentState(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc);

/// The line under "Read File": what was read and from which Blender, and whether it still describes the file.
maxon::String DescribeProxyContent(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc);

//----------------------------------------------------------------------------------------
/// Asks a Blender what the file of @p proxy holds and stores the answer in the object (in one undo
/// step when the object is in a document): starts a Blender in the background (about 2 s), sends it
/// `blend_info` and ends it. Blocks: call it from the object's button, on the main thread. Blender
/// runs with --factory-startup and without the file's scripts (session/blender_process.cpp).
/// @return  What was read, in a sentence for the status bar.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::String> ReadProxyFile(cinema::BaseObject* proxy, cinema::BaseDocument* doc);

//----------------------------------------------------------------------------------------
/// The box @p proxy is drawn as, in its own axes and the document's units: that of the scene or
/// collection it shows, when the stored content knows it, else a cube of PROXY_DEFAULT_BOX standing
/// on the origin.
/// @param[out] centre, radius  The box.
/// @return                     True when the box is the file's own.
//----------------------------------------------------------------------------------------
maxon::Bool GetProxyBox(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc, cinema::Vector& centre, cinema::Vector& radius);

/// For tests: the object's content, its state and its box as JSON: {"state", "status", "file", "stamp",
/// "blender", "entries": [{"kind", "name", "objects", "min", "max"}], "box": {"own", "centre", "radius"},
/// "source": {the fields of ProxySource, or "problem"}}.
maxon::String DescribeProxy(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc);

} // namespace cyclesrender

#endif // CYCLESRENDER_PROXY_EXPORT_H__
