// SPDX-License-Identifier: Apache-2.0
#ifndef OCYCLESPROXY_H__
#define OCYCLESPROXY_H__

// Parameter IDs of the Cycles Proxy object. They are stored in scene files: never renumber, and
// never reuse a retired number.
//
// Blocks: 1000 the file and what is taken from it, 1100 what a Blender said the file holds (kept with
// the object so that a scene opens without a Blender; not in the description), 2000 tabs and groups.
// The layout is to be redesigned (internal roadmap, F11): a parameter keeps its number when it moves.
enum
{
	// Tabs and groups
	CYCLESPROXY_GROUP_PROXY = 2000,

	// The file and what is taken from it
	CYCLESPROXY_FILE = 1000,                     // the .blend file, absolute or relative to the document
	CYCLESPROXY_MAKE_RELATIVE = 1001,            // buttons, as the Cycles Environment's (nodespace/path_commands.h)
	CYCLESPROXY_MAKE_ABSOLUTE = 1002,
	CYCLESPROXY_READ_FILE = 1003,                // button: asks a Blender what the file holds (scene/proxy_export.h, ReadProxyFile)
	CYCLESPROXY_TAKE = 1004,                     // the menu. Not stored: a position in the list read from the file, which stands
	                                             // for KIND and NAME below (objects/proxy_object.cpp, GetDParameter)
		CYCLESPROXY_TAKE_FILE_SCENE = 0,           // its first entry: the file's scene
	CYCLESPROXY_TAKE_KIND = 1005,                // what is taken; no field of its own
		CYCLESPROXY_TAKE_KIND_FILE_SCENE = 0,      // what the file's scene shows (its first scene by name)
		CYCLESPROXY_TAKE_KIND_SCENE = 1,           // what the scene NAME shows
		CYCLESPROXY_TAKE_KIND_COLLECTION = 2,      // the collection NAME
	CYCLESPROXY_TAKE_NAME = 1006,                // Blender's name of that scene or collection; no field of its own
	CYCLESPROXY_STATUS = 1007,                   // a line of text: what was read from the file and when; not stored
	CYCLESPROXY_LIGHTS = 1008,                   // the file's lights light the scene
	CYCLESPROXY_FRAME_OFFSET = 1009,             // frames added to the document's frame for the file's animation

	// What a Blender said the file holds (ReadProxyFile); read by GetProxyContent
	CYCLESPROXY_CONTENT = 1100,                  // a container of containers, one per scene and collection
	CYCLESPROXY_CONTENT_FILE = 1101,             // the file it was read from, absolute
	CYCLESPROXY_CONTENT_STAMP = 1102,            // that file's date then (ProxyFileStamp)
	CYCLESPROXY_CONTENT_BLENDER = 1103           // the Blender that read it, as Blender writes its version
};

#endif // OCYCLESPROXY_H__
