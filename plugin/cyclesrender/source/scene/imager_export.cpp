// SPDX-License-Identifier: Apache-2.0
// The camera's exposure and the Imager tab of a Cycles Camera tag as a `compositor` item (imager_export.h).

#include "c4d.h"

#include "tcyclescamera.h"

#include "../materials/node_graph.h"
#include "../tags/camera_tag.h"
#include "imager_built.h"
#include "imager_export.h"
#include "redshift_camera.h"

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
	exposure = other.exposure;
	notes.CopyFrom(other.notes) iferr_return;
	return maxon::OK;
}

maxon::Result<void> ImagerChain::AppendLayer(const maxon::Char* effectName, maxon::Int32 source, const maxon::String& name,
	std::initializer_list<Given> given)
{
	iferr_scope;
	const ImagerEffect* const effect = FindImagerEffect(maxon::String(effectName), source);
	if (!effect)
		return maxon::OK;
	Layer layer;
	layer.effect = effect;
	layer.name = name;
	layer.firstValue = values.GetCount();
	for (maxon::Int32 s = 0; s < effect->settingCount; ++s)
	{
		const ImagerSetting& setting = effect->settings[s];
		Value value;
		value.option = setting.defaultOption;
		for (maxon::Int i = 0; i < 3; ++i)
			value.number[i] = setting.value[i];
		for (const Given& item : given)
		{
			if (maxon::String(item.identifier) != maxon::String(setting.identifier))
				continue;
			if (setting.kind == 'm' && item.option)
			{
				for (maxon::Int32 o = 0; o < GetImagerOptionCount(setting); ++o)
				{
					if (GetImagerOption(setting, o) == maxon::String(item.option))
						value.option = o;
				}
			}
			else
			{
				for (maxon::Int i = 0; i < 3; ++i)
					value.number[i] = item.number[i];
			}
		}
		values.Append(value) iferr_return;
	}
	layers.Append(std::move(layer)) iferr_return;
	return maxon::OK;
}

