// SPDX-License-Identifier: Apache-2.0
// One `scene` request under construction, and delta sync (scene_writer.h).

#include "c4d.h"

#include "../materials/baked_files.h"
#include "../session/json.h"
#include "scene_writer.h"

namespace cyclesrender
{

maxon::UInt64 HashBytes(const maxon::Char* data, maxon::Int count, maxon::UInt64 seed)
{
	maxon::UInt64 hash = seed ^ (maxon::UInt64(count) * 0x9E3779B97F4A7C15ull);
	maxon::Int i = 0;
	for (; i + 8 <= count; i += 8)
	{
		maxon::UInt64 word;
		maxon::MemCopyType(reinterpret_cast<maxon::Char*>(&word), data + i, 8);
		hash = (hash ^ word) * 0x9E3779B97F4A7C15ull;
		hash ^= hash >> 29;
	}
	for (; i < count; ++i)
	{
		hash = (hash ^ maxon::UInt64(maxon::UChar(data[i]))) * 0x100000001B3ull;
	}
	return hash ^ (hash >> 32);
}

maxon::Result<void> SyncState::CopyFrom(const SyncState& other)
{
	iferr_scope;
	_items.Reset();
	_sources.Reset();
	_sourcesAfter.Reset();
	for (const auto& entry : other._items)
	{
		_items.Insert(entry.GetKey(), entry.GetValue()) iferr_return;
	}
	for (const auto& entry : other._sources)
	{
		_sources.Insert(entry.GetKey(), entry.GetValue()) iferr_return;
	}
	for (const auto& entry : other._sourcesAfter)
	{
		_sourcesAfter.Insert(entry.GetKey(), entry.GetValue()) iferr_return;
	}
	return maxon::OK;
}

SceneWriter::SceneWriter(maxon::Bool clear) : _clear(clear)
{
}

maxon::Result<maxon::BaseArray<maxon::String>> SceneWriter::GetChangedIds() const
{
	iferr_scope;
	maxon::BaseArray<maxon::String> ids;
	for (const Item& item : _items)
	{
		if (item.send)
		{
			ids.Append(item.id) iferr_return;
		}
	}
	for (const maxon::String& id : _removed)
	{
		ids.Append("removed "_s + id) iferr_return;
	}
	return ids;
}

SceneWriter::~SceneWriter()
{
	ReleaseBakedFiles(_bakedFiles);
}

maxon::Result<void> SceneWriter::HoldBakedFile(const maxon::String& id)
{
	iferr_scope;
	_bakedFiles.Append(id) iferr_return;
	return maxon::OK;
}

maxon::String SceneWriter::GetItemsJson(const maxon::Char* kind) const
{
	const maxon::String wanted = FormatString("{\"kind\":\"@\"", maxon::String(kind));
	maxon::String json = "["_s;
	maxon::Bool first = true;
	for (const Item& item : _items)
	{
		if (!item.json.GetText().StartsWith(wanted))
			continue;
		if (!first)
			json += ","_s;
		json += item.json.GetText();
		first = false;
	}
	return json + "]"_s;
}

JsonWriter& SceneWriter::BeginItem(const maxon::Char* kind, const maxon::String& id)
{
	DebugAssert(!_itemOpen, "Finish one scene item before opening the next.");
	_itemOpen = true;

	Item item;
	item.id = id;
	item.firstBlob = _itemFirstBlob;
	iferr (Item& stored = _items.Append(std::move(item)))
	{
		_discarded = JsonWriter();
		_discarded.BeginObject();
		return _discarded;
	}
	stored.json.BeginObject();
	stored.json.String("kind", maxon::String(kind));
	stored.json.String("id", id);
	return stored.json;
}

void SceneWriter::SendWith(const maxon::String& id)
{
	if (!_itemOpen || _items.IsEmpty())
		return;
	// The other item is nearly always the one written just before.
	for (maxon::Int i = _items.GetCount() - 2; i >= 0; --i)
	{
		if (_items[i].id == id)
		{
			_items[_items.GetCount() - 1].sendWith = i;
			return;
		}
	}
}

void SceneWriter::EndItem()
{
	_itemOpen = false;
	if (_items.IsEmpty())
		return;
	Item& item = _items[_items.GetCount() - 1];
	item.blobCount = _blobs.GetCount() - item.firstBlob;
	item.json.EndObject();
	_itemFirstBlob = _blobs.GetCount();
}

maxon::Result<maxon::Char*> SceneWriter::AllocBytes(maxon::Int count, maxon::Int& blobIndex)
{
	iferr_scope;

	Blob blob;
	blob.Resize(count) iferr_return;
	blobIndex = _blobs.GetCount() - _itemFirstBlob;
	// Moving a BaseArray keeps its memory block, so the pointer survives later appends.
	Blob& stored = _blobs.Append(std::move(blob)) iferr_return;
	return stored.GetFirst();
}

maxon::Result<maxon::Float32*> SceneWriter::AllocFloats(maxon::Int count, maxon::Int& blobIndex)
{
	iferr_scope;
	maxon::Char* const bytes = AllocBytes(count * SIZEOF(maxon::Float32), blobIndex) iferr_return;
	return reinterpret_cast<maxon::Float32*>(bytes);
}

maxon::Result<maxon::Int32*> SceneWriter::AllocInts(maxon::Int count, maxon::Int& blobIndex)
{
	iferr_scope;
	maxon::Char* const bytes = AllocBytes(count * SIZEOF(maxon::Int32), blobIndex) iferr_return;
	return reinterpret_cast<maxon::Int32*>(bytes);
}

void SceneWriter::RemoveItem(const maxon::String& id)
{
	_toRemove.Append(id) iferr_ignore("The item then stays until the next full request removes it.");
}

void SceneWriter::Warn(const maxon::String& message)
{
	for (const maxon::String& known : _warnings)
	{
		if (known == message)
			return;
	}
	_warnings.Append(message) iferr_ignore("A lost warning is not worth failing the export.");
}

maxon::Bool SceneWriter::KeepMesh(const SyncState& state, const maxon::String& meshId, maxon::UInt64 signature)
{
	if (_clear || _itemOpen)
		return false;
	const maxon::String ids[2] = { meshId, meshId + ".points"_s };
	maxon::UInt64 fingerprints[2] = { 0, 0 };
	maxon::UInt64 sources[2] = { 0, 0 };
	maxon::UInt64 sourcesAfter[2] = { 0, 0 };
	for (maxon::Int i = 0; i < 2; ++i)
	{
		const auto* const source = state._sources.Find(ids[i]);
		const auto* const after = state._sourcesAfter.Find(ids[i]);
		const auto* const held = state._items.Find(ids[i]);
		if (!source || !after || !held || (source->GetValue() != signature && after->GetValue() != signature))
			return false;
		fingerprints[i] = held->GetValue();
		sources[i] = source->GetValue();
		sourcesAfter[i] = after->GetValue();
	}
	const maxon::Int before = _items.GetCount();
	for (maxon::Int i = 0; i < 2; ++i)
	{
		Item item;
		item.id = ids[i];
		item.firstBlob = _itemFirstBlob;
		item.send = false;
		item.kept = true;
		item.keptFingerprint = fingerprints[i];
		item.hasSource = true;
		item.source = sources[i];
		item.sourceAfter = sourcesAfter[i];
		iferr (_items.Append(std::move(item)))
		{
			// Without both the mesh is exported as usual.
			_items.Resize(before) iferr_ignore("Shrinking an array cannot fail.");
			return false;
		}
	}
	return true;
}

void SceneWriter::NoteMeshSource(const maxon::String& meshId, maxon::UInt64 signature, maxon::UInt64 after)
{
	// The two items ExportMesh() has just written are the last ones.
	const maxon::String pointsId = meshId + ".points"_s;
	for (maxon::Int i = _items.GetCount() - 1; i >= 0 && i >= _items.GetCount() - 2; --i)
	{
		if (_items[i].id == meshId || _items[i].id == pointsId)
		{
			_items[i].hasSource = true;
			_items[i].source = signature;
			_items[i].sourceAfter = after;
		}
	}
}

maxon::UInt64 SceneWriter::Fingerprint(const Item& item) const
{
	if (item.kept)
		return item.keptFingerprint;
	maxon::UInt64 hash = maxon::UInt64(item.json.GetText().GetHashCode());
	for (maxon::Int i = 0; i < item.blobCount; ++i)
	{
		const Blob& blob = _blobs[item.firstBlob + i];
		hash = HashBytes(blob.GetFirst(), blob.GetCount(), hash);
	}
	return hash;
}

maxon::Result<maxon::Int> SceneWriter::KeepChanges(SyncState& state, maxon::Bool partial)
{
	iferr_scope;

	maxon::HashMap<maxon::String, maxon::UInt64> current;
	maxon::HashMap<maxon::String, maxon::UInt64> sources;
	maxon::HashMap<maxon::String, maxon::UInt64> sourcesAfter;
	maxon::Int changes = 0;
	for (Item& item : _items)
	{
		const maxon::UInt64 fingerprint = Fingerprint(item);

		const auto* const known = state._items.Find(item.id);
		item.send = _clear || !known || known->GetValue() != fingerprint;
		if (!item.send && item.sendWith != NOTOK && _items[item.sendWith].send)
			item.send = true;
		// A kept item has no content to send: KeepMesh() found it in this very state, so this cannot
		// happen; should it, the request is made again without keeping anything.
		if (item.send && item.kept)
			_missingKept = true;
		if (item.send)
			++changes;
		if (partial)
		{
			state._items.Insert(item.id, fingerprint) iferr_return;
			if (item.hasSource)
			{
				state._sources.Insert(item.id, item.source) iferr_return;
				state._sourcesAfter.Insert(item.id, item.sourceAfter) iferr_return;
			}
		}
		else
		{
			current.Insert(item.id, fingerprint) iferr_return;
			if (item.hasSource)
			{
				sources.Insert(item.id, item.source) iferr_return;
				sourcesAfter.Insert(item.id, item.sourceAfter) iferr_return;
			}
		}
	}

	if (partial)
	{
		for (const maxon::String& id : _toRemove)
		{
			if (state._items.Erase(id).GetValue())
			{
				state._sources.Erase(id) iferr_return;
				state._sourcesAfter.Erase(id) iferr_return;
				_removed.Append(id) iferr_return;
				++changes;
			}
		}
		return changes;
	}

	if (!_clear)
	{
		for (const auto& entry : state._items)
		{
			if (!current.Find(entry.GetKey()))
			{
				_removed.Append(entry.GetKey()) iferr_return;
				++changes;
			}
		}
	}
	state._items = std::move(current);
	state._sources = std::move(sources);
	state._sourcesAfter = std::move(sourcesAfter);
	return changes;
}

maxon::Result<void> SceneWriter::Send(BridgeClient& client, cinema::BaseThread* cancel)
{
	iferr_scope;

	JsonWriter request;
	request.BeginObject();
	request.String("op", "scene"_s);
	request.Bool("clear", _clear);
	if (!_removed.IsEmpty())
	{
		request.BeginArray("remove");
		for (const maxon::String& id : _removed)
			request.String(nullptr, id);
		request.EndArray();
	}

	// Only the blobs of the items sent travel; each item says where its own begin. (Before 0.2.0 the
	// blobs of items left out travelled empty, which put every blob of the scene into every request:
	// a scene of 900 meshes, 4500 blobs, broke the protocol's limit even for a change of one.)
	maxon::BaseArray<maxon::Block<const maxon::Char>> views;
	request.BeginArray("items");
	for (const Item& item : _items)
	{
		if (!item.send || item.kept)
			continue;
		if (item.blobCount > 0)
		{
			// Tell the bridge where this item's blobs start. The position is added here and not
			// to the item's own text, so that the fingerprint does not change when other items
			// come and go.
			const maxon::String& text = item.json.GetText();
			request.Raw(nullptr, text.GetLeftPart(text.GetLength() - 1) + FormatString(",\"blobs\":@}", views.GetCount()));
		}
		else
		{
			request.Raw(nullptr, item.json.GetText());
		}
		for (maxon::Int i = 0; i < item.blobCount; ++i)
		{
			const Blob& blob = _blobs[item.firstBlob + i];
			views.Append(maxon::Block<const maxon::Char>(blob.GetFirst(), blob.GetCount())) iferr_return;
		}
	}
	request.EndArray();

	// From now on this Blender may read the baked files; the removed items' files it drops.
	NoteBakedFilesSent(&client, _bakedFiles, _removed, _clear);
	const BridgeMessage reply = client.Request(request, BlobViews(views.GetFirst(), views.GetCount()), cancel) iferr_return;

	// The bridge reports problems that do not stop the render, such as a missing texture file.
	for (const maxon::String& warning : GetJsonStrings(reply.header, "warnings"_s))
		Warn(warning);
	return maxon::OK;
}

} // namespace cyclesrender
