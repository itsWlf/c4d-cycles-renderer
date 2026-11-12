// SPDX-License-Identifier: Apache-2.0
// The bookkeeping of baked files (baked_files.h).
//
// In this file, in order: names of folders and files and the checks that recognise them; the
// store (one entry per image item, and what is known of each document); the functions the
// exporter, the scene writer and the Blender client call; the sweep that deletes; start-up
// and end of the session; the message hook that watches the document list.

#include "c4d.h"
#include "ddoc.h"
#include "maxon/spinlock.h"

#include "../ids.h"
#include "../platform/file_lock.h"
#include "../platform/shared_memory.h"
#include "../scene/scene_writer.h"
#include "../session/json_writer.h"
#include "baked_files.h"

namespace cyclesrender
{

// --- names ------------------------------------------------------------------------------------

static const maxon::Char* const SESSION_PREFIX = "session-";
static const maxon::Char* const LOCK_FILE = "session.lock";

static cinema::Filename BakeRoot()
{
	return cinema::GeGetC4DPath(C4D_PATH_PREFS) + cinema::Filename("cyclesrender"_s) + cinema::Filename("baked"_s);
}

static maxon::Bool IsHexDigit(maxon::Utf32Char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

// Counts the characters of @p text from @p start that satisfy @p test.
template <typename TEST> static maxon::Int CountWhile(const maxon::String& text, maxon::Int start, TEST test)
{
	maxon::Int end = start;
	while (end < text.GetLength() && test(text[end]))
		++end;
	return end - start;
}

//----------------------------------------------------------------------------------------
// A name this module gives to a texture: "image_baked_" or "image_pixels_", 1 to 16 hex
// digits, with @p numbered "-" and 1 to 10 decimal digits (the names in session folders;
// version 0.1.0 wrote them without), then ".exr". Exactly that, so that no other file is
// ever taken for one of ours.
//----------------------------------------------------------------------------------------
static maxon::Bool IsTextureFileName(const maxon::String& name, maxon::Bool numbered)
{
	maxon::Int position = 0;
	if (name.StartsWith("image_baked_"_s))
		position = 12;
	else if (name.StartsWith("image_pixels_"_s))
		position = 13;
	else
		return false;
	const maxon::Int hex = CountWhile(name, position, IsHexDigit);
	if (hex < 1 || hex > 16)
		return false;
	position += hex;
	if (numbered)
	{
		if (position >= name.GetLength() || name[position] != '-')
			return false;
		const maxon::Int digits = CountWhile(name, position + 1, [](maxon::Utf32Char c) { return c >= '0' && c <= '9'; });
		if (digits < 1 || digits > 10)
			return false;
		position += 1 + digits;
	}
	return name.GetLength() == position + 4 && name.EndsWith(".exr"_s);
}

// "session-<process id>-<hex time>", exactly.
static maxon::Bool IsSessionFolderName(const maxon::String& name)
{
	const maxon::Int start = 8;
	if (!name.StartsWith(maxon::String(SESSION_PREFIX)))
		return false;
	const maxon::Int digits = CountWhile(name, start, [](maxon::Utf32Char c) { return c >= '0' && c <= '9'; });
	if (digits < 1 || digits > 10 || start + digits >= name.GetLength() || name[start + digits] != '-')
		return false;
	const maxon::Int hex = CountWhile(name, start + digits + 1, IsHexDigit);
	return hex >= 1 && hex <= 16 && name.GetLength() == start + digits + 1 + hex;
}

static maxon::Bool DeleteFile(const cinema::Filename& file)
{
	cinema::GeFKill(file);
	return !cinema::GeFExist(file);
}

// The names of the entries of @p folder: files, or folders.
static maxon::Result<maxon::BaseArray<maxon::String>> ListFolder(const cinema::Filename& folder, maxon::Bool folders)
{
	iferr_scope;
	maxon::BaseArray<maxon::String> names;
	cinema::AutoAlloc<cinema::BrowseFiles> browse;
	if (!browse)
		return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
	browse->Init(folder, 0);
	while (browse->GetNext())
	{
		if (browse->IsDir() == folders)
		{
			names.Append(browse->GetFilename().GetString()) iferr_return;
		}
	}
	return names;
}

static maxon::Result<void> ToCString(const maxon::String& text, maxon::BaseArray<maxon::Char>& out)
{
	iferr_scope;
	out = text.GetCString() iferr_return;
	out.Append('\0') iferr_return;
	return maxon::OK;
}

// --- the store --------------------------------------------------------------------------------

// What is known of a document a file was made for.
enum class OWNERSTATE
{
	UNLISTED,   ///< never seen in the document list: kept until the end of the session
	LISTED,     ///< in the document list
	CLOSED      ///< was in the list and is not any more
} MAXON_ENUM_LIST(OWNERSTATE);

struct BakedFile
{
	cinema::Filename file;
	maxon::BaseArray<maxon::String> owners;           ///< document keys
	maxon::BaseArray<const BridgeClient*> clients;    ///< Blenders that may read it
	maxon::Int pins = 0;                              ///< scene writers that hold it
};

struct BakedStore
{
	maxon::HashMap<maxon::String, BakedFile> files;      ///< image item id -> its file
	maxon::HashMap<maxon::String, OWNERSTATE> owners;    ///< document key -> state
	maxon::BaseArray<cinema::Filename> undeleted;        ///< files whose deletion failed: tried again
	cinema::Filename folder;                             ///< this session's; empty until first used
	maxon::Bool folderFailed = false;
	void* lock = nullptr;                                ///< the lock file in the folder, held open
	maxon::Int nextNumber = 1;
	maxon::Int checksOnChange = 0;                       ///< for tests: checks of the document list, by cause,
	maxon::Int checksOnTimer = 0;
	maxon::Int closedOnChange = 0;                       ///< and documents found closed by each
	maxon::Int closedOnTimer = 0;
	maxon::Int deleted = 0;
};

// Guards g_store. Deleting a file is done while holding it, so that a file is never written
// and deleted at the same time; that takes a fraction of a millisecond.
static maxon::Spinlock g_lock;
static BakedStore* g_store = nullptr;
static maxon::Bool g_ended = false;

// The store, made on first use; nullptr after the end of the session or without memory.
static BakedStore* GetStore()
{
	if (!g_store && !g_ended)
	{
		iferr (g_store = NewObj(BakedStore))
			g_store = nullptr;
	}
	return g_store;
}

//----------------------------------------------------------------------------------------
// The key of a document: its MAXON_CREATOR_ID, which the copy a render works on shares with
// the document it was made from (measured: RenderDocument in c4dpy, docs/reference/c4d-sdk.md).
// "?" when it has none: such files stay until the end of the session.
//----------------------------------------------------------------------------------------
static maxon::String GetDocumentKey(const cinema::BaseDocument* doc)
{
	const maxon::Char* memory = nullptr;
	maxon::Int bytes = 0;
	if (!doc->FindUniqueID(cinema::MAXON_CREATOR_ID, memory, bytes) || !memory || bytes <= 0)
		return "?"_s;
	static const maxon::Char* const DIGITS = "0123456789abcdef";
	maxon::String key;
	for (maxon::Int i = 0; i < bytes; ++i)
	{
		const maxon::UChar value = maxon::UChar(memory[i]);
		key.AppendChar(maxon::Utf32Char(DIGITS[value >> 4])) iferr_ignore("A shorter key only makes two documents look alike.");
		key.AppendChar(maxon::Utf32Char(DIGITS[value & 15])) iferr_ignore("A shorter key only makes two documents look alike.");
	}
	return key;
}

// True when @p doc is in Cinema 4D's document list. Main thread only.
static maxon::Bool IsInDocumentList(const cinema::BaseDocument* doc)
{
	if (!doc || !cinema::GeIsMainThread())
		return false;
	for (const cinema::BaseDocument* listed = cinema::GetFirstDocument(); listed; listed = listed->GetNext())
	{
		if (listed == doc)
			return true;
	}
	return false;
}

// Adds a document to the owners of @p entry. g_lock is held.
static void AddOwner(BakedStore& store, BakedFile& entry, const maxon::String& key, maxon::Bool listed)
{
	if (key.IsEmpty())
		return;
	if (!entry.owners.Contains(key))
	{
		entry.owners.Append(key) iferr_ignore("Without the owner the file goes as soon as no Blender holds it; it is baked again when needed.");
	}
	maxon::Bool created = false;
	iferr (OWNERSTATE& state = store.owners.InsertKey(key, created))
		return;
	if (created)
		state = listed ? OWNERSTATE::LISTED : OWNERSTATE::UNLISTED;
	else if (listed)
		state = OWNERSTATE::LISTED;
	// A document that was closed stays closed for a render that still works on its copy: that
	// render's Blender holds the files it needs.
}

static maxon::Bool OwnersClosed(const BakedStore& store, const BakedFile& entry)
{
	for (const maxon::String& key : entry.owners)
	{
		const auto* const state = store.owners.Find(key);
		if (!state || state->GetValue() != OWNERSTATE::CLOSED)
			return false;
	}
	return true;
}

//----------------------------------------------------------------------------------------
// Deletes every file that may go (baked_files.h); with @p retry it also tries again the
// files whose deletion failed before (a program had them open). g_lock is held.
//----------------------------------------------------------------------------------------
static void Sweep(BakedStore& store, maxon::Bool retry)
{
	maxon::BaseArray<maxon::String> doomed;
	for (const auto& entry : store.files)
	{
		const BakedFile& file = entry.GetValue();
		if (file.pins == 0 && file.clients.IsEmpty() && OwnersClosed(store, file))
		{
			doomed.Append(entry.GetKey()) iferr_ignore("The file is deleted at the next sweep.");
		}
	}
	for (const maxon::String& id : doomed)
	{
		const auto* const entry = store.files.Find(id);
		if (!entry)
			continue;
		const cinema::Filename file = entry->GetValue().file;
		store.files.Erase(id) iferr_ignore("Erasing an entry that exists does not fail.");
		if (DeleteFile(file))
		{
			++store.deleted;
		}
		else
		{
			store.undeleted.Append(file) iferr_ignore("Left to the clean-up at the next start.");
		}
	}
	if (retry)
	{
		for (maxon::Int i = store.undeleted.GetCount() - 1; i >= 0; --i)
		{
			if (DeleteFile(store.undeleted[i]))
			{
				++store.deleted;
				store.undeleted.Erase(i) iferr_ignore("Erasing an element that exists does not fail.");
			}
		}
	}
}

// --- what the exporter, the writer and the client call ----------------------------------------

maxon::Result<maxon::String> FindBakedFile(const maxon::String& id, const cinema::BaseDocument* doc, SceneWriter& writer)
{
	iferr_scope;
	const maxon::String key = doc ? GetDocumentKey(doc) : maxon::String();
	const maxon::Bool listed = IsInDocumentList(doc);

	maxon::ScopedLock lock(g_lock);
	BakedStore* const store = g_store;
	if (!store)
		return maxon::String();
	auto* const found = store->files.Find(id);
	if (!found)
		return maxon::String();
	BakedFile& entry = found->GetValue();
	if (!cinema::GeFExist(entry.file))
	{
		// Deleted by someone else: bake again. The entry goes when nothing holds it.
		return maxon::String();
	}
	writer.HoldBakedFile(id) iferr_return;
	++entry.pins;
	AddOwner(*store, entry, key, listed);
	return entry.file.GetString();
}

cinema::Filename NewBakedFileName(const maxon::String& id)
{
	maxon::ScopedLock lock(g_lock);
	BakedStore* const store = GetStore();
	if (!store || store->folderFailed)
		return cinema::Filename();

	if (!store->folder.IsPopulated())
	{
		const cinema::Filename root = BakeRoot();
		const maxon::String name = maxon::String(SESSION_PREFIX) + FormatString("@-@", maxon::Int64(platform::CurrentProcessId()),
			maxon::String::HexToString(maxon::UInt64(platform::UnixTime() * 1000.0), false));
		const cinema::Filename folder = root + cinema::Filename(name);
		// Twice: a process starting at the same moment may take the empty folder for one left behind.
		for (maxon::Int attempt = 0; attempt < 2 && !store->lock; ++attempt)
		{
			if (!cinema::GeFExist(folder, true) && !cinema::GeFCreateDirRec(folder))
				continue;
			maxon::BaseArray<maxon::Char> path;
			iferr (ToCString((folder + cinema::Filename(maxon::String(LOCK_FILE))).GetString(), path))
				break;
			store->lock = platform::HoldFileLock(path.GetFirst(), true);
		}
		if (!store->lock)
		{
			// Without the lock another Cinema 4D could take the folder for one left behind.
			store->folderFailed = true;
			return cinema::Filename();
		}
		store->folder = folder;
	}
	const cinema::Filename file = store->folder + cinema::Filename(FormatString("@-@.exr", id, store->nextNumber));
	++store->nextNumber;
	return file;
}

maxon::Result<maxon::String> AddBakedFile(const maxon::String& id, const cinema::Filename& file, const cinema::BaseDocument* doc, SceneWriter& writer)
{
	iferr_scope;
	const maxon::String key = doc ? GetDocumentKey(doc) : maxon::String();
	const maxon::Bool listed = IsInDocumentList(doc);

	maxon::ScopedLock lock(g_lock);
	BakedStore* const store = GetStore();
	if (!store)
		return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
	writer.HoldBakedFile(id) iferr_return;

	auto* const found = store->files.Find(id);
	if (found && cinema::GeFExist(found->GetValue().file))
	{
		// Another render thread baked the same shader meanwhile: one file is enough.
		DeleteFile(file);
		BakedFile& entry = found->GetValue();
		++entry.pins;
		AddOwner(*store, entry, key, listed);
		return entry.file.GetString();
	}

	BakedFile created;
	created.file = file;
	created.pins = 1;
	if (found)
	{
		// The file of the old entry was deleted by someone else; whoever still held it keeps
		// holding the new one, which is harmless.
		created.clients = std::move(found->GetValue().clients);
		created.pins += found->GetValue().pins;
		created.owners = std::move(found->GetValue().owners);
		store->files.Erase(id) iferr_return;
	}
	auto& inserted = store->files.Insert(id, std::move(created)) iferr_return;
	AddOwner(*store, inserted.GetValue(), key, listed);
	return file.GetString();
}

void ReleaseBakedFiles(const maxon::BaseArray<maxon::String>& ids)
{
	if (ids.IsEmpty())
		return;
	maxon::ScopedLock lock(g_lock);
	if (!g_store)
		return;
	for (const maxon::String& id : ids)
	{
		if (auto* const entry = g_store->files.Find(id))
		{
			if (entry->GetValue().pins > 0)
				--entry->GetValue().pins;
		}
	}
	Sweep(*g_store, false);
}

static void RemoveClient(BakedFile& entry, const BridgeClient* client)
{
	for (maxon::Int i = entry.clients.GetCount() - 1; i >= 0; --i)
	{
		if (entry.clients[i] == client)
			entry.clients.Erase(i) iferr_ignore("Erasing an element that exists does not fail.");
	}
}

void NoteBakedFilesSent(const BridgeClient* client, const maxon::BaseArray<maxon::String>& ids, const maxon::BaseArray<maxon::String>& removed,
	maxon::Bool clear)
{
	maxon::ScopedLock lock(g_lock);
	if (!g_store)
		return;
	if (clear)
	{
		for (auto& entry : g_store->files)
			RemoveClient(entry.GetValue(), client);
	}
	for (const maxon::String& id : removed)
	{
		if (auto* const entry = g_store->files.Find(id))
			RemoveClient(entry->GetValue(), client);
	}
	for (const maxon::String& id : ids)
	{
		auto* const entry = g_store->files.Find(id);
		if (entry && !entry->GetValue().clients.Contains(client))
		{
			// Without the entry a file could go while this Blender still reads it: then it reports
			// a missing texture. Memory for one pointer is not expected to run out.
			entry->GetValue().clients.Append(client) iferr_ignore("See above.");
		}
	}
	Sweep(*g_store, false);
}

void ForgetBakedFileClient(const BridgeClient* client)
{
	maxon::ScopedLock lock(g_lock);
	if (!g_store)
		return;
	for (auto& entry : g_store->files)
		RemoveClient(entry.GetValue(), client);
	Sweep(*g_store, false);
}

// --- the document list ----------------------------------------------------------------------

static void CheckDocuments(maxon::Bool onTimer)
{
	if (!cinema::GeIsMainThread())
		return;
	{
		maxon::ScopedLock lock(g_lock);
		if (!g_store || (g_store->owners.IsEmpty() && g_store->undeleted.IsEmpty()))
			return;
	}
	maxon::BaseArray<maxon::String> listed;
	for (const cinema::BaseDocument* doc = cinema::GetFirstDocument(); doc; doc = doc->GetNext())
	{
		listed.Append(GetDocumentKey(doc)) iferr_ignore("A document left out is taken for closed only if it was seen before; it is seen again next time.");
	}

	maxon::ScopedLock lock(g_lock);
	if (!g_store)
		return;
	if (onTimer)
		++g_store->checksOnTimer;
	else
		++g_store->checksOnChange;
	for (auto& owner : g_store->owners)
	{
		if (listed.Contains(owner.GetKey()))
		{
			owner.GetValue() = OWNERSTATE::LISTED;
		}
		else if (owner.GetValue() == OWNERSTATE::LISTED)
		{
			owner.GetValue() = OWNERSTATE::CLOSED;
			++(onTimer ? g_store->closedOnTimer : g_store->closedOnChange);
		}
	}
	Sweep(*g_store, onTimer);
}

void CheckOpenDocuments()
{
	CheckDocuments(false);
}

// --- start and end of the session -------------------------------------------------------------

//----------------------------------------------------------------------------------------
// Empties one session folder of another process if that process is not running any more:
// then its lock file can be opened. Files are deleted only by the names this module gives;
// the folder goes only when that left it empty.
//----------------------------------------------------------------------------------------
static maxon::Result<void> RemoveSessionFolder(const cinema::Filename& folder)
{
	iferr_scope;
	const cinema::Filename lockFile = folder + cinema::Filename(maxon::String(LOCK_FILE));
	if (!cinema::GeFExist(lockFile))
	{
		// No lock: a session that is just being set up, or one this clean-up finished but for
		// the folder. Only an empty folder is removed.
		const maxon::BaseArray<maxon::String> files = ListFolder(folder, false) iferr_return;
		const maxon::BaseArray<maxon::String> folders = ListFolder(folder, true) iferr_return;
		if (files.IsEmpty() && folders.IsEmpty())
			cinema::GeFKill(folder, GE_FKILL_DIRECTORY);
		return maxon::OK;
	}

	maxon::BaseArray<maxon::Char> path;
	ToCString(lockFile.GetString(), path) iferr_return;
	void* const lock = platform::HoldFileLock(path.GetFirst(), false);
	if (!lock)
		return maxon::OK;   // that session is still running
	const maxon::BaseArray<maxon::String> files = ListFolder(folder, false) iferr_return;
	maxon::Bool others = false;
	for (const maxon::String& name : files)
	{
		if (IsTextureFileName(name, true))
			DeleteFile(folder + cinema::Filename(name));
		else if (name != maxon::String(LOCK_FILE))
			others = true;
	}
	platform::ReleaseFileLock(lock);
	if (others)
		return maxon::OK;   // not ours: the folder and its lock stay
	DeleteFile(lockFile);
	const maxon::BaseArray<maxon::String> left = ListFolder(folder, false) iferr_return;
	const maxon::BaseArray<maxon::String> folders = ListFolder(folder, true) iferr_return;
	if (left.IsEmpty() && folders.IsEmpty())
		cinema::GeFKill(folder, GE_FKILL_DIRECTORY);
	return maxon::OK;
}

static maxon::Result<void> RemoveEarlierSessions()
{
	iferr_scope;
	const cinema::Filename root = BakeRoot();
	if (!cinema::GeFExist(root, true))
		return maxon::OK;

	maxon::String own;
	{
		maxon::ScopedLock lock(g_lock);
		if (g_store && g_store->folder.IsPopulated())
			own = g_store->folder.GetFileString();
	}
	const maxon::BaseArray<maxon::String> folders = ListFolder(root, true) iferr_return;
	for (const maxon::String& name : folders)
	{
		if (IsSessionFolderName(name) && name != own)
		{
			RemoveSessionFolder(root + cinema::Filename(name)) iferr_return;
		}
	}
	// Version 0.1.0 wrote its files into the baked folder itself and never deleted them. A file
	// that is open in a program cannot be deleted and stays.
	const maxon::BaseArray<maxon::String> files = ListFolder(root, false) iferr_return;
	for (const maxon::String& name : files)
	{
		if (IsTextureFileName(name, false))
			DeleteFile(root + cinema::Filename(name));
	}
	return maxon::OK;
}

void RemoveBakedFilesOfEarlierSessions()
{
	iferr (RemoveEarlierSessions())
	{
		ApplicationOutput("Cycles Render: baked textures of earlier sessions could not be removed: @", err);
	}
}

void FreeBakedFiles()
{
	maxon::ScopedLock lock(g_lock);
	g_ended = true;
	BakedStore* const store = g_store;
	if (!store)
		return;
	// Every Blender has ended by now: nothing holds the files any more.
	for (const auto& entry : store->files)
		DeleteFile(entry.GetValue().file);
	for (const cinema::Filename& file : store->undeleted)
		DeleteFile(file);
	if (store->lock)
	{
		platform::ReleaseFileLock(store->lock);
		const cinema::Filename lockFile = store->folder + cinema::Filename(maxon::String(LOCK_FILE));
		DeleteFile(lockFile);
		// Only when nothing is left: a file that could not be deleted keeps the folder, and the
		// next start, finding no lock, leaves it alone. GE_FKILL_DIRECTORY without
		// GE_FKILL_RECURSIVE removes an empty folder only.
		cinema::GeFKill(store->folder, GE_FKILL_DIRECTORY);
	}
	DeleteObj(g_store);
}

// --- for tests --------------------------------------------------------------------------------

static const maxon::Char* OwnerStateName(OWNERSTATE state)
{
	switch (state)
	{
		case OWNERSTATE::UNLISTED: return "unlisted";
		case OWNERSTATE::LISTED: return "listed";
		case OWNERSTATE::CLOSED: return "closed";
	}
	return "?";
}

maxon::String DescribeBakedFiles()
{
	JsonWriter json;
	json.BeginObject();
	maxon::ScopedLock lock(g_lock);
	if (g_store)
	{
		json.String("folder", g_store->folder.GetString());
		json.Int("checks_on_change", g_store->checksOnChange);
		json.Int("checks_on_timer", g_store->checksOnTimer);
		json.Int("closed_on_change", g_store->closedOnChange);
		json.Int("closed_on_timer", g_store->closedOnTimer);
		json.Int("deleted", g_store->deleted);
		json.Int("undeleted", g_store->undeleted.GetCount());
		// "<document key>:<state>" for every document files were made for.
		json.BeginArray("owners");
		for (const auto& owner : g_store->owners)
			json.String(nullptr, owner.GetKey() + ":"_s + maxon::String(OwnerStateName(owner.GetValue())));
		json.EndArray();
		json.BeginArray("files");
		for (const auto& entry : g_store->files)
		{
			const BakedFile& file = entry.GetValue();
			json.BeginObject();
			json.String("id", entry.GetKey());
			json.String("file", file.file.GetString());
			json.Int("pins", file.pins);
			json.Int("clients", file.clients.GetCount());
			json.BeginArray("owners");
			for (const maxon::String& key : file.owners)
				json.String(nullptr, key);
			json.EndArray();
			json.EndObject();
		}
		json.EndArray();
	}
	json.EndObject();
	return json.GetText();
}

// --- the message hook -------------------------------------------------------------------------

// Watches the document list on Cinema 4D's main thread: on every change (a document closed
// sends one), and once a second for whatever no change announced.
class BakedFilesWatch : public cinema::MessageData
{
public:
	virtual cinema::Int32 GetTimer() override { return 1000; }
	virtual cinema::Bool CoreMessage(cinema::Int32 id, const cinema::BaseContainer& bc) override
	{
		if (id == EVMSG_CHANGE)
			CheckDocuments(false);
		else if (id == MSG_TIMER)
			CheckDocuments(true);
		return true;
	}
};

cinema::Bool RegisterBakedFilesWatch()
{
	iferr (BakedFilesWatch* const watch = NewObj(BakedFilesWatch))
		return false;
	return cinema::RegisterMessagePlugin(ID_BAKED_FILES_WATCH, maxon::String(), 0, watch);
}

} // namespace cyclesrender