maxon::Result<ImagerChain> ImagerChain::Read(const cinema::BaseObject* camera, const RedshiftTranslation& redshift)
{
	iferr_scope;
	ImagerChain chain;
	if (!camera)
		return chain;
	if (redshift.exposure)
	{
		// A Redshift camera's own exposure counts with a tag and without one; the Color tab's multiplies with it.
		RedshiftExposure exposure = ReadRedshiftExposure(camera) iferr_return;
		chain.exposure = exposure.stops;
		chain.notes = std::move(exposure.notes);
	}
	// Then what Redshift does to the picture after it (tone mapping, bloom ...), as layers before the tag's own.
	ReadRedshiftEffects(camera, chain, redshift) iferr_return;
	const cinema::BaseTag* const tag = FindCameraTag(camera);
	if (!tag)
		return chain;
	const cinema::BaseContainer& data = tag->GetDataInstanceRef();
	chain.exposure += data.GetFloat(CYCLESCAMERA_COLOR_EXPOSURE, CAMERATAG_DEFAULT::EXPOSURE);
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
		// An effect is found by its name and source: a bundled group or a plain node (a .blend file's group is reserved).
		if (!effect)
		{
			chain.unknown.Append(name) iferr_return;
			continue;
		}
		Layer values;
		values.effect = effect;
		values.name = name;
		values.opacity = maxon::Clamp01(data.GetFloat(ImagerLayerId(layer, IMAGERLAYER_OPACITY), 1.0));
		// A layer saved before the engine existed applies in both, as it did.
		values.engine = data.GetInt32(ImagerLayerId(layer, IMAGERLAYER_ENGINE), IMAGERLAYER_ENGINE_BOTH);
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

// The node of a bundled group or of a plain node, with the layer's values set (@p values: one per setting of
// @p effect). What the Blender in use lacks is left out and named.
static maxon::Result<NodeOutput> AddEffectNode(SceneWriter& writer, NodeGraph& graph, const ImagerEffect& effect, const ImagerChain::Value* values,
	const DocumentColors& colors, maxon::Int32 blenderVersion)
{
	iferr_scope;
	// Blender's node group of that name, from its own bundled assets; or a plain node of that type.
	const maxon::Bool plain = effect.source == IMAGERLAYER_SOURCE_NODE;
	const maxon::Int node = graph.Add(plain ? effect.name : "CompositorNodeGroup") iferr_return;
	if (!plain)
		graph.SetGroup(node, maxon::String(effect.name));
	for (maxon::Int32 s = 0; s < effect.settingCount; ++s)
	{
		const ImagerSetting& setting = effect.settings[s];
		const ImagerChain::Value& value = values[s];
		if (setting.since > blenderVersion)
		{
			if (!IsDefault(setting, value))
				writer.Warn(FormatString("Imager: \"@\" of the effect \"@\" needs Blender @ or newer; Blender @ renders without it.", maxon::String(setting.label),
					maxon::String(effect.label), VersionText(setting.since), VersionText(blenderVersion)));
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
						GetImagerOption(setting, value.option), maxon::String(setting.label), maxon::String(effect.label),
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
	return NodeOutput{ node, maxon::String(effect.imageOut) };
}

maxon::Result<maxon::Bool> ExportImager(SceneWriter& writer, const ImagerChain& chain, const DocumentColors& colors, const ImagerTarget& target)
{
	iferr_scope;
	const maxon::Int32 blenderVersion = target.blenderVersion;
	for (const maxon::String& name : chain.unknown)
		writer.Warn(FormatString("Imager: the effect of the layer \"@\" is not known to this version of the plugin; it is left out.", name));
	for (const maxon::String& note : chain.notes)
		writer.Warn(note);

	NodeGraph graph;
	maxon::BaseArray<maxon::String> passes;
	const maxon::Int render = graph.Add("CompositorNodeRLayers") iferr_return;
	NodeOutput picture = { render, "Image"_s };
	// The camera's exposure, on the scene-linear picture before every layer (Blender's Exposure node multiplies by
	// 2^stops, in 5.0 to 5.3, beyond its declared -10 to 10 too: measured, docs/reference/blender.md, "The compositor").
	const maxon::Int exposure = graph.Add("CompositorNodeExposure") iferr_return;
	graph.SetInput(exposure, "Exposure", chain.exposure);
	graph.Link(picture, exposure, "Image") iferr_return;
	picture = { exposure, "Image"_s };
	maxon::Int count = 0;
	for (const ImagerChain::Layer& layer : chain.layers)
	{
		const ImagerEffect& effect = *layer.effect;
		// A layer meant for the other engine is no part of this picture (and no warning: it is as asked).
		if ((layer.engine == IMAGERLAYER_ENGINE_CYCLES && target.eevee) || (layer.engine == IMAGERLAYER_ENGINE_EEVEE && !target.eevee))
			continue;
		if (effect.since > blenderVersion)
		{
			writer.Warn(FormatString("Imager: the effect \"@\" (layer \"@\") needs Blender @ or newer; Blender @ renders without it.", maxon::String(effect.label),
				layer.name, VersionText(effect.since), VersionText(blenderVersion)));
			continue;
		}
		NodeOutput result;
		if (effect.source == IMAGERLAYER_SOURCE_BUILT)
		{
			// Several of Blender's nodes, made by the plugin (imager_built.cpp).
			ImagerBuildContext context{ graph, render, colors, target, writer, passes };
			result = BuildImagerEffect(context, effect, &chain.values[layer.firstValue], layer.name, picture) iferr_return;
		}
		else
		{
			result = AddEffectNode(writer, graph, effect, &chain.values[layer.firstValue], colors, blenderVersion) iferr_return;
			graph.Link(picture, result.node, effect.imageIn) iferr_return;
		}
		// The opacity: a Mix of what came before (A) and the effect's result (B). It is there at
		// 100 % too, so that another opacity is another value and never another graph: the bridge
		// then sets it on the tree it has, and the render's samples stay.
		const maxon::Int mix = graph.Add("ShaderNodeMix") iferr_return;
		graph.SetProperty(mix, "data_type", "RGBA"_s);
		graph.SetProperty(mix, "blend_type", "MIX"_s);
		graph.SetInput(mix, "Factor_Float", layer.opacity);
		graph.Link(picture, mix, "A_Color") iferr_return;
		graph.Link(result, mix, "B_Color") iferr_return;
		picture = { mix, "Result_Color"_s };
		++count;
	}
	if (count == 0 && chain.exposure == 0.0)
		return false;
	// The tree's one output is the picture (the bridge gives the tree that socket).
	const maxon::Int output = graph.Add("NodeGroupOutput") iferr_return;
	maxon::Int firstSocket = 0;
	graph.Link(picture, output, firstSocket) iferr_return;

	JsonWriter& json = writer.BeginItem("compositor", ImagerItemId());
	graph.Write(json);
	// The passes the tree reads: the bridge keeps them on while it has the tree.
	if (!passes.IsEmpty())
	{
		json.BeginArray("view_layer");
		for (const maxon::String& pass : passes)
			json.String(nullptr, pass);
		json.EndArray();
	}
	writer.EndItem();
	return true;
}

} // namespace cyclesrender
