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

SceneWriter::SceneWriter(maxon::Bool clear) : _clear(clear)
{
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

void SceneWriter::Warn(const maxon::String& message)
{
	for (const maxon::String& known : _warnings)
	{
		if (known == message)
			return;
	}
	_warnings.Append(message) iferr_ignore("A lost warning is not worth failing the export.");
}

maxon::UInt64 SceneWriter::Fingerprint(const Item& item) const
{
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
	maxon::Int changes = 0;
	for (Item& item : _items)
	{
		const maxon::UInt64 fingerprint = Fingerprint(item);

		const auto* const known = state._items.Find(item.id);
		item.send = _clear || !known || known->GetValue() != fingerprint;
		if (!item.send && item.sendWith != NOTOK && _items[item.sendWith].send)
			item.send = true;
		if (item.send)
			++changes;
		if (partial)
		{
			state._items.Insert(item.id, fingerprint) iferr_return;
		}
		else
		{
			current.Insert(item.id, fingerprint) iferr_return;
		}
	}

	if (partial)
		return changes;

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

	// Blobs of items that are not sent travel as empty blobs, so the indices of the others hold.
	maxon::BaseArray<maxon::Block<const maxon::Char>> views;
	views.Resize(_blobs.GetCount()) iferr_return;
	request.BeginArray("items");
	for (const Item& item : _items)
	{
		if (!item.send)
			continue;
		if (item.blobCount > 0)
		{
			// Tell the bridge where this item's blobs start. The position is added here and not
			// to the item's own text, so that the fingerprint does not change when other items
			// come and go.
			const maxon::String& text = item.json.GetText();
			request.Raw(nullptr, text.GetLeftPart(text.GetLength() - 1) + FormatString(",\"blobs\":@}", item.firstBlob));
		}
		else
		{
			request.Raw(nullptr, item.json.GetText());
		}
		for (maxon::Int i = 0; i < item.blobCount; ++i)
		{
			const Blob& blob = _blobs[item.firstBlob + i];
			views[item.firstBlob + i] = maxon::Block<const maxon::Char>(blob.GetFirst(), blob.GetCount());
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
