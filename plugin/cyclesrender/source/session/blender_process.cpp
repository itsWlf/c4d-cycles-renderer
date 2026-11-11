// SPDX-License-Identifier: Apache-2.0
// Launching Blender with the bridge and waiting for it to come up (blender_process.h).
//
// The launch in short: Blender is started with the bridge's main.py as its start-up script
// and two arguments for it, a file name and a secret. The bridge opens a socket on a port the
// system picks, writes the port into that file, and from then on only talks to a client that
// knows the secret (protocol/protocol.md, "Handshake").

#include "c4d.h"
#include "maxon/application.h"
#include "maxon/configuration.h"
#include "maxon/file_utilities.h"
#include "maxon/timevalue.h"
#include "maxon/uuid.h"

#include "../prefs/settings.h"
#include "blender_install.h"
#include "blender_process.h"
#include "json.h"

// During development the bridge is used straight from the repository:
//   g_cyclesRenderBridge=<repo>/bridge/c4d_bridge/main.py
MAXON_CONFIGURATION_STRING(g_cyclesRenderBridge, "", maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: path of the bridge's main.py. Empty: use the bridge installed in Blender.");

namespace cyclesrender
{

static const maxon::Float64 HANDSHAKE_TIMEOUT_SECONDS = 60.0;

// The script Blender is started with: the one named by the development setting, else the
// bridge the user installed in Blender (FindBridgeScript).
static maxon::Result<maxon::String> BridgeScriptPath(const maxon::Url& executable)
{
	iferr_scope;

	if (!g_cyclesRenderBridge.IsEmpty())
		return g_cyclesRenderBridge;

	const maxon::Url script = FindBridgeScript(executable) iferr_return;
	if (script.IsEmpty())
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "The Cycles Render bridge is not installed in Blender. "
			"Install the bridge's .zip of this release in Blender: Edit > Preferences > Get Extensions > Install from Disk."_s);
	return script.GetSystemPath();
}

// A fresh secret for one launch, made only of letters and digits so it is safe in a file
// name and on a command line.
static maxon::Result<maxon::String> CreateToken()
{
	iferr_scope;

	maxon::Uuid uuid = maxon::Uuid::Create() iferr_return;
	uuid.CreateId() iferr_return;
	const maxon::String text = uuid.ToString();

	maxon::String token;
	for (maxon::Int i = 0; i < text.GetLength(); ++i)
	{
		const maxon::Utf32Char c = text[i];
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
		{
			token.AppendChar(c) iferr_return;
		}
	}
	return token;
}

// The SDK passes arguments to the new process unquoted, so paths with spaces need quotes.
static maxon::String Quoted(const maxon::String& argument)
{
	maxon::Int position = 0;
	if (!argument.Find(" "_s, &position))
		return argument;
	return "\""_s + argument + "\""_s;
}

// The last part of what Blender printed, for error messages.
static maxon::String ReadLogTail(const maxon::Url& logFile)
{
	if (logFile.IsEmpty())
		return maxon::String();
	maxon::BaseArray<maxon::Char> bytes;
	iferr (maxon::FileUtilities::ReadFileToMemory(maxon::Url(logFile), bytes))
		return maxon::String();
	const maxon::Int count = bytes.GetCount();
	const maxon::Int start = count > 1500 ? count - 1500 : 0;
	return count > 0 ? maxon::String(&bytes[start], count - start) : maxon::String();
}

BlenderProcess::~BlenderProcess()
{
	Kill();
	if (!_handshakeFile.IsEmpty())
		_handshakeFile.IoDelete(true) iferr_ignore("The handshake file is a temporary file.");
	if (_process)
	{
		const maxon::Url logFile = _process.GetLogFile();
		if (!logFile.IsEmpty())
			logFile.IoDelete(true) iferr_ignore("The captured output is a temporary file.");
	}
}

maxon::Result<void> BlenderProcess::Start(BRIDGEMODE mode)
{
	iferr_scope;

	const cinema::Filename blender = GetBlenderPathOrDetect();
	if (!blender.IsPopulated())
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "No Blender is set. Choose it in Preferences > Renderer > Cycles Render."_s);
	const maxon::Url executable = cinema::MaxonConvert(blender, cinema::MAXONCONVERTMODE::NONE);
	if (executable.IoDetect() != maxon::IODETECT::FILE)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "The Blender set in the Preferences does not exist."_s);

	const maxon::String script = BridgeScriptPath(executable) iferr_return;

	_token = CreateToken() iferr_return;
	const maxon::Url temp = maxon::Application::GetUrl(maxon::APPLICATION_URLTYPE::TEMP_DIR) iferr_return;
	_handshakeFile = (temp + FormatString("cyclesrender_@.json", _token)) iferr_return;
	const maxon::String handshakePath = _handshakeFile.GetSystemPath() iferr_return;

	// The command line (docs/reference/blender.md, "Launch"):
	//   -b                    background mode, for final frames: no window, one blocking render at a time
	//   --factory-startup     ignore the user's start-up file and preferences: the same Blender on every machine
	//   --offline-mode        Blender itself never goes online
	//   -Y                    allow Python to run without asking (the bridge is Python)
	//   --python-exit-code 1  a Python error in the bridge ends Blender with that code instead of going unnoticed
	//   --no-window-focus, -p, --gpu-vsync off
	//                         the IPR's window: it must not take the focus from Cinema 4D, starts small (it
	//                         is hidden and resized by the bridge), and draws as fast as frames arrive
	maxon::BaseArray<maxon::String> arguments;
	if (mode == BRIDGEMODE::FINAL)
	{
		arguments.Append("-b"_s) iferr_return;
	}
	arguments.Append("--factory-startup"_s) iferr_return;
	arguments.Append("--offline-mode"_s) iferr_return;
	arguments.Append("-Y"_s) iferr_return;
	arguments.Append("--python-exit-code"_s) iferr_return;
	arguments.Append("1"_s) iferr_return;
	if (mode == BRIDGEMODE::VIEWPORT)
	{
		arguments.Append("--no-window-focus"_s) iferr_return;
		arguments.Append("-p"_s) iferr_return;
		arguments.Append("0"_s) iferr_return;
		arguments.Append("0"_s) iferr_return;
		arguments.Append("960"_s) iferr_return;
		arguments.Append("540"_s) iferr_return;
		arguments.Append("--gpu-vsync"_s) iferr_return;
		arguments.Append("off"_s) iferr_return;
	}
	arguments.Append("--python"_s) iferr_return;
	arguments.Append(Quoted(script)) iferr_return;
	arguments.Append("--"_s) iferr_return;
	arguments.Append("--handshake"_s) iferr_return;
	arguments.Append(Quoted(handshakePath)) iferr_return;
	arguments.Append("--token"_s) iferr_return;
	arguments.Append(_token) iferr_return;

	// Blender's output goes to a temporary file so a failed start can say why.
	maxon::EXECUTEPROGRAMFLAGS flags = maxon::EXECUTEPROGRAMFLAGS::HIDE_APPLICATION |
		maxon::EXECUTEPROGRAMFLAGS::CAPTURELOGFILE | maxon::EXECUTEPROGRAMFLAGS::DONT_DELETE_LOGFILE;
