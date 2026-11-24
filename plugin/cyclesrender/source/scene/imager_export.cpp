// SPDX-License-Identifier: Apache-2.0
// The Imager tab of a Cycles Camera tag as a `compositor` item (imager_export.h).

#include "c4d.h"

#include "tcyclescamera.h"

#include "../materials/node_graph.h"
#include "../tags/camera_tag.h"
#include "imager_export.h"

namespace cyclesrender
{

maxon::String ImagerItemId()
{
	return "compositor"_s;
}

maxon::Result<void> ImagerChain::CopyFrom(const ImagerChain& other)
{
	iferr_scope;
	layers.CopyFrom(other.layers) iferr_return;
	values.CopyFrom(other.values) iferr_return;
	unknown.CopyFrom(other.unknown) iferr_return;
	return maxon::OK;
}

maxon::Result<ImagerChain> ImagerChain::Read(const cinema::BaseObject* camera)
{
	iferr_scope;
	ImagerChain chain;
	const cinema::BaseTag* const tag = FindCameraTag(camera);
	if (!tag)
		return chain;
	const cinema::BaseContainer& data = tag->GetDataInstanceRef();
	if (!data.GetBool(CYCLESCAMERA_IMAGER_ENABLE, true))
		return chain;
	maxon::BaseArray<maxon::Int32> order;
	GetImagerLayers(data, order) iferr_return;
	// The list's bottom layer is applied first.
	for (maxon::Int i = order.GetCount() - 1; i >= 0; --i)
	{
		const maxon::Int32 layer = order[i];
		if (!data.GetBool(ImagerLayerId(layer, IMAGERLAYER_ENABLED), true))
			continue;
		const ImagerEffect* const effect = GetImagerLayerEffect(data, layer);
		maxon::String name = data.GetString(ImagerLayerId(layer, IMAGERLAYER_NAME));
		if (name.IsEmpty())
			name = data.GetString(ImagerLayerId(layer, IMAGERLAYER_EFFECT));
		if (!effect || data.GetInt32(ImagerLayerId(layer, IMAGERLAYER_SOURCE), IMAGERLAYER_SOURCE_BLENDER) != IMAGERLAYER_SOURCE_BLENDER)
		{
			chain.unknown.Append(name) iferr_return;
			continue;
		}
		Layer values;
		values.effect = effect;
		values.name = name;
		values.opacity = maxon::Clamp01(data.GetFloat(ImagerLayerId(layer, IMAGERLAYER_OPACITY), 1.0));
		values.firstValue = chain.values.GetCount();
		for (maxon::Int32 s = 0; s < effect->settingCount; ++s)
		{
			const ImagerSetting& setting = effect->settings[s];
			const maxon::Int32 id = ImagerLayerId(layer, setting.number);
			Value value;
			switch (setting.kind)
			{
				case 'f': value.number[0] = data.GetFloat(id, setting.value[0]); break;
				case 'i': value.number[0] = data.GetInt32(id, maxon::Int32(setting.value[0])); break;
				case 'b': value.number[0] = data.GetBool(id, setting.value[0] != 0.0) ? 1.0 : 0.0; break;
				case 'm':
				{
					// A stored number no option stands for (a scene of a newer plugin): Blender's default.
					const maxon::Int32 option = FindImagerOption(setting, data.GetInt32(id, GetImagerOptionValue(setting, setting.defaultOption)));
					value.option = option == NOTOK ? setting.defaultOption : option;
					break;
				}
				default:
				{
					const cinema::Vector vector = data.GetVector(id, cinema::Vector(setting.value[0], setting.value[1], setting.value[2]));
					value.number[0] = vector.x;
					value.number[1] = vector.y;
					value.number[2] = vector.z;
					break;
				}
			}
			chain.values.Append(value) iferr_return;
		}
		chain.layers.Append(std::move(values)) iferr_return;
	}
	return chain;
}

// True when @p value is the setting's default in the table.
static maxon::Bool IsDefault(const ImagerSetting& setting, const ImagerChain::Value& value)
{
	if (setting.kind == 'm')
		return value.option == setting.defaultOption;
	const maxon::Int32 count = (setting.kind == 'c' || setting.kind == 'v') ? 3 : 1;
	for (maxon::Int32 i = 0; i < count; ++i)
	{
		if (maxon::Abs(value.number[i] - setting.value[i]) > 1e-6)
			return false;
	}
	return true;
}

static maxon::String VersionText(maxon::Int32 code)
{
	return FormatString("@.@", code / 100, code % 100);
}

maxon::Result<maxon::Bool> ExportImager(SceneWriter& writer, const ImagerChain& chain, const DocumentColors& colors, maxon::Int32 blenderVersion)
{
	iferr_scope;
	for (const maxon::String& name : chain.unknown)
		writer.Warn(FormatString("Imager: the effect of the layer \"@\" is not known to this version of the plugin; it is left out.", name));

	NodeGraph graph;
	const maxon::Int render = graph.Add("CompositorNodeRLayers") iferr_return;
	NodeOutput picture = { render, "Image"_s };
	maxon::Int count = 0;
	for (const ImagerChain::Layer& layer : chain.layers)
	{
		const ImagerEffect& effect = *layer.effect;
		if (effect.since > blenderVersion)
		{
			writer.Warn(FormatString("Imager: the effect \"@\" (layer \"@\") needs Blender @ or newer; Blender @ renders without it.", maxon::String(effect.name),
				layer.name, VersionText(effect.since), VersionText(blenderVersion)));
			continue;
		}
		// Blender's node group of that name, from its own bundled assets.
		const maxon::Int node = graph.Add("CompositorNodeGroup") iferr_return;
		graph.SetGroup(node, maxon::String(effect.name));
		for (maxon::Int32 s = 0; s < effect.settingCount; ++s)
		{
			const ImagerSetting& setting = effect.settings[s];
			const ImagerChain::Value& value = chain.values[layer.firstValue + s];
			if (setting.since > blenderVersion)
			{
				if (!IsDefault(setting, value))
					writer.Warn(FormatString("Imager: \"@\" of the effect \"@\" needs Blender @ or newer; Blender @ renders without it.", maxon::String(setting.label),
						maxon::String(effect.name), VersionText(setting.since), VersionText(blenderVersion)));
				continue;
			}
			switch (setting.kind)
			{
				case 'f': graph.SetInput(node, setting.identifier, value.number[0]); break;
				case 'i': graph.SetInputJson(node, setting.identifier, maxon::String::IntToString(maxon::Int64(value.number[0]))); break;
				case 'b': graph.SetInputJson(node, setting.identifier, value.number[0] != 0.0 ? "true"_s : "false"_s); break;
				case 'm':
				{
					if (GetImagerOptionSince(setting, value.option) > blenderVersion)
					{
						writer.Warn(FormatString("Imager: \"@\" for \"@\" of the effect \"@\" needs Blender @ or newer; Blender @ uses its own choice.",
							GetImagerOption(setting, value.option), maxon::String(setting.label), maxon::String(effect.name),
							VersionText(GetImagerOptionSince(setting, value.option)), VersionText(blenderVersion)));
						break;
					}
					graph.SetInputJson(node, setting.identifier, JsonQuoted(GetImagerOption(setting, value.option)));
					break;
				}
				case 'c':
					// A colour as Cinema 4D's renderers calculate with it, like a light's.
					graph.SetInput(node, setting.identifier, colors.Input(cinema::Vector(value.number[0], value.number[1], value.number[2])));
					break;
				default:
					// A vector: the bridge takes as many numbers as the socket has (two for a place in the picture).
					graph.SetInput(node, setting.identifier, cinema::Vector(value.number[0], value.number[1], value.number[2]));
					break;
			}
		}
		graph.Link(picture, node, effect.imageIn) iferr_return;
		// The opacity: a Mix of what came before (A) and the effect's result (B). It is there at
		// 100 % too, so that another opacity is another value and never another graph: the bridge
		// then sets it on the tree it has, and the render's samples stay.
		const maxon::Int mix = graph.Add("ShaderNodeMix") iferr_return;
		graph.SetProperty(mix, "data_type", "RGBA"_s);
		graph.SetProperty(mix, "blend_type", "MIX"_s);
		graph.SetInput(mix, "Factor_Float", layer.opacity);
		graph.Link(picture, mix, "A_Color") iferr_return;
		graph.Link({ node, maxon::String(effect.imageOut) }, mix, "B_Color") iferr_return;
		picture = { mix, "Result_Color"_s };
		++count;
	}
	if (count == 0)
		return false;
	// The tree's one output is the picture (the bridge gives the tree that socket).
	const maxon::Int output = graph.Add("NodeGroupOutput") iferr_return;
	maxon::Int firstSocket = 0;
	graph.Link(picture, output, firstSocket) iferr_return;

	JsonWriter& json = writer.BeginItem("compositor", ImagerItemId());
	graph.Write(json);
	writer.EndItem();
	return true;
}

} // namespace cyclesrender
