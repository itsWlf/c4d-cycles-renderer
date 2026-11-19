// SPDX-License-Identifier: Apache-2.0
// Translates Cinema 4D materials into Blender node graphs and writes them as scene items.

#ifndef CYCLESRENDER_MATERIAL_EXPORT_H__
#define CYCLESRENDER_MATERIAL_EXPORT_H__

#include "c4d.h"
#include "maxon/hashmap.h"

#include "../scene/document_colors.h"
#include "../scene/scene_writer.h"
#include "node_graph.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// How a texture tag lays a material's textures on the object.
//----------------------------------------------------------------------------------------
struct TextureMapping
{
	maxon::Int32 projection = 6;       ///< TEXTURETAG_PROJECTION_...; 6 is UVW
	maxon::Float64 offsetX = 0.0;      ///< as on the tag: fractions of the UV square
	maxon::Float64 offsetY = 0.0;
	maxon::Float64 lengthX = 1.0;      ///< 1 / tiles
	maxon::Float64 lengthY = 1.0;
	maxon::Bool tile = true;
	cinema::Matrix ml;                 ///< the tag's texture axes, in the object's space (projections other than UVW)

	/// The settings of @p tag; for nullptr the plain UVW mapping.
	static TextureMapping Read(const cinema::BaseTag* tag);
	/// True when textures simply follow the UV coordinates.
	maxon::Bool IsPlain() const;
	/// A text that differs when the mapping does.
	maxon::String GetKey() const;
};

/// The Blender object properties (protocol: `properties` of an `object`, `floats` of an
/// `instancer`) that hold the keys Redshift's random nodes vary by: a number in 0..1 from the
/// object's path of names, and its Redshift Object ID (SceneExporter::Leaf::nameKey). A material
/// reads them with an Attribute node of type INSTANCER. They are sent only for objects whose
/// material reads them (MaterialExporter::ReadsObjectKeys).
static const maxon::Char* const OBJECT_NAME_KEY = "cr_name_key";
static const maxon::Char* const OBJECT_ID_KEY = "cr_object_id";

/// One material of a stack on an object, with the mapping of its texture tag.
struct MaterialLayer
{
	cinema::BaseMaterial* material = nullptr;
	TextureMapping mapping;
};

//----------------------------------------------------------------------------------------
/// The material ids one export handed out, by what each material item was made from (the materials of
/// a stack and their mappings): for writing some objects again later without writing a material
/// (MaterialExporter::SetKnownIds; the IPR's update of single objects). The ids count up in
/// the order materials are first asked for, so only the export that made them can name them.
//----------------------------------------------------------------------------------------
struct MaterialIdTable
{
	maxon::HashMap<maxon::String, maxon::String> ids;      ///< what a material item was made from -> its id
	maxon::HashMap<maxon::String, maxon::Bool> keyed;      ///< ids of materials that read the object keys
	maxon::Bool hasDefault = false;                        ///< the default material was written
};

//----------------------------------------------------------------------------------------
/// Hands out the Blender material id for a Cinema 4D material, writing the `material` item
/// (and the `image` items it needs) the first time a material is asked for. Ask for the id
/// before opening the item that refers to it: items are built by the bridge in the order
/// they are written.
///
/// Which translator a material goes to (MaterialExporter::WriteLayers, GetMaterialId):
/// - A classic material: its channels become a Principled BSDF (ClassicMaterialGraph in
///   material_export.cpp): colour, luminance, transparency, reflectance (one specular lobe and
///   a clear coat), alpha, bump, normal and displacement, placed as its texture tag says.
///   Textures go through ShaderTranslator (shader_translate.h): nodes where Blender has the
///   same thing, a baked texture otherwise.
/// - A material of Cycles nodes: written node for node (cycles_node_material.h).
/// - A Redshift material: its Standard Material node, option by option (redshift_material.h).
/// - Any other node material: as Cinema 4D describes it to other programs, which is an
///   approximation (exchange_material.h).
/// - A Cycles Native Material: its Blender data as it is (native_material.h).
/// A material none of them can read falls back to its average colour, with a warning.
//----------------------------------------------------------------------------------------
class MaterialExporter
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(MaterialExporter);

