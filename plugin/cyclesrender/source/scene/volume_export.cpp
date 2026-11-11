// SPDX-License-Identifier: Apache-2.0
// Volume grids as a `volume` item with its material (volume_export.h).

#include "c4d.h"
#include "ddoc.h"
#include "lib_volumeobject.h"
#include "maxon/hashmap.h"
#include "maxon/volume.h"
#include "maxon/volumetools.h"
#include "ovolumesequence.h"
#include "ovolumeset.h"

#include "tcyclesobject.h"

#include "../ids.h"
#include "../materials/node_graph.h"
#include "../tags/object_tag.h"
#include "parameters.h"
#include "volume_export.h"

namespace cyclesrender
{

// What was last written for a volume item: which grids, and into which file.
struct WrittenVolume
{
	maxon::UInt64 source = 0;
	cinema::Filename file;
};

static maxon::HashMap<maxon::String, WrittenVolume> g_written;
static maxon::Spinlock g_writtenLock;

static cinema::Filename VolumeFolder()
{
	return cinema::GeGetC4DPath(C4D_PATH_PREFS) + cinema::Filename("cyclesrender"_s) + cinema::Filename("volumes"_s);
}

void FreeVolumeFiles()
{
	maxon::ScopedLock lock(g_writtenLock);
	for (const auto& entry : g_written)
		cinema::GeFKill(entry.GetValue().file);
	g_written.Reset();
}

static maxon::String JsonText(const maxon::String& text)
{
	maxon::String escaped = text;
	escaped.Replace("\\"_s, "\\\\"_s) iferr_ignore("An odd grid name only fails to match.");
	escaped.Replace("\""_s, "\\\""_s) iferr_ignore("An odd grid name only fails to match.");
	return "\""_s + escaped + "\""_s;
}

// The .vdb file a Volume Loader read these grids from, or an empty name.
static cinema::Filename FindLoadedFile(const VolumeSource& source)
{
	if (!source.set)
		return cinema::Filename();
	cinema::Filename file = GetFilenameParameter(source.set, ID_VOLUMESET_PATH);
	if (!file.IsPopulated())
	{
		const maxon::String text = GetStringParameter(source.set, ID_VOLUMESET_PATH);
		if (text.IsPopulated())
			file = cinema::Filename(text);
	}
	if (file.IsPopulated() && cinema::GeFExist(file))
		return file;

	// A loader names the frame's Volume Set after its file.
	if (source.owner && source.owner->GetType() == Ovolumeloader)
	{
		cinema::Filename first = GetFilenameParameter(source.owner, ID_VOLUMESEQUENCE_PATH);
		if (!first.IsPopulated())
			first = cinema::Filename(GetStringParameter(source.owner, ID_VOLUMESEQUENCE_PATH));
		if (first.IsPopulated())
		{
			const maxon::String name = FormatString("@.vdb", source.set->GetName());
			file = first.GetDirectory() + cinema::Filename(name);
			if (cinema::GeFExist(file))
				return file;
		}
	}
	return cinema::Filename();
}

// How many document units one unit of a loaded file is.
static maxon::Float64 LoadedUnitScale(const VolumeSource& source, const cinema::BaseDocument* doc)
{
	const cinema::BaseList2D* holder = nullptr;
	maxon::Int32 parameter = 0;
	if (source.owner && source.owner->GetType() == Ovolumeloader)
	{
		holder = source.owner;
		parameter = ID_VOLUMESEQUENCE_UNIT_SCALE;
	}
	else if (source.set)
	{
		holder = source.set;
		parameter = ID_VOLUMESET_UNIT_SCALE;
	}
	if (!holder || !doc)
		return 1.0;

	const cinema::GeData fileData = GetParameterData(holder, parameter);
	cinema::GeData documentData;
	doc->GetParameter(ConstDescIDLevel(cinema::DOCUMENT_DOCUNIT), documentData, cinema::DESCFLAGS_GET::NONE);
	const cinema::UnitScaleData* const fileUnit = fileData.GetCustomDataType<cinema::UnitScaleData>();
	const cinema::UnitScaleData* const documentUnit = documentData.GetCustomDataType<cinema::UnitScaleData>();
	if (!fileUnit || !documentUnit)
		return 1.0;
	const maxon::Float64 scale = cinema::CalculateTranslationScale(fileUnit, documentUnit);
	return scale > 0.0 ? scale : 1.0;
}

static maxon::Result<maxon::UInt64> HashFile(const cinema::Filename& file)
{
	iferr_scope;

	cinema::AutoAlloc<cinema::BaseFile> handle;
	if (!handle || !handle->Open(file, cinema::FILEOPEN::READ, cinema::FILEDIALOG::NONE, cinema::BYTEORDER::V_INTEL))
		return maxon::IoError(MAXON_SOURCE_LOCATION, maxon::Url(), "The volume file could not be read back."_s);
	maxon::BaseArray<maxon::Char> buffer;
	buffer.Resize(1 << 20) iferr_return;
	maxon::UInt64 hash = 0x766F6C756D65ull;
	maxon::Int64 remaining = handle->GetLength();
	while (remaining > 0)
	{
		const maxon::Int count = maxon::Int(maxon::Min(remaining, maxon::Int64(buffer.GetCount())));
		if (handle->ReadBytes(buffer.GetFirst(), count, true) != count)
			break;
		hash = HashBytes(buffer.GetFirst(), count, hash);
		remaining -= count;
	}
	handle->Close();
	return hash;
}

// Saves the grids to a .vdb file named after its content and returns the file.
static maxon::Result<cinema::Filename> SaveGrids(const maxon::String& id, const maxon::BaseArray<const cinema::VolumeObject*>& grids,
	maxon::UInt64 sourceKey)
{
	iferr_scope;

	{
		maxon::ScopedLock lock(g_writtenLock);
		const auto* const known = g_written.Find(id);
		if (known && known->GetValue().source == sourceKey && cinema::GeFExist(known->GetValue().file))
			return known->GetValue().file;
	}

	const cinema::Filename folder = VolumeFolder();
	if (!cinema::GeFExist(folder, true) && !cinema::GeFCreateDirRec(folder))
		return maxon::IoError(MAXON_SOURCE_LOCATION, maxon::Url(), "The folder for volume files could not be created."_s);

	maxon::BaseArray<maxon::Volume> volumes;
	for (const cinema::VolumeObject* grid : grids)
	{
		volumes.Append(maxon::Volume(grid->GetVolume())) iferr_return;
	}
	const cinema::Filename temporary = folder + cinema::Filename(id + ".writing.vdb"_s);
	const maxon::Url url = cinema::MaxonConvert(temporary, cinema::MAXONCONVERTMODE::WRITE);
	maxon::VolumeToolsInterface::SaveVDBFile(url, 1.0, volumes) iferr_return;

	const maxon::UInt64 hash = HashFile(temporary) iferr_return;
	const cinema::Filename file = folder + cinema::Filename(id + "_"_s + maxon::String::HexToString(hash, false) + ".vdb"_s);
	if (cinema::GeFExist(file))
		cinema::GeFKill(temporary);
	else if (!cinema::GeFMove(temporary, file))
		return maxon::IoError(MAXON_SOURCE_LOCATION, maxon::Url(), "The volume file could not be renamed."_s);

	maxon::ScopedLock lock(g_writtenLock);
	const auto* const known = g_written.Find(id);
	if (known && known->GetValue().file != file)
		cinema::GeFKill(known->GetValue().file);   // fails harmlessly while Blender still reads it
	WrittenVolume written;
	written.source = sourceKey;
	written.file = file;
	g_written.Insert(id, written) iferr_return;
	return file;
}

maxon::Result<maxon::Bool> ExportVolume(SceneWriter& writer, const Coords& coords, cinema::BaseDocument* doc, const maxon::String& id,
	const VolumeSource& source)
{
	iferr_scope;

	// The grids that can render, and their names.
	maxon::BaseArray<const cinema::VolumeObject*> grids;
	maxon::BaseArray<maxon::String> names;
	maxon::String firstFog;
	maxon::UInt64 sourceKey = maxon::UInt64(reinterpret_cast<maxon::UInt>(doc));
	for (const cinema::VolumeObject* grid : source.grids)
	{
		const maxon::VolumeInterface* const volume = grid ? grid->GetVolume() : nullptr;
		if (!volume)
			continue;
		const maxon::GRIDCLASS gridClass = volume->GetGridClass();
		if (gridClass == maxon::GRIDCLASS::SDF)
			continue;
		const maxon::String name = volume->GetGridName();
		if (gridClass == maxon::GRIDCLASS::FOG && firstFog.IsEmpty())
			firstFog = name;
		grids.Append(grid) iferr_return;
		names.Append(name) iferr_return;
		sourceKey = HashBytes(reinterpret_cast<const maxon::Char*>(&volume), SIZEOF(volume), sourceKey);
	}
	if (grids.IsEmpty())
		return false;
	// Files of other programs (EmberGen) leave the class of their grids unset: any grid that is
	// no distance field can be fog.
	if (firstFog.IsEmpty())
		firstFog = names[0];

	// The file.
	maxon::Float64 unitScale = 1.0;
	cinema::Filename file = FindLoadedFile(source);
	const maxon::Bool loaded = file.IsPopulated();
	if (loaded)
	{
		unitScale = LoadedUnitScale(source, doc);
	}
	else
	{
		if (source.owner)
			sourceKey = sourceKey * 31 + maxon::UInt64(source.owner->GetDirty(cinema::DIRTYFLAGS::CACHE | cinema::DIRTYFLAGS::DATA));
		iferr (file = SaveGrids(id, grids, sourceKey))
		{
			writer.Warn(FormatString("Volume \"@\" could not be written for Blender and is left out. @", source.owner ? source.owner->GetName() : maxon::String(), err));
			return false;
		}
	}

	// The material: a Principled Volume reading the grids by name.
	const cinema::BaseTag* const tag = FindObjectTag(source.owner, CYCLESOBJECT_VOLUME_ENABLE);
	auto has = [&names](const maxon::Char* name) -> maxon::Bool
	{
		return names.Contains(maxon::String(name));
	};
	maxon::String densityGrid = GetStringParameter(tag, CYCLESOBJECT_VOLUME_DENSITY_GRID);
	if (densityGrid.IsEmpty())
		densityGrid = has("density") ? "density"_s : firstFog;
	maxon::String temperatureGrid = GetStringParameter(tag, CYCLESOBJECT_VOLUME_TEMPERATURE_GRID);
	// "flames" is what EmberGen calls its fire grid; "heat" and "flame" are Blender's and Houdini's.
	for (const maxon::Char* name : { "temperature", "flames", "flame", "heat" })
	{
		if (temperatureGrid.IsEmpty() && has(name))
			temperatureGrid = maxon::String(name);
	}
	maxon::String colorGrid = GetStringParameter(tag, CYCLESOBJECT_VOLUME_COLOR_GRID);
	if (colorGrid.IsEmpty())
		colorGrid = has("color") ? "color"_s : (has("Cd") ? "Cd"_s : maxon::String());

	NodeGraph graph;
	const maxon::Int shader = graph.Add("ShaderNodeVolumePrincipled") iferr_return;
	const maxon::Int output = graph.Add("ShaderNodeOutputMaterial") iferr_return;
	graph.SetInputJson(shader, "Density Attribute", JsonText(densityGrid));
	graph.SetInputJson(shader, "Color Attribute", JsonText(colorGrid));
	graph.SetInput(shader, "Density", GetFloatParameter(tag, CYCLESOBJECT_VOLUME_DENSITY, 1.0));
	graph.SetInput(shader, "Color", GetVectorParameter(tag, CYCLESOBJECT_VOLUME_COLOR, cinema::Vector(0.5)));
	graph.SetInput(shader, "Anisotropy", GetFloatParameter(tag, CYCLESOBJECT_VOLUME_ANISOTROPY, 0.0));
	const maxon::Float64 emission = GetFloatParameter(tag, CYCLESOBJECT_VOLUME_EMISSION_STRENGTH, 0.0);
	graph.SetInput(shader, "Emission Strength", emission);
	graph.SetInput(shader, "Emission Color", GetVectorParameter(tag, CYCLESOBJECT_VOLUME_EMISSION_COLOR, cinema::Vector(1.0)));

	// Fire. A real temperature (Blender's own smoke: the grid times "Temperature" is kelvin) glows
	// as a black body. The fire grids of other programs (EmberGen's "flames", 0 to about 4) say
	// how bright the fire is: there the grid drives the emission's strength, and its colour is
	// that of a black body at grid times "Temperature".
	const maxon::Float64 fire = GetFloatParameter(tag, CYCLESOBJECT_VOLUME_BLACKBODY, 1.0);
	const maxon::Float64 kelvin = GetFloatParameter(tag, CYCLESOBJECT_VOLUME_TEMPERATURE, 1000.0);
	maxon::Int32 fireMode = GetIntParameter(tag, CYCLESOBJECT_VOLUME_FIRE_MODE, CYCLESOBJECT_VOLUME_FIRE_MODE_AUTO);
	if (fireMode == CYCLESOBJECT_VOLUME_FIRE_MODE_AUTO)
		fireMode = temperatureGrid == "temperature"_s ? CYCLESOBJECT_VOLUME_FIRE_MODE_TEMPERATURE : CYCLESOBJECT_VOLUME_FIRE_MODE_BRIGHTNESS;
	const maxon::Bool brightness = temperatureGrid.IsPopulated() && fire > 0.0 && fireMode == CYCLESOBJECT_VOLUME_FIRE_MODE_BRIGHTNESS;
	graph.SetInputJson(shader, "Temperature Attribute", JsonText(brightness ? maxon::String() : temperatureGrid));
	if (brightness)
	{
		const maxon::Int attribute = graph.Add("ShaderNodeAttribute") iferr_return;
		graph.SetProperty(attribute, "attribute_name", temperatureGrid);
		const maxon::Int toKelvin = graph.Add("ShaderNodeMath") iferr_return;
		graph.SetProperty(toKelvin, "operation", "MULTIPLY"_s);
		graph.SetInput(toKelvin, "Value_001", kelvin);
		graph.Link(NodeOutput { attribute, "Fac"_s }, toKelvin, "Value") iferr_return;
		const maxon::Int colour = graph.Add("ShaderNodeBlackbody") iferr_return;
		graph.Link(NodeOutput { toKelvin, "Value"_s }, colour, "Temperature") iferr_return;
		graph.Link(NodeOutput { colour, "Color"_s }, shader, "Emission Color") iferr_return;
		// strength = grid * intensity + the tag's own even glow
		const maxon::Int strength = graph.Add("ShaderNodeMath") iferr_return;
		graph.SetProperty(strength, "operation", "MULTIPLY_ADD"_s);
		graph.SetInput(strength, "Value_001", fire);
		graph.SetInput(strength, "Value_002", emission);
		graph.Link(NodeOutput { attribute, "Fac"_s }, strength, "Value") iferr_return;
		graph.Link(NodeOutput { strength, "Value"_s }, shader, "Emission Strength") iferr_return;
	}
	// A temperature grid glows as fire. Without one nothing may glow: the node would fall back to
	// one temperature for the whole grid.
	graph.SetInput(shader, "Temperature", kelvin);
	graph.SetInput(shader, "Blackbody Intensity", temperatureGrid.IsPopulated() && !brightness ? fire : 0.0);
	graph.Link(NodeOutput { shader, "Volume"_s }, output, "Volume") iferr_return;

	const maxon::String materialId = id + ".material"_s;
	{
		JsonWriter& json = writer.BeginItem("material", materialId);
		graph.Write(json);
		writer.EndItem();
	}

	maxon::Float64 matrix[16];
	// A grid in Cinema 4D's memory holds its place in the world, and so does the file saved from
	// it. A loaded file holds what its author gave it and is placed by the loader.
	coords.RawMatrix(loaded ? source.mg : cinema::Matrix(), unitScale, matrix);
	JsonWriter& json = writer.BeginItem("volume", id);
	json.String("path", file.GetString());
	json.Floats("matrix", matrix, 16);
	json.BeginArray("materials");
	json.String(nullptr, materialId);
	json.EndArray();
	writer.EndItem();
	return true;
}

} // namespace cyclesrender
