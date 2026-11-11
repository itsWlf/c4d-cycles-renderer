// SPDX-License-Identifier: Apache-2.0
// Thumbnails of materials of Cycles nodes, rendered by a Blender of their own
// (material_preview.h).

#include <mutex>

#include "c4d.h"

#include "../session/bridge_client.h"
#include "../session/json.h"
#include "cycles_node_material.h"
#include "material_export.h"
#include "material_preview.h"

namespace cyclesrender
{

// The id of the material in the preview Blender: each preview replaces the one before.
static const maxon::Char* const PREVIEW_MATERIAL_ID = "preview_material";
static const maxon::Int PREVIEW_SAMPLES = 32;
// After Blender could not be started (not installed, no bridge), it is not tried again for
// this long: Cinema 4D asks for previews often.
static const maxon::Float64 RETRY_SECONDS = 30.0;

// One Blender for all previews; one preview at a time. A preview takes about a second, so
// the threads that wait must sleep, not spin.
static std::mutex g_mutex;
static BridgeClient* g_client = nullptr;       // guarded by g_mutex
static maxon::Float64 g_failedAt = -1.0e30;    // guarded by g_mutex
static maxon::Bool g_closed = false;           // guarded by g_mutex; set when the plugin is unloaded

maxon::Result<void> MaterialPreview::Prepare(const maxon::GraphModelRef& graph)
{
	iferr_scope;
	// No document: an image a node names is looked for by its full path and in the texture paths.
	MaterialExporter exporter(_writer, nullptr);
	WriteCyclesGraphMaterial(exporter, graph, maxon::String(PREVIEW_MATERIAL_ID)) iferr_return;
	_prepared = true;
	return maxon::OK;
}

maxon::Result<void> MaterialPreview::Render(maxon::Int size, maxon::BaseArray<maxon::Float32>& pixels)
{
	iferr_scope;
	if (!_prepared)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "the preview was not prepared"_s);
	_prepared = false;   // the writer can send once

	std::lock_guard<std::mutex> guard(g_mutex);
	if (g_closed)
		return maxon::OperationCancelledError(MAXON_SOURCE_LOCATION);
	if (g_client && !g_client->IsAlive())
		DeleteObj(g_client);
	if (!g_client)
	{
		const maxon::Float64 now = maxon::TimeValue::GetTime().GetSeconds();
		if (now - g_failedAt < RETRY_SECONDS)
			return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "Blender could not be started for material previews"_s);
		g_client = NewObj(BridgeClient) iferr_return;
		iferr (g_client->Open(BRIDGEMODE::FINAL))
		{
			DeleteObj(g_client);
			g_failedAt = now;
			return err;
		}
	}

	_writer.Send(*g_client, nullptr) iferr_return;

	JsonWriter request;
	request.BeginObject();
	request.String("op", "material_preview"_s);
	request.String("material", maxon::String(PREVIEW_MATERIAL_ID));
	request.Int("size", size);
	request.Int("samples", PREVIEW_SAMPLES);
	const BridgeMessage reply = g_client->Request(request) iferr_return;

	const maxon::Int blobIndex = maxon::Int(GetJsonInt(reply.header, "pixels"_s, -1));
	const maxon::Int count = size * size * 4;
	if (blobIndex < 0 || blobIndex >= reply.blobs.GetCount() || reply.blobs[blobIndex].GetCount() != count * SIZEOF(maxon::Float32))
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "Blender's reply holds no preview picture"_s);
	pixels.Resize(count) iferr_return;
	maxon::MemCopyType(reinterpret_cast<maxon::Char*>(pixels.GetFirst()), reply.blobs[blobIndex].GetFirst(), count * SIZEOF(maxon::Float32));
	return maxon::OK;
}

void FreeMaterialPreviews()
{
	std::lock_guard<std::mutex> guard(g_mutex);
	g_closed = true;
	DeleteObj(g_client);   // ends Blender
}

} // namespace cyclesrender
