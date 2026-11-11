// SPDX-License-Identifier: Apache-2.0
// Starts and supervises the Blender process that runs the bridge.

#ifndef CYCLESRENDER_BLENDER_PROCESS_H__
#define CYCLESRENDER_BLENDER_PROCESS_H__

#include "c4d.h"
#include "maxon/system_process.h"
#include "maxon/url.h"

namespace cyclesrender
{

/// How Blender is started. See "Launch" in docs/reference/blender.md.
enum class BRIDGEMODE
{
	FINAL,		///< Background Blender: one blocking render per frame (Picture Viewer).
	VIEWPORT	///< Windowed Blender with a hidden window: live viewport rendering (IPR).
} MAXON_ENUM_LIST(BRIDGEMODE);

//----------------------------------------------------------------------------------------
/// One launched Blender. Destroying the object kills the process if it is still running.
//----------------------------------------------------------------------------------------
class BlenderProcess
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(BlenderProcess);

public:
	BlenderProcess() = default;
	~BlenderProcess();

	/// Starts the Blender chosen in the Preferences with the bridge and waits until the bridge
	/// has published its port. Fails with a message for the user when it does not come up.
	maxon::Result<void> Start(BRIDGEMODE mode);

	maxon::Bool IsRunning() const;

	/// The port the bridge listens on and the secret it expects. Valid after Start().
	maxon::Int32 GetPort() const { return _port; }
	const maxon::String& GetToken() const { return _token; }
	maxon::Int GetProcessId() const { return _pid; }

	/// Waits for Blender to exit and returns its exit code. Kills it when the time is up.
	maxon::Result<maxon::Int> WaitForExit(maxon::Float64 seconds);

	/// Kills Blender if it is running.
	void Kill();

private:
	maxon::SystemProcessObjectRef _process;
	maxon::Url _handshakeFile;
	maxon::String _token;
	maxon::Int32 _port = 0;
	maxon::Int _pid = 0;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_BLENDER_PROCESS_H__