#ifdef MAXON_TARGET_WINDOWS
	flags |= maxon::EXECUTEPROGRAMFLAGS::KILL_IF_STARTING_PROCESS_ENDS;
#endif
	_process = maxon::SystemProcessInterface::CreateProcess(executable, arguments, flags, nullptr) iferr_return;
	_process.StartProcess() iferr_return;

	// The bridge writes the handshake file once it listens.
	const maxon::TimeValue deadline = maxon::TimeValue::GetTime() + maxon::Seconds(HANDSHAKE_TIMEOUT_SECONDS);
	while (_handshakeFile.IoDetect() != maxon::IODETECT::FILE)
	{
		if (!_process.IsRunning())
			return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, FormatString("Blender exited with code @ before the bridge started. Its output ended with: @",
				_process.GetExitCode(), ReadLogTail(_process.GetLogFile())));
		if (maxon::TimeValue::GetTime() > deadline)
		{
			Kill();
			return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "Blender did not start the bridge in time."_s);
		}
		cinema::GeSleep(20);
	}

	// The file appears by being renamed into place; for a moment after that Windows can still
	// refuse to open it, so a failed read is tried again for up to two seconds.
	maxon::BaseArray<maxon::Char> bytes;
	for (maxon::Int attempt = 0; ; ++attempt)
	{
		bytes.Reset();
		const maxon::Result<void> read = maxon::FileUtilities::ReadFileToMemory(maxon::Url(_handshakeFile), bytes);
		if (read == maxon::OK && bytes.GetCount() > 0)
			break;
		if (attempt >= 100)
			return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "The bridge's handshake file could not be read."_s);
		cinema::GeSleep(20);
	}
	const maxon::DataDictionary handshake = ParseJson(maxon::String(bytes.GetFirst(), bytes.GetCount())) iferr_return;

	const maxon::String refusal = GetJsonString(handshake, "error"_s);
	if (!refusal.IsEmpty())
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, refusal);

	_port = maxon::Int32(GetJsonInt(handshake, "port"_s, 0));
	_pid = maxon::Int(GetJsonInt(handshake, "pid"_s, 0));
	if (_port <= 0)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "The bridge did not report a port."_s);
	return maxon::OK;
}

maxon::Bool BlenderProcess::IsRunning() const
{
	return _process && _process.IsRunning();
}

maxon::Result<maxon::Int> BlenderProcess::WaitForExit(maxon::Float64 seconds)
{
	iferr_scope;

	if (!_process)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "Blender was not started."_s);

	const maxon::WAIT_PROCESS_RESULT state = maxon::SystemProcessInterface::WaitForProcess(_process, maxon::Seconds(seconds)) iferr_return;
	if (state == maxon::WAIT_PROCESS_RESULT::RUNNING)
	{
		Kill();
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "Blender did not exit in time and was stopped."_s);
	}
	return _process.GetExitCode();
}

void BlenderProcess::Kill()
{
	if (IsRunning())
		_process.KillProcess() iferr_ignore("The process may have ended in the meantime.");
}

} // namespace cyclesrender
