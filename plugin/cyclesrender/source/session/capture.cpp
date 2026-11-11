// SPDX-License-Identifier: Apache-2.0
// Development aid: the capture file of everything exchanged with the bridge (capture.h).

#include "c4d.h"
#include "maxon/configuration.h"
#include "maxon/iostreams.h"
#include "maxon/url.h"

#include "capture.h"

// Record every request and reply header in a text file, one per line:
//   g_cyclesRenderCapture=<file>
MAXON_CONFIGURATION_STRING(g_cyclesRenderCapture, "", maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: file that receives every message header exchanged with Blender. Empty: off.");

namespace cyclesrender
{

maxon::Bool IsCapturing()
{
	return !g_cyclesRenderCapture.IsEmpty();
}

static maxon::Result<void> Append(const maxon::String& line)
{
	iferr_scope;

	const maxon::Url file(g_cyclesRenderCapture);
	const maxon::OutputStreamRef stream = file.OpenOutputStream(maxon::OPENSTREAMFLAGS::WRITE_DONT_TRUNCATE | maxon::OPENSTREAMFLAGS::SEEK_TO_END) iferr_return;
	const maxon::BaseArray<maxon::Char> bytes = line.GetCString() iferr_return;
	stream.Write(bytes.ToBlock()) iferr_return;
	stream.Close() iferr_return;
	return maxon::OK;
}

void Capture(const maxon::Char* kind, const maxon::String& text)
{
	if (!IsCapturing())
		return;
	iferr (Append(maxon::String(kind) + " "_s + text + "\n"_s))
		ApplicationOutput("Cycles Render: could not write to the capture file. @", err);
}

} // namespace cyclesrender
