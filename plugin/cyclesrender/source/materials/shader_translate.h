// SPDX-License-Identifier: Apache-2.0
// Turns a Cinema 4D shader (what sits in a material channel's Texture field) into Blender nodes.

#ifndef CYCLESRENDER_SHADER_TRANSLATE_H__
#define CYCLESRENDER_SHADER_TRANSLATE_H__

#include "c4d.h"

#include "node_graph.h"

namespace cyclesrender
{

class MaterialExporter;

//----------------------------------------------------------------------------------------
/// Three ways, tried in this order:
///
/// 1. Native: shaders with an exact counterpart become nodes: bitmap, colour, checkerboard,
///    gradient (along U or V), colorizer, fresnel, fusion and layer (for the blend modes
///    Blender computes the same way), vertex map, MoGraph colour. They stay sharp at any distance and cost nothing
///    when they change.
/// 2. Baked: any other shader that depends only on the UV coordinates (noise, filter, tiles,
///    the gradient's other types, ...) is rendered by Cinema 4D into a texture, once, and
///    again only when it changes (MaterialExporter::GetBakedImageId).
/// 3. Neither: the caller falls back to the channel's plain value, and the user is told.
///
/// A translator that cannot do what a shader's settings ask returns an invalid output;
/// then way 2 is tried. Add shaders in Native().
//----------------------------------------------------------------------------------------
class ShaderTranslator
{
public:
	ShaderTranslator(MaterialExporter& exporter, NodeGraph& graph, const maxon::String& materialName)
		: _exporter(exporter), _graph(graph), _materialName(materialName) { }

	/// Where textures are looked up: an output giving UV coordinates in Blender's convention.
	/// Invalid (the default): the mesh's render UV set.
	/// @param[in] box   The output is a point in a unit cube instead, and images are put on the
	///                  cube's six sides (Cinema 4D's cubic projection).
	/// @param[in] clip  Images are not repeated outside the UV square (the tag's "Tile" is off).
	void SetCoordinates(const NodeOutput& uv, maxon::Bool box = false, maxon::Bool clip = false)
	{
		_uv = uv;
		_box = box;
		_clip = clip;
	}
	const NodeOutput& GetCoordinates() const { return _uv; }

	/// The shader's colour. Invalid when @p shader is null or could not be translated; in the
	/// second case a warning naming @p channel has been issued.
	/// @param[in] colorData  False for data such as heights and masks: files are read without colour conversion.
	maxon::Result<NodeOutput> Translate(cinema::BaseShader* shader, maxon::Bool colorData, const maxon::Char* channel);

	/// The alpha channel of a bitmap shader's file. Invalid for anything else.
	maxon::Result<NodeOutput> ImageAlpha(cinema::BaseShader* shader);

	/// True when the shader and everything it reads depend only on the UV coordinates.
	static maxon::Bool CanBake(cinema::BaseShader* shader);

private:
	maxon::Result<NodeOutput> Native(cinema::BaseShader* shader, maxon::Bool colorData);
	maxon::Result<NodeOutput> Any(cinema::BaseShader* shader, maxon::Bool colorData);
	maxon::Result<NodeOutput> Image(const maxon::String& imageId, maxon::Bool closest, const maxon::Char* output);

	maxon::Result<NodeOutput> Bitmap(cinema::BaseShader* shader, maxon::Bool colorData);
	maxon::Result<NodeOutput> PlainColor(cinema::BaseShader* shader);
	maxon::Result<NodeOutput> Checkerboard(cinema::BaseShader* shader);
	maxon::Result<NodeOutput> GradientShader(cinema::BaseShader* shader);
	maxon::Result<NodeOutput> Colorizer(cinema::BaseShader* shader, maxon::Bool colorData);
	maxon::Result<NodeOutput> Fresnel(cinema::BaseShader* shader);
	maxon::Result<NodeOutput> Fusion(cinema::BaseShader* shader, maxon::Bool colorData);
	maxon::Result<NodeOutput> Layer(cinema::BaseShader* shader, maxon::Bool colorData);
	maxon::Result<NodeOutput> MeshAttribute(cinema::BaseShader* shader);

	/// A Color Ramp node set up like the gradient in parameter @p id, fed by @p factor.
	/// Invalid when the gradient uses something a Color Ramp does not have.
	maxon::Result<NodeOutput> Ramp(cinema::BaseShader* shader, maxon::Int32 id, const NodeOutput& factor);
	/// One of U and V of the texture coordinates as a number, in Blender's convention (V = 0 at the bottom).
	maxon::Result<NodeOutput> Coordinate(maxon::Bool v);
	/// Mixes @p top over @p bottom with one of the layer shader's blend modes. Invalid for modes Blender computes differently.
	maxon::Result<NodeOutput> Blend(maxon::Int32 layerBlendMode, const NodeOutput& bottom, const NodeOutput& top, maxon::Float64 amount, const NodeOutput& mask);
	maxon::Result<NodeOutput> Invert(const NodeOutput& color);
	maxon::Result<NodeOutput> Brightness(const NodeOutput& color);

	MaterialExporter& _exporter;
	NodeGraph& _graph;
	maxon::String _materialName;
	NodeOutput _uv;
	maxon::Bool _box = false;
	maxon::Bool _clip = false;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_SHADER_TRANSLATE_H__
