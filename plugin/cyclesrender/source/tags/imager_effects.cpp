// SPDX-License-Identifier: Apache-2.0
// The table of Blender's compositor effects and the layers of the Imager tab (imager_effects.h).

#include "c4d.h"

#include "tcyclescamera.h"

#include "imager_effects.h"

namespace cyclesrender
{

#include "imager_effects_table.h"

maxon::Block<const ImagerEffect> GetImagerEffects()
{
	return maxon::Block<const ImagerEffect>(IMAGER_EFFECTS, SIZEOF(IMAGER_EFFECTS) / SIZEOF(IMAGER_EFFECTS[0]));
}

const ImagerEffect* FindImagerEffect(const maxon::String& name)
{
	for (const ImagerEffect& effect : GetImagerEffects())
	{
		if (name == maxon::String(effect.name))
			return &effect;
	}
	return nullptr;
}

// The part number @p index of @p text, whose parts are separated by @p separator; false past the last.
static maxon::Bool Part(const maxon::Char* text, maxon::Char separator, maxon::Int32 index, const maxon::Char*& start, maxon::Int& length)
{
	if (!text || !*text)
		return false;
	const maxon::Char* at = text;
	for (maxon::Int32 i = 0; ; ++i)
	{
		const maxon::Char* end = at;
		while (*end && *end != separator)
			++end;
		if (i == index)
		{
			start = at;
			length = end - at;
			return true;
		}
		if (!*end)
			return false;
		at = end + 1;
	}
}

maxon::Int32 GetImagerOptionCount(const ImagerSetting& setting)
{
	maxon::Int32 count = 0;
	const maxon::Char* start = nullptr;
	maxon::Int length = 0;
	while (Part(setting.options, '\n', count, start, length))
		++count;
	return count;
}

maxon::String GetImagerOption(const ImagerSetting& setting, maxon::Int32 index)
{
	const maxon::Char* start = nullptr;
	maxon::Int length = 0;
	if (!Part(setting.options, '\n', index, start, length))
		return maxon::String();
	maxon::String name;
	iferr (name.SetCString(start, length))
		return maxon::String();
	return name;
}

maxon::Int32 GetImagerOptionSince(const ImagerSetting& setting, maxon::Int32 index)
{
	const maxon::Char* start = nullptr;
	maxon::Int length = 0;
	if (!Part(setting.optionSince, ',', index, start, length))
		return setting.since;
	maxon::Int32 code = 0;
	for (maxon::Int i = 0; i < length && start[i] >= '0' && start[i] <= '9'; ++i)
		code = code * 10 + (start[i] - '0');
	return code;
}

maxon::Int32 GetImagerOptionValue(const ImagerSetting& setting, maxon::Int32 index)
{
	// FNV-1a over the name's bytes, as a positive number: the same for as long as Blender keeps the name.
	const maxon::Char* start = nullptr;
	maxon::Int length = 0;
	if (!Part(setting.options, '\n', index, start, length))
		return 0;
	maxon::UInt32 hash = 2166136261u;
	for (maxon::Int i = 0; i < length; ++i)
		hash = (hash ^ maxon::UChar(start[i])) * 16777619u;
	return maxon::Int32(hash & 0x7fffffffu);
}

maxon::Int32 FindImagerOption(const ImagerSetting& setting, maxon::Int32 value)
{
	const maxon::Int32 count = GetImagerOptionCount(setting);
	for (maxon::Int32 i = 0; i < count; ++i)
	{
		if (GetImagerOptionValue(setting, i) == value)
			return i;
	}
	return NOTOK;
}

maxon::Int32 ImagerLayerId(maxon::Int32 layer, maxon::Int32 field)
{
	return IMAGERLAYER_BASE + layer * IMAGERLAYER_STRIDE + field;
}

maxon::Bool ImagerLayerOfId(maxon::Int32 id, maxon::Int32& layer, maxon::Int32& field)
{
	if (id < IMAGERLAYER_BASE + IMAGERLAYER_STRIDE)
		return false;
	layer = (id - IMAGERLAYER_BASE) / IMAGERLAYER_STRIDE;
	field = (id - IMAGERLAYER_BASE) % IMAGERLAYER_STRIDE;
	return true;
}

maxon::Result<void> GetImagerLayers(const cinema::BaseContainer& data, maxon::BaseArray<maxon::Int32>& layers)
{
	iferr_scope;
	layers.Reset();
	const cinema::BaseContainer order = data.GetContainer(CYCLESCAMERA_IMAGER_LAYERS);
	// The order container holds the layers' numbers under 0, 1, 2, ...
	for (maxon::Int32 i = 0; ; ++i)
	{
		const maxon::Int32 layer = order.GetInt32(i, 0);
		if (layer <= 0)
			break;
		// A number without an effect is no layer (a damaged file): pass it over.
		if (data.FindIndex(ImagerLayerId(layer, IMAGERLAYER_EFFECT)) == NOTOK || layers.Contains(layer))
			continue;
		layers.Append(layer) iferr_return;
	}
	return maxon::OK;
}

// Removes everything layer @p layer stores.
static void EraseImagerLayer(cinema::BaseContainer& data, maxon::Int32 layer)
{
	maxon::BaseArray<maxon::Int32> ids;
	maxon::Int32 index = 0;
	while (true)
	{
		const maxon::Int32 id = data.GetIndexId(index++);
		if (id == NOTOK)
			break;
		maxon::Int32 owner = 0, field = 0;
		if (ImagerLayerOfId(id, owner, field) && owner == layer)
		{
			iferr (ids.Append(id))
				return;
		}
	}
	for (const maxon::Int32 id : ids)
		data.RemoveData(id);
}

void SetImagerLayers(cinema::BaseContainer& data, const maxon::BaseArray<maxon::Int32>& layers)
{
	maxon::BaseArray<maxon::Int32> before;
	iferr (GetImagerLayers(data, before))
		return;
	cinema::BaseContainer order;
	maxon::Int32 count = 0;
	for (const maxon::Int32 layer : layers)
	{
		if (before.Contains(layer))
			order.SetInt32(count++, layer);
	}
	for (const maxon::Int32 layer : before)
	{
		if (!layers.Contains(layer))
			EraseImagerLayer(data, layer);
	}
	data.SetContainer(CYCLESCAMERA_IMAGER_LAYERS, order);
	// The selected layer is always one of the list.
	const maxon::Int32 selected = data.GetInt32(CYCLESCAMERA_IMAGER_SELECTED, 0);
	if (!layers.Contains(selected) || !before.Contains(selected))
		data.SetInt32(CYCLESCAMERA_IMAGER_SELECTED, count > 0 ? order.GetInt32(0) : 0);
}

const ImagerEffect* GetImagerLayerEffect(const cinema::BaseContainer& data, maxon::Int32 layer)
{
	return FindImagerEffect(data.GetString(ImagerLayerId(layer, IMAGERLAYER_EFFECT)));
}

cinema::GeData GetImagerDefault(const ImagerSetting& setting)
{
	switch (setting.kind)
	{
		case 'f': return cinema::GeData(maxon::Float(setting.value[0]));
		case 'i': return cinema::GeData(maxon::Int32(setting.value[0]));
		case 'b': return cinema::GeData(setting.value[0] != 0.0);
		case 'm': return cinema::GeData(GetImagerOptionValue(setting, setting.defaultOption));
		default: return cinema::GeData(cinema::Vector(setting.value[0], setting.value[1], setting.value[2]));
	}
}

maxon::Int32 AddImagerLayer(cinema::BaseContainer& data, const ImagerEffect& effect)
{
	maxon::BaseArray<maxon::Int32> layers;
	iferr (GetImagerLayers(data, layers))
		return 0;
	// A number no layer of this tag ever had, so that what refers to a layer (an animation track of
	// its opacity) never finds another one under its number.
	maxon::Int32 layer = maxon::Max(data.GetInt32(CYCLESCAMERA_IMAGER_NEXT, 1), maxon::Int32(1));
	for (const maxon::Int32 used : layers)
		layer = maxon::Max(layer, used + 1);
	if (layer > IMAGERLAYER_MAXIMUM)
		return 0;
	data.SetInt32(CYCLESCAMERA_IMAGER_NEXT, layer + 1);

	data.SetString(ImagerLayerId(layer, IMAGERLAYER_EFFECT), maxon::String(effect.name));
	data.SetInt32(ImagerLayerId(layer, IMAGERLAYER_SOURCE), IMAGERLAYER_SOURCE_BLENDER);
	data.SetString(ImagerLayerId(layer, IMAGERLAYER_NAME), maxon::String(effect.name));
	data.SetBool(ImagerLayerId(layer, IMAGERLAYER_ENABLED), true);
	data.SetFloat(ImagerLayerId(layer, IMAGERLAYER_OPACITY), 1.0);
	for (maxon::Int32 i = 0; i < effect.settingCount; ++i)
		data.SetData(ImagerLayerId(layer, effect.settings[i].number), GetImagerDefault(effect.settings[i]));

	cinema::BaseContainer order;
	order.SetInt32(0, layer);
	maxon::Int32 count = 1;
	for (const maxon::Int32 other : layers)
		order.SetInt32(count++, other);
	data.SetContainer(CYCLESCAMERA_IMAGER_LAYERS, order);
	data.SetInt32(CYCLESCAMERA_IMAGER_SELECTED, layer);
	return layer;
}

void RemoveImagerLayer(cinema::BaseContainer& data, maxon::Int32 layer)
{
	maxon::BaseArray<maxon::Int32> layers;
	iferr (GetImagerLayers(data, layers))
		return;
	const maxon::Int index = layers.FindIndex(layer);
	if (index == NOTOK)
		return;
	// The layer below takes the selection, or the one above at the list's end.
	const maxon::Int32 next = index + 1 < layers.GetCount() ? layers[index + 1] : (index > 0 ? layers[index - 1] : 0);
	iferr (layers.Erase(index))
		return;
	if (data.GetInt32(CYCLESCAMERA_IMAGER_SELECTED, 0) == layer)
		data.SetInt32(CYCLESCAMERA_IMAGER_SELECTED, next);
	SetImagerLayers(data, layers);
}

maxon::Bool MoveImagerLayer(cinema::BaseContainer& data, maxon::Int32 layer, maxon::Int32 direction)
{
	maxon::BaseArray<maxon::Int32> layers;
	iferr (GetImagerLayers(data, layers))
		return false;
	const maxon::Int index = layers.FindIndex(layer);
	const maxon::Int target = index + (direction < 0 ? -1 : 1);
	if (index == NOTOK || target < 0 || target >= layers.GetCount())
		return false;
	layers[index] = layers[target];
	layers[target] = layer;
	SetImagerLayers(data, layers);
	return true;
}

maxon::Int32 GetSelectedImagerLayer(const cinema::BaseContainer& data)
{
	maxon::BaseArray<maxon::Int32> layers;
	iferr (GetImagerLayers(data, layers))
		return 0;
	const maxon::Int32 selected = data.GetInt32(CYCLESCAMERA_IMAGER_SELECTED, 0);
	if (layers.Contains(selected))
		return selected;
	return layers.IsEmpty() ? 0 : layers[0];
}

} // namespace cyclesrender
