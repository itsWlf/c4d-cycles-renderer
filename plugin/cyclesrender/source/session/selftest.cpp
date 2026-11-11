// SPDX-License-Identifier: Apache-2.0
// Developer self-test of the session layer (selftest.h).

#include "c4d.h"
#include "maxon/configuration.h"

#include "bridge_client.h"
#include "frame_reader.h"
#include "json.h"
#include "selftest.h"

MAXON_CONFIGURATION_INT(g_cyclesRenderSelfTest, 0, 0, 1000, maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: number of start/ping/stop cycles the session self-test runs.");

namespace cyclesrender
{

// The bridge writes a test pattern into the shared-memory frame block; every value must
// arrive exactly. Value number i in the block's own order (bottom row first) is
// ((i * 2654435761) mod 2^32) >> 8.
static maxon::Result<void> CheckFrameBlock(BridgeClient& client)
{
	iferr_scope;

	const maxon::Int32 width = 1920;
	const maxon::Int32 height = 1080;

	JsonWriter request;
	request.BeginObject();
	request.String("op", "view_test_pattern"_s);
	request.Int("width", width);
	request.Int("height", height);
	client.Request(request) iferr_return;

	FrameReader reader;
	reader.Open(client.GetToken()) iferr_return;
	Frame frame;
	const maxon::Bool got = reader.Read(frame) iferr_return;
	if (!got || frame.width != width || frame.height != height)
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "No frame of the requested size arrived in shared memory."_s);

	const maxon::Int rowValues = maxon::Int(width) * 4;
	for (maxon::Int32 y = 0; y < height; ++y)
	{
		const maxon::Float32* const row = frame.pixels.GetFirst() + rowValues * y;
		const maxon::UInt64 first = maxon::UInt64(rowValues) * maxon::UInt64(height - 1 - y);
		for (maxon::Int x = 0; x < rowValues; ++x)
		{
			const maxon::UInt32 expected = maxon::UInt32(((first + maxon::UInt64(x)) * 2654435761ull) & 0xFFFFFFFFull) >> 8;
			if (row[x] != maxon::Float32(expected))
				return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, FormatString("The test pattern differs in row @ at value @.", y, x));
		}
	}
	return maxon::OK;
}

static maxon::Result<void> RunOneCycle(BRIDGEMODE mode)
{
	iferr_scope;

	BridgeClient client;
	client.Open(mode) iferr_return;

	Blob payload;
	payload.Resize(1 << 20) iferr_return;
	for (maxon::Int i = 0; i < payload.GetCount(); ++i)
		payload[i] = maxon::Char(i * 31 + 7);

	maxon::DataDictionary ping;
	ping.Set("op"_s, "ping"_s) iferr_return;
	ping.Set("text"_s, "hello"_s) iferr_return;
	const maxon::Block<const maxon::Char> blob(payload.GetFirst(), payload.GetCount());
	const maxon::Block<const maxon::Block<const maxon::Char>> blobs(&blob, 1);
	const BridgeMessage reply = client.Request(std::move(ping), blobs) iferr_return;

	if (GetJsonString(reply.header, "text"_s) != "hello"_s)
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "The ping reply lost its text."_s);
	if (reply.blobs.GetCount() != 1 || reply.blobs[0].GetCount() != payload.GetCount())
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "The ping reply lost its blob."_s);
	for (maxon::Int i = 0; i < payload.GetCount(); ++i)
	{
		if (reply.blobs[0][i] != payload[i])
			return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "The ping reply's blob is corrupted."_s);
	}

	if (mode == BRIDGEMODE::VIEWPORT)
	{
		CheckFrameBlock(client) iferr_return;
	}

	const maxon::Int exitCode = client.Shutdown() iferr_return;
	if (exitCode != 0)
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, FormatString("Blender exited with code @.", exitCode));
	return maxon::OK;
}

maxon::Bool RunSessionSelfTest()
{
	const maxon::Int cycles = g_cyclesRenderSelfTest;
	if (cycles <= 0)
		return false;

	maxon::Bool allPassed = true;
	for (maxon::Int i = 0; i < cycles; ++i)
	{
		const BRIDGEMODE mode = (i % 2) ? BRIDGEMODE::VIEWPORT : BRIDGEMODE::FINAL;
		const maxon::String modeName = (i % 2) ? "viewport"_s : "final"_s;
		const cinema::Int32 start = cinema::GeGetTimer();

		iferr (RunOneCycle(mode))
		{
			allPassed = false;
			ApplicationOutput("Cycles Render self-test: cycle @ (@) FAILED: @", i + 1, modeName, err);
			continue;
		}
		ApplicationOutput("Cycles Render self-test: cycle @ (@) ok in @ ms", i + 1, modeName, cinema::GeGetTimer() - start);
	}
	ApplicationOutput("Cycles Render self-test: @ of @ cycles", allPassed ? "PASSED all"_s : "FAILED some"_s, cycles);
	return allPassed;
}

} // namespace cyclesrender
