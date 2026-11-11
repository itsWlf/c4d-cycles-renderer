// SPDX-License-Identifier: Apache-2.0
// Node materials through Cinema 4D's material exchange (exchange_material.h): the material is
// asked to describe itself as a Standard Surface, and a Principled BSDF is built from that.

#include "c4d.h"
#include "c4d_basematerial.h"
#include "maxon/material/datadescription_material_standard_surface.h"
#include "maxon/material/materialexport.h"
#include "maxon/material/materialparameter.h"

#include "exchange_material.h"
#include "material_export.h"

namespace cyclesrender
{

namespace surface = maxon::MATERIAL::PORTBUNDLE::STANDARDSURFACE;

// One entry of the description: its value, and its texture if it has one.
template <typename TYPE> struct Entry
{
	TYPE value;
	NodeOutput texture;
};

class ExchangeReader
{
public:
	ExchangeReader(MaterialExporter& exporter, NodeGraph& graph, const maxon::material::MaterialExchangeData& data)
		: _exporter(exporter), _graph(graph), _data(data) { }

	template <typename TYPE, typename KEY> maxon::Result<Entry<TYPE>> Read(KEY&& id, const TYPE& fallback, maxon::Bool colorData)
	{
		iferr_scope;

		Entry<TYPE> entry;
		entry.value = fallback;
		iferr (const maxon::material::TypedConstantParameter<TYPE> parameter = maxon::material::ParameterStorageInterface::Extract<TYPE>(_data._parameters, id))
			return entry;
		entry.value = parameter._value;

		const auto* const texture = _data._textures.Find(parameter._id);
		if (!texture)
			return entry;
		const maxon::Data& textureData = texture->GetValue();
		maxon::String image;
		maxon::Bool isFile = false;
		ifnoerr (const maxon::material::ImageReference& reference = textureData.Get<maxon::material::ImageReference>())
		{
			isFile = true;
			const maxon::String path = reference._absolutePath.GetSystemPath() iferr_return;
			image = _exporter.GetFileImageId(path, colorData) iferr_return;
		}
		if (!isFile)
		{
			ifnoerr (const maxon::material::TextureBuffer& buffer = textureData.Get<maxon::material::TextureBuffer>())
			{
				if (buffer._pixels.GetPointer())
				{
					image = _exporter.GetPixelImageId(*buffer._pixels, maxon::Int32(buffer._dimensions.x), maxon::Int32(buffer._dimensions.y)) iferr_return;
				}
			}
		}
		if (image.IsPopulated())
		{
			const maxon::Int node = _graph.Add("ShaderNodeTexImage") iferr_return;
			_graph.SetProperty(node, "image", image);
			entry.texture = NodeOutput { node, "Color"_s };
		}
		return entry;
	}

private:
	MaterialExporter& _exporter;
	NodeGraph& _graph;
	const maxon::material::MaterialExchangeData& _data;
};

// Measured: a Standard node material's colours arrive with the sRGB curve applied (a base
// colour of 0.9, 0.05, 0.05 comes as 0.955, 0.248, 0.248); a Redshift material's arrive as set.
static maxon::Float64 UndoSrgbCurve(maxon::Float64 value)
{
	return value <= 0.04045 ? value / 12.92 : maxon::Pow((value + 0.055) / 1.055, 2.4);
}

static cinema::Vector ToVector(const maxon::Color& color, maxon::Bool undoCurve)
{
	if (undoCurve)
		return cinema::Vector(UndoSrgbCurve(color.r), UndoSrgbCurve(color.g), UndoSrgbCurve(color.b));
	return cinema::Vector(color.r, color.g, color.b);
}

maxon::Result<maxon::Bool> BuildExchangeSurface(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, NodeOutput& surfaceOutput)
{
	iferr_scope;

	// The node space the material is described from: its own, if it is a node material.
	maxon::Id space = cinema::GetActiveNodeSpaceId();
	const cinema::NodeMaterial* const nodeMaterial = static_cast<const cinema::NodeMaterial*>(material);
	for (const maxon::Id& candidate : { maxon::Id("com.redshift3d.redshift4c4d.class.nodespace"), maxon::Id("net.maxon.nodespace.standard") })
	{
		if (nodeMaterial->HasSpace(candidate))
		{
			space = candidate;
			break;
		}
	}

	const maxon::Bool undoCurve = space == maxon::Id("net.maxon.nodespace.standard");
	const maxon::Int32 size = exporter.GetBakeResolution();
	maxon::DataDictionary config;
	maxon::material::MaterialTypesMap types;
	types.Insert(surface::GetId(), maxon::material::MaterialTypeSupport::DIRECT) iferr_return;
	config.Set(maxon::material::EXPORT::CONFIG::MATERIALTYPESWITHSUPPORT, std::move(types)) iferr_return;
	config.Set(maxon::material::EXPORT::CONFIG::NODESPACEID, space) iferr_return;
	config.Set(maxon::material::EXPORT::CONFIG::TEXTUREDIMENSIONS, maxon::IntVector2d(size, size)) iferr_return;
	config.Set(maxon::material::EXPORT::CONFIG::TEXTURESUPPORT,
		maxon::material::EXPORT::TextureSupport::TEXTUREBUFFER | maxon::material::EXPORT::TextureSupport::IMAGEREFERENCE) iferr_return;

	iferr (const maxon::material::MaterialExchangeData data = maxon::material::MaterialExportInterface::Export(*material, config))
		return false;
	if (data._materialTypeId != surface::GetId())
		return false;

	ExchangeReader reader(exporter, graph, data);
	const Entry<maxon::Float> base = reader.Read(surface::BASE, maxon::Float(1.0), false) iferr_return;
	const Entry<maxon::Color> baseColor = reader.Read(surface::BASE_COLOR, maxon::Color(0.8), true) iferr_return;
	const Entry<maxon::Float> metalness = reader.Read(surface::METALNESS, maxon::Float(0.0), false) iferr_return;
	const Entry<maxon::Float> specular = reader.Read(surface::SPECULAR, maxon::Float(1.0), false) iferr_return;
	const Entry<maxon::Float> roughness = reader.Read(surface::SPECULAR_ROUGHNESS, maxon::Float(0.2), false) iferr_return;
	const Entry<maxon::Float> ior = reader.Read(surface::SPECULAR_IOR, maxon::Float(1.5), false) iferr_return;
	const Entry<maxon::Float> transmission = reader.Read(surface::TRANSMISSION, maxon::Float(0.0), false) iferr_return;
	const Entry<maxon::Float> coat = reader.Read(surface::COAT, maxon::Float(0.0), false) iferr_return;
	const Entry<maxon::Float> coatRoughness = reader.Read(surface::COAT_ROUGHNESS, maxon::Float(0.1), false) iferr_return;
	const Entry<maxon::Float> coatIor = reader.Read(surface::COAT_IOR, maxon::Float(1.5), false) iferr_return;
	const Entry<maxon::Float> sheen = reader.Read(surface::SHEEN, maxon::Float(0.0), false) iferr_return;
	const Entry<maxon::Float> emission = reader.Read(surface::EMISSION, maxon::Float(0.0), false) iferr_return;
	const Entry<maxon::Color> emissionColor = reader.Read(surface::EMISSION_COLOR, maxon::Color(1.0), true) iferr_return;
	const Entry<maxon::Color> opacity = reader.Read(surface::OPACITY, maxon::Color(1.0), false) iferr_return;

	const maxon::Int bsdf = graph.Add("ShaderNodeBsdfPrincipled") iferr_return;
	auto number = [&graph, bsdf](const maxon::Char* socket, const Entry<maxon::Float>& entry, maxon::Float64 scale) -> maxon::Result<void>
	{
		iferr_scope;
		if (entry.texture.IsValid() && scale == 1.0)
		{
			graph.Link(entry.texture, bsdf, socket) iferr_return;
		}
		else
		{
			graph.SetInput(bsdf, socket, maxon::Float64(entry.value) * scale);
		}
		return maxon::OK;
	};

	if (baseColor.texture.IsValid())
	{
		graph.Link(baseColor.texture, bsdf, "Base Color") iferr_return;
	}
	else
	{
		graph.SetInput(bsdf, "Base Color", ToVector(baseColor.value, undoCurve) * maxon::Float64(base.value));
	}
	number("Metallic", metalness, 1.0) iferr_return;
	// Standard Surface's specular weight of 1 is the Principled BSDF's level of 0.5.
	number("Specular IOR Level", specular, 0.5) iferr_return;
	number("Roughness", roughness, 1.0) iferr_return;
	graph.SetInput(bsdf, "IOR", maxon::Max(1.0, maxon::Float64(ior.value)));
	number("Transmission Weight", transmission, 1.0) iferr_return;
	number("Coat Weight", coat, 1.0) iferr_return;
	number("Coat Roughness", coatRoughness, 1.0) iferr_return;
	graph.SetInput(bsdf, "Coat IOR", maxon::Max(1.0, maxon::Float64(coatIor.value)));
	number("Sheen Weight", sheen, 1.0) iferr_return;

	if (emissionColor.texture.IsValid())
	{
		graph.Link(emissionColor.texture, bsdf, "Emission Color") iferr_return;
	}
	else
	{
		graph.SetInput(bsdf, "Emission Color", ToVector(emissionColor.value, undoCurve));
	}
	graph.SetInput(bsdf, "Emission Strength", maxon::Float64(emission.value));

	if (opacity.texture.IsValid())
	{
		graph.Link(opacity.texture, bsdf, "Alpha") iferr_return;
	}
	else
	{
		graph.SetInput(bsdf, "Alpha", maxon::Clamp01((opacity.value.r + opacity.value.g + opacity.value.b) / 3.0));
	}

	surfaceOutput = NodeOutput { bsdf, "BSDF"_s };
	return true;
}

} // namespace cyclesrender