public:
	/// @param[in] bakeResolution  Side, in pixels, of the texture a shader is baked into.
	MaterialExporter(SceneWriter& writer, cinema::BaseDocument* doc, maxon::Int32 bakeResolution = 1024)
		: _writer(writer), _doc(doc), _colors(doc), _bakeResolution(bakeResolution) { }

	/// The id of the Blender material for @p material; for nullptr, of the default material
	/// Cinema 4D gives to objects without a texture tag.
	maxon::Result<maxon::String> GetMaterialId(cinema::BaseMaterial* material);

	//----------------------------------------------------------------------------------------
	/// The same for a stack of materials on one object, bottom first: each layer covers the
	/// ones below except where its alpha channel lets them through. One layer with a mapping
	/// other than plain UVW also comes here. An empty stack gives the default material.
	//----------------------------------------------------------------------------------------
	maxon::Result<maxon::String> GetMaterialId(const maxon::BaseArray<MaterialLayer>& layers);

	/// True when the file of a bitmap shader has an alpha channel. "Image Alpha" reads that
	/// channel if there is one, and the picture's brightness if not.
	maxon::Bool ImageHasAlpha(cinema::BaseShader* shader);

	/// True when @p material has an alpha channel that can let a material below show through.
	static maxon::Bool HasAlpha(cinema::BaseMaterial* material);

	//----------------------------------------------------------------------------------------
	/// The id of the `image` item for a bitmap shader, or an empty string when @p shader is
	/// not a bitmap shader with a file name. Empty as well for colours that Blender cannot load
	/// as Cinema 4D reads them (DocumentColors::ImageColorSpace gives no colour space): bake
	/// the shader instead (GetBakedImageId), which gives exactly what Cinema 4D calculates with.
	/// @param[in] colorData  True for colours (colour-managed), false for data such as bump heights.
	//----------------------------------------------------------------------------------------
	maxon::Result<maxon::String> GetImageId(cinema::BaseShader* shader, maxon::Bool colorData);

	//----------------------------------------------------------------------------------------
	/// The id of the `image` item holding @p shader rendered by Cinema 4D over the UV square,
	/// as float values in the render colour space. The id follows from what the picture depends
	/// on: the settings of the shader and of the shaders inside it, the time of a bitmap's file,
	/// the material's name, the document's colour settings, the bake resolution, and the frame
	/// only for a shader that changes over time by itself. A file baked for the same id before,
	/// in this session and not deleted since (baked_files.h), is used again: by final renders
	/// and the IPR alike (GetBakeCount() tells how often baking really happened). Empty when
	/// baking failed.
	//----------------------------------------------------------------------------------------
	maxon::Result<maxon::String> GetBakedImageId(cinema::BaseShader* shader);

	//----------------------------------------------------------------------------------------
	/// The id of the `image` item for an image file. Colours are loaded in the colour space the
	/// document's colour settings call for (DocumentColors::ImageColorSpace); where Blender has
	/// none for it, the file's values are used as they are, with a warning.
	/// @param[in] colorData      True for colours (colour-managed), false for data.
	/// @param[in] bitmapProfile  BITMAPSHADER_COLORPROFILE of the bitmap shader reading the file, or NOTOK.
	//----------------------------------------------------------------------------------------
	maxon::Result<maxon::String> GetFileImageId(const maxon::String& path, maxon::Bool colorData, maxon::Int32 bitmapProfile = NOTOK);

	//----------------------------------------------------------------------------------------
	/// The file a node's image port names, as a path Blender can open: a path relative to the
	/// document's folder (a Url of URLSCHEME_RELATIVE, as the node's "Make Relative" button
	/// writes it) is joined to that folder; a file of the Asset Browser is copied out of its
	/// repository into this session's folder (baked_files.h); every name is then looked for as
	/// Cinema 4D looks for a bitmap's (next to the document, in "tex", in the search paths).
	/// @param[out] problem  What went wrong, in words for the user, or empty. The path may still
	///                      be usable (it is sent as it is, and the bridge reports a missing file).
	/// @return              The path, or empty when there is none to send.
	//----------------------------------------------------------------------------------------
	maxon::Result<maxon::String> ResolveImageUrl(const maxon::Url& url, maxon::String& problem);
	/// The id of the `image` item for pixels in memory (RGBA, top row first, values as they
	/// are to be used). They are written to a file kept like a baked shader's (baked_files.h).
	maxon::Result<maxon::String> GetPixelImageId(const maxon::BaseArray<maxon::Vector4d32>& pixels, maxon::Int32 width, maxon::Int32 height);
	maxon::Int32 GetBakeResolution() const { return _bakeResolution; }
	/// Number of shaders baked by this process so far; for tests.
	static maxon::Int GetBakeCount();

	/// A warning for the console, through the writer.
	void Warn(const maxon::String& message);
	SceneWriter& GetWriter() { return _writer; }

	/// Said by a translation that reads the object keys (OBJECT_NAME_KEY, OBJECT_ID_KEY) while the
	/// material it belongs to is written; ReadsObjectKeys then answers true for that material's id,
	/// and the objects wearing it are sent with their keys.
	void UseObjectKeys() { ++_objectKeyUses; }
	maxon::Bool ReadsObjectKeys(const maxon::String& materialId) const { return (_known ? _known->keyed : _keyedMaterials).Find(materialId) != nullptr; }

	//----------------------------------------------------------------------------------------
	/// After the materials were asked for: hands over which id each got (MaterialIdTable).
	//----------------------------------------------------------------------------------------
	maxon::Result<void> TakeIds(MaterialIdTable& table);
	//----------------------------------------------------------------------------------------
	/// From now on nothing is written: GetMaterialId() answers from @p known, the ids of an earlier
	/// export of the same document, and for a material that export did not write it answers an empty id
	/// and HasUnknown() turns true (the caller then gives up what it was writing). @p known must
	/// outlive this.
	//----------------------------------------------------------------------------------------
	void SetKnownIds(const MaterialIdTable* known) { _known = known; }
	maxon::Bool HasUnknown() const { return _unknown; }

	/// The Blender that will render what is written, as BlenderVersionCode() (503 for 5.3) and as
	/// Blender writes it ("5.2.3 LTS"). A node, socket, setting or option of the Cycles node space
	/// that this Blender lacks is left out (cycles_node_material.cpp). 0, the default, leaves
	/// nothing out.
	void SetBlenderVersion(maxon::Int32 code, const maxon::String& text) { _blenderVersion = code; _blenderVersionText = text; }
	maxon::Int32 GetBlenderVersion() const { return _blenderVersion; }
	const maxon::String& GetBlenderVersionText() const { return _blenderVersionText; }
	cinema::BaseDocument* GetDocument() const { return _doc; }
	/// The document's colour settings: every colour parameter goes through Input() on its way out.
	const DocumentColors& GetColors() const { return _colors; }

private:
	maxon::Result<void> WriteLayers(const maxon::BaseArray<MaterialLayer>& layers, const maxon::String& id);
	maxon::Result<void> WritePlainMaterial(const maxon::String& id, const cinema::Vector& color, maxon::Float64 specular);

	SceneWriter& _writer;
	cinema::BaseDocument* _doc;
	DocumentColors _colors;
	maxon::Int32 _bakeResolution;
	maxon::Int32 _blenderVersion = 0;
	maxon::String _blenderVersionText;
	maxon::HashMap<maxon::String, maxon::String> _materials;   ///< what a material item was made from -> its id
	maxon::HashMap<maxon::String, maxon::String> _images;
	maxon::HashMap<maxon::String, maxon::Bool> _imageAlpha;      ///< image file -> has an alpha channel
	maxon::String _defaultId;
	maxon::Int _objectKeyUses = 0;
	maxon::HashMap<maxon::String, maxon::Bool> _keyedMaterials;   ///< ids of materials that read the object keys
	const MaterialIdTable* _known = nullptr;   ///< SetKnownIds()
	maxon::Bool _unknown = false;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_MATERIAL_EXPORT_H__
