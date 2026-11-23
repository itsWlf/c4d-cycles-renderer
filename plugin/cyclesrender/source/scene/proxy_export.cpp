// SPDX-License-Identifier: Apache-2.0
// The Blender file of a Cycles Proxy and what a Blender said it holds (proxy_export.h). Reading order:
// the file on disk, what the object shows, the stored content, asking a Blender, the box.

#include "c4d.h"

#include "ocyclesproxy.h"

#include "../session/bridge_client.h"
#include "../session/json.h"
#include "../session/json_writer.h"
#include "coords.h"
#include "parameters.h"
#include "proxy_export.h"

namespace cyclesrender
{

// Keys of one entry inside CYCLESPROXY_CONTENT. Stored in scene files: never renumber.
enum
{
	ENTRY_COLLECTION = 1,
	ENTRY_NAME = 2,
	ENTRY_OBJECTS = 3,
	ENTRY_HAS_BOX = 4,
	ENTRY_BOX_MIN = 5,
	ENTRY_BOX_MAX = 6
};

maxon::Bool IsProxySwitchedOn(const cinema::BaseObject* proxy)
{
	return proxy && proxy->GetDeformMode();
}

// --- the file on disk --------------------------------------------------------------------------------

// The file the object names, as found on disk. False when none is chosen (@p named empty) or it is not found.
static maxon::Bool FindProxyFile(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc, cinema::Filename& named, cinema::Filename& found)
{
	named = proxy->GetDataInstanceRef().GetFilename(CYCLESPROXY_FILE);
	if (!named.IsPopulated())
		return false;
	// Found as a bitmap's file is: as named, next to the document, in its "tex" folder, in the search paths.
	const cinema::Filename folder = doc ? doc->GetDocumentPath() : cinema::Filename();
	return cinema::GenerateTexturePath(folder, named, cinema::Filename(), &found) && found.IsPopulated();
}

maxon::String ProxyFileStamp(const cinema::Filename& file)
{
	cinema::LocalFileTime changed;
	changed.Init();
	if (!file.IsPopulated() || !cinema::GeGetFileTime(file, GE_FILETIME_MODIFIED, &changed))
		return maxon::String();
	const maxon::Int64 date = (maxon::Int64(changed.year) * 100 + changed.month) * 100 + changed.day;
	const maxon::Int64 time = (maxon::Int64(changed.hour) * 100 + changed.minute) * 100 + changed.second;
	return FormatString("@-@", date, time);
}

maxon::Bool ReadProxySource(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc, ProxySource& source, maxon::String& problem)
{
	problem = maxon::String();
	if (!proxy)
		return false;
	const cinema::BaseContainer& data = proxy->GetDataInstanceRef();
	cinema::Filename named;
	cinema::Filename found;
	if (!FindProxyFile(proxy, doc, named, found))
	{
		problem = named.IsPopulated() ? FormatString("Proxy \"@\": the Blender file \"@\" was not found; it shows nothing.", proxy->GetName(), named.GetString())
			: FormatString("Proxy \"@\": no Blender file is chosen; it shows nothing.", proxy->GetName());
		return false;
	}
	source.path = found.GetString();
	source.stamp = ProxyFileStamp(found);
	source.kind = data.GetInt32(CYCLESPROXY_TAKE_KIND, CYCLESPROXY_TAKE_KIND_FILE_SCENE);
	source.name = data.GetString(CYCLESPROXY_TAKE_NAME);
	if (source.kind != CYCLESPROXY_TAKE_KIND_SCENE && source.kind != CYCLESPROXY_TAKE_KIND_COLLECTION)
		source.kind = CYCLESPROXY_TAKE_KIND_FILE_SCENE;
	source.lights = data.GetBool(CYCLESPROXY_LIGHTS, true);
	const maxon::Int32 frame = doc ? doc->GetTime().GetFrame(doc->GetFps()) : 0;
	source.frame = frame + data.GetInt32(CYCLESPROXY_FRAME_OFFSET, 0);
	source.label = proxy->GetName();
	return true;
}

// --- what a Blender said the file holds ----------------------------------------------------------------

maxon::Result<ProxyContent> GetProxyContent(const cinema::BaseObject* proxy)
{
	iferr_scope;
	ProxyContent content;
	if (!proxy)
		return content;
	const cinema::BaseContainer& data = proxy->GetDataInstanceRef();
	content.file = data.GetString(CYCLESPROXY_CONTENT_FILE);
	content.stamp = data.GetString(CYCLESPROXY_CONTENT_STAMP);
	content.blender = data.GetString(CYCLESPROXY_CONTENT_BLENDER);
	const cinema::BaseContainer* const list = data.GetContainerInstance(CYCLESPROXY_CONTENT);
	for (maxon::Int32 i = 0; list; ++i)
	{
		const cinema::BaseContainer* const stored = list->GetContainerInstance(i);
		if (!stored)
			break;
		ProxyContentEntry entry;
		entry.collection = stored->GetBool(ENTRY_COLLECTION);
		entry.name = stored->GetString(ENTRY_NAME);
		entry.objects = stored->GetInt32(ENTRY_OBJECTS);
		entry.hasBox = stored->GetBool(ENTRY_HAS_BOX);
		entry.boxMin = stored->GetVector(ENTRY_BOX_MIN);
		entry.boxMax = stored->GetVector(ENTRY_BOX_MAX);
		content.entries.Append(std::move(entry)) iferr_return;
	}
	return content;
}

PROXYCONTENT GetProxyContentState(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc)
{
	if (!proxy)
		return PROXYCONTENT::NONE;
	const cinema::BaseContainer& data = proxy->GetDataInstanceRef();
	const maxon::String file = data.GetString(CYCLESPROXY_CONTENT_FILE);
	if (file.IsEmpty())
		return PROXYCONTENT::NONE;
	cinema::Filename named;
	cinema::Filename found;
	if (!FindProxyFile(proxy, doc, named, found))
		return PROXYCONTENT::FILE_MISSING;
	// (Windows does not tell upper from lower case in a path.)
	if (maxon::String(found.GetString()).ToLower() != file.ToLower())
		return PROXYCONTENT::OTHER_FILE;
	// (maxon::String on both sides: the 2024 SDK's String does not compare with it unambiguously.)
	const maxon::String stamp = data.GetString(CYCLESPROXY_CONTENT_STAMP);
	return ProxyFileStamp(found) == stamp ? PROXYCONTENT::CURRENT : PROXYCONTENT::CHANGED;
}

maxon::String DescribeProxyContent(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc)
{
	if (!proxy)
		return maxon::String();
	if (!proxy->GetDataInstanceRef().GetFilename(CYCLESPROXY_FILE).IsPopulated())
		return "No file chosen."_s;
	switch (GetProxyContentState(proxy, doc))
	{
		case PROXYCONTENT::NONE:
		{
			cinema::Filename named;
			cinema::Filename found;
			return FindProxyFile(proxy, doc, named, found) ? "Not read yet."_s : "The file was not found."_s;
		}
		case PROXYCONTENT::FILE_MISSING: return "The file was not found."_s;
		case PROXYCONTENT::OTHER_FILE: return "Read from another file: press Read File."_s;
		case PROXYCONTENT::CHANGED: return "The file changed since it was read: press Read File."_s;
		case PROXYCONTENT::CURRENT: break;
	}
	maxon::Int scenes = 0;
	maxon::Int collections = 0;
	iferr (const ProxyContent content = GetProxyContent(proxy))
		return maxon::String();
	for (const ProxyContentEntry& entry : content.entries)
		++(entry.collection ? collections : scenes);
	return FormatString("Read: @ scene(s), @ collection(s), by Blender @.", scenes, collections, content.blender);
}

// A JSON number, whichever numeric type the parser gave it.
static maxon::Float64 NumberOf(const maxon::Data& data)
{
	const maxon::Result<maxon::Float64> asFloat = data.Get<maxon::Float64>();
	if (asFloat == maxon::OK)
		return asFloat.GetValue();
	const maxon::Result<maxon::Int64> asInt64 = data.Get<maxon::Int64>();
	if (asInt64 == maxon::OK)
		return maxon::Float64(asInt64.GetValue());
	const maxon::Result<maxon::Int32> asInt32 = data.Get<maxon::Int32>();
	return asInt32 == maxon::OK ? maxon::Float64(asInt32.GetValue()) : 0.0;
}

// Three numbers of a reply as a vector; false when the list is not there.
static maxon::Bool VectorOf(const maxon::DataDictionary& entry, const maxon::String& key, cinema::Vector& out)
{
	maxon::BaseArray<maxon::Data> numbers;
	iferr (entry.GetCopy(key, numbers))
		return false;
	if (numbers.GetCount() != 3)
		return false;
	out = cinema::Vector(NumberOf(numbers[0]), NumberOf(numbers[1]), NumberOf(numbers[2]));
	return true;
}

maxon::Result<maxon::String> ReadProxyFile(cinema::BaseObject* proxy, cinema::BaseDocument* doc)
{
	iferr_scope;
	if (!proxy)
		return maxon::NullptrError(MAXON_SOURCE_LOCATION);
	cinema::Filename named;
	cinema::Filename found;
	if (!FindProxyFile(proxy, doc, named, found))
		return maxon::IllegalArgumentError(MAXON_SOURCE_LOCATION, named.IsPopulated() ? FormatString("the Blender file \"@\" was not found.", named.GetString()) : "no Blender file is chosen."_s);
	const maxon::String path = found.GetString();
	const maxon::String stamp = ProxyFileStamp(found);

	// A Blender of its own for the question, as for the devices (prefs/devices.cpp, FindDevices).
	BridgeClient client;
	client.Open(BRIDGEMODE::FINAL) iferr_return;
	JsonWriter request;
	request.BeginObject();
	request.String("op", "blend_info"_s);
	request.String("path", path);
	const maxon::Result<BridgeMessage> answer = client.Request(request);
	client.Shutdown() iferr_ignore("Blender is ended with the client either way.");
	if (answer == maxon::FAILED)
		return answer.GetError();
	const maxon::DataDictionary& reply = answer.GetValue().header;

	cinema::BaseContainer list;
	maxon::Int32 count = 0;
	maxon::Int scenes = 0;
	maxon::Int collections = 0;
	for (const maxon::Bool collection : { false, true })
	{
		maxon::BaseArray<maxon::Data> listed;
		reply.GetCopy(collection ? "collections"_s : "scenes"_s, listed) iferr_return;
		for (const maxon::Data& data : listed)
		{
			const maxon::Result<maxon::DataDictionary> entry = data.Get<maxon::DataDictionary>();
			if (entry == maxon::FAILED)
				continue;
			cinema::BaseContainer stored;
			stored.SetBool(ENTRY_COLLECTION, collection);
			stored.SetString(ENTRY_NAME, GetJsonString(entry.GetValue(), "name"_s));
			stored.SetInt32(ENTRY_OBJECTS, maxon::Int32(GetJsonInt(entry.GetValue(), "objects"_s, 0)));
			cinema::Vector low;
			cinema::Vector high;
			const maxon::Bool hasBox = VectorOf(entry.GetValue(), "min"_s, low) && VectorOf(entry.GetValue(), "max"_s, high);
			stored.SetBool(ENTRY_HAS_BOX, hasBox);
			stored.SetVector(ENTRY_BOX_MIN, low);
			stored.SetVector(ENTRY_BOX_MAX, high);
			list.SetContainer(count++, stored);
			++(collection ? collections : scenes);
		}
	}

	if (doc)
	{
		doc->StartUndo();
		doc->AddUndo(cinema::UNDOTYPE::CHANGE_SMALL, proxy);
	}
	cinema::BaseContainer& data = proxy->GetDataInstanceRef();
	data.SetContainer(CYCLESPROXY_CONTENT, list);
	data.SetString(CYCLESPROXY_CONTENT_FILE, path);
	data.SetString(CYCLESPROXY_CONTENT_STAMP, stamp);
	data.SetString(CYCLESPROXY_CONTENT_BLENDER, client.GetBlenderVersionText());
	proxy->SetDirty(cinema::DIRTYFLAGS::DATA);
	if (doc)
		doc->EndUndo();
	return FormatString("@ scene(s) and @ collection(s) read from @ by Blender @.", scenes, collections, found.GetFileString(), client.GetBlenderVersionText());
}

// --- the box ----------------------------------------------------------------------------------------------

maxon::Bool GetProxyBox(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc, cinema::Vector& centre, cinema::Vector& radius)
{
	// Blender's metres in document units; a point of the file (x, y, z) lies at (x, z, y) of the object (coords.h).
	const maxon::Float64 units = 1.0 / maxon::Max(Coords::FromDocument(doc).GetScale(), 1e-12);
	centre = cinema::Vector(0.0, 0.5 * PROXY_DEFAULT_BOX, 0.0) * units;
	radius = cinema::Vector(0.5 * PROXY_DEFAULT_BOX) * units;
	if (!proxy)
		return false;
	const cinema::BaseContainer& data = proxy->GetDataInstanceRef();
	const maxon::Int32 kind = data.GetInt32(CYCLESPROXY_TAKE_KIND, CYCLESPROXY_TAKE_KIND_FILE_SCENE);
	const maxon::String name = data.GetString(CYCLESPROXY_TAKE_NAME);
	iferr (const ProxyContent content = GetProxyContent(proxy))
		return false;
	for (const ProxyContentEntry& entry : content.entries)
	{
		// The file's scene is its first one; the list has the scenes first.
		const maxon::Bool shown = kind == CYCLESPROXY_TAKE_KIND_COLLECTION ? (entry.collection && entry.name == name)
			: kind == CYCLESPROXY_TAKE_KIND_SCENE ? (!entry.collection && entry.name == name) : !entry.collection;
		if (!shown)
			continue;
		if (!entry.hasBox)
			return false;
		const cinema::Vector low(entry.boxMin.x, entry.boxMin.z, entry.boxMin.y);
		const cinema::Vector high(entry.boxMax.x, entry.boxMax.z, entry.boxMax.y);
		centre = (low + high) * (0.5 * units);
		radius = (high - low) * (0.5 * units);
		return true;
	}
	return false;
}

maxon::String DescribeProxy(const cinema::BaseObject* proxy, const cinema::BaseDocument* doc)
{
	JsonWriter json;
	json.BeginObject();
	if (proxy)
	{
		static const maxon::Char* const STATES[] = { "none", "current", "changed", "other_file", "file_missing" };
		json.String("state", maxon::String(STATES[maxon::Int(GetProxyContentState(proxy, doc))]));
		json.String("status", DescribeProxyContent(proxy, doc));
		iferr (const ProxyContent content = GetProxyContent(proxy))
		{
		}
		else
		{
			json.String("file", content.file);
			json.String("stamp", content.stamp);
			json.String("blender", content.blender);
			json.BeginArray("entries");
			for (const ProxyContentEntry& entry : content.entries)
			{
				json.BeginObject();
				json.String("kind", entry.collection ? "collection"_s : "scene"_s);
				json.String("name", entry.name);
				json.Int("objects", entry.objects);
				if (entry.hasBox)
				{
					const maxon::Float64 low[3] = { entry.boxMin.x, entry.boxMin.y, entry.boxMin.z };
					const maxon::Float64 high[3] = { entry.boxMax.x, entry.boxMax.y, entry.boxMax.z };
					json.Floats("min", low, 3);
					json.Floats("max", high, 3);
				}
				json.EndObject();
			}
			json.EndArray();
		}
		cinema::Vector centre;
		cinema::Vector radius;
		const maxon::Bool own = GetProxyBox(proxy, doc, centre, radius);
		const maxon::Float64 middle[3] = { centre.x, centre.y, centre.z };
		const maxon::Float64 half[3] = { radius.x, radius.y, radius.z };
		json.BeginObject("box");
		json.Bool("own", own);
		json.Floats("centre", middle, 3);
		json.Floats("radius", half, 3);
		json.EndObject();
		ProxySource source;
		maxon::String problem;
		json.BeginObject("source");
		if (ReadProxySource(proxy, doc, source, problem))
		{
			json.String("path", source.path);
			json.String("stamp", source.stamp);
			json.Int("kind", source.kind);
			json.String("name", source.name);
			json.Bool("lights", source.lights);
			json.Int("frame", source.frame);
		}
		else
		{
			json.String("problem", problem);
		}
		json.EndObject();
	}
	json.EndObject();
	return json.GetText();
}

} // namespace cyclesrender
