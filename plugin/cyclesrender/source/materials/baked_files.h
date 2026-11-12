// SPDX-License-Identifier: Apache-2.0
// The texture files the exporter writes for Blender (baked shaders, pixel images): where they
// go, which documents they belong to, who may still read them, and when they are deleted.

#ifndef CYCLESRENDER_BAKED_FILES_H__
#define CYCLESRENDER_BAKED_FILES_H__

#include "c4d.h"

namespace cyclesrender
{

class BridgeClient;
class SceneWriter;

//----------------------------------------------------------------------------------------
/// Baked files are written by MaterialExporter::GetBakedImageId and GetPixelImageId into a
/// folder of this process, <preferences>/cyclesrender/baked/session-<process id>-<time>, and
/// known by the id of their `image` item, which follows from what was baked. An export that
/// asks for a known id gets the file again instead of a new bake: within a session, across
/// final renders and the IPR.
///
/// A file is deleted (SweepBakedFiles) once all of this holds:
/// - Every document it was made for has been closed: it was in Cinema 4D's document list and
///   is not any more (CheckOpenDocuments). A document is known by its MAXON_CREATOR_ID, which
///   the copy a render works on shares. A document that was never in the list (made by a
///   script, rendered by the Render Queue or the command line) keeps its files until the end
///   of the session.
/// - No scene request that names it is being built or waits to be sent: the SceneWriter
///   holds it until it is destroyed.
/// - No Blender the plugin started may read it: a BridgeClient that was sent a scene naming
///   the file holds it until a scene removes that item or clears everything, or until the
///   client is destroyed.
/// At the end of the session every file goes, with the folder (FreeBakedFiles). Folders of
/// sessions that ended without that (a crash) are emptied at the next start: a running
/// session holds a lock file in its folder that no other process can open meanwhile.
///
/// Nothing else is ever deleted: only files this module named, in the plugin's own folder.
/// Thread-safe; exports run on the main thread (IPR) and on render threads.
//----------------------------------------------------------------------------------------

/// The path of the file of image item @p id, or an empty string when there is none. When
/// there is one, it is from now on also @p doc's, and @p writer holds it.
maxon::Result<maxon::String> FindBakedFile(const maxon::String& id, const cinema::BaseDocument* doc, SceneWriter& writer);

/// A file name in this session's folder that was never used, for a new file of image item
/// @p id. Makes the folder on first use; an empty name when that failed.
cinema::Filename NewBakedFileName(const maxon::String& id);

//----------------------------------------------------------------------------------------
/// Takes @p file, just written for image item @p id and @p doc, into the bookkeeping, and
/// @p writer holds it. Returns the path to use: when another thread was quicker with the
/// same id, its file, and @p file is deleted.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::String> AddBakedFile(const maxon::String& id, const cinema::Filename& file, const cinema::BaseDocument* doc, SceneWriter& writer);

/// For ~SceneWriter: @p writer no longer holds the files of the image items @p ids.
void ReleaseBakedFiles(const maxon::BaseArray<maxon::String>& ids);

//----------------------------------------------------------------------------------------
/// For SceneWriter::Send, before the request goes out: @p client will hold the files of the
/// image items @p ids, and no longer those of @p removed. With @p clear, Blender drops
/// everything first, so the client holds nothing else.
//----------------------------------------------------------------------------------------
void NoteBakedFilesSent(const BridgeClient* client, const maxon::BaseArray<maxon::String>& ids, const maxon::BaseArray<maxon::String>& removed,
	maxon::Bool clear);

/// For ~BridgeClient: that Blender holds no file any more.
void ForgetBakedFileClient(const BridgeClient* client);

/// Main thread only: compares the documents the files were made for with Cinema 4D's document
/// list, then deletes what may go. Called by a message hook on every change and once a second.
void CheckOpenDocuments();

/// At start-up: empties the folders of sessions that are not running any more, and removes the
/// files version 0.1.0 left directly in the baked folder.
void RemoveBakedFilesOfEarlierSessions();

/// At the end of the session, when no Blender runs any more: deletes every file and the folder.
void FreeBakedFiles();

/// The bookkeeping as JSON, for tests (the IPR test hook's command "bakes").
maxon::String DescribeBakedFiles();

/// Registers the message hook that calls CheckOpenDocuments().
cinema::Bool RegisterBakedFilesWatch();

} // namespace cyclesrender

#endif // CYCLESRENDER_BAKED_FILES_H__
