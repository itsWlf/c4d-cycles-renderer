// SPDX-License-Identifier: Apache-2.0
// Development aid: writes the exporter's listing of the active document to a file
// (scene_dump.h).

#include "c4d.h"
#include "maxon/configuration.h"
#include "maxon/file_utilities.h"

#include "scene_dump.h"
#include "scene_exporter.h"

// Tests list what would be exported, without rendering:
//   g_cyclesRenderDumpFile=<file>
MAXON_CONFIGURATION_STRING(g_cyclesRenderDumpFile, "", maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: file that receives the exporter's listing of the active document when a test asks for it. Empty: off.");

namespace cyclesrender
{

maxon::Bool IsSceneDumpRequested()
{
	return !g_cyclesRenderDumpFile.IsEmpty();
}

static maxon::Result<void> Dump()
{
	iferr_scope;

	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	if (!doc)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "There is no active document."_s);

	SceneExporter exporter(doc, ExportSettings());
	exporter.Collect() iferr_return;
	const maxon::String text = exporter.GetListing() iferr_return;
	maxon::FileUtilities::WriteString(maxon::Url(g_cyclesRenderDumpFile), text) iferr_return;
	return maxon::OK;
}

maxon::Bool DumpActiveDocument()
{
	iferr (Dump())
	{
		ApplicationOutput("Cycles Render: the scene listing could not be written. @", err);
		return false;
	}
	return true;
}

} // namespace cyclesrender
