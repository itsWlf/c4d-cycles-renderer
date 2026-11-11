// SPDX-License-Identifier: Apache-2.0
// Reading parameters of objects, tags, materials and shaders.
//
// Use these instead of reading the node's BaseContainer: several parameters are not stored
// there under their id (a light's type, a material's channel switches), and reading the
// container then silently returns 0. GetParameter() always answers what the Attribute Manager
// shows.

#ifndef CYCLESRENDER_PARAMETERS_H__
#define CYCLESRENDER_PARAMETERS_H__

#include "c4d.h"

namespace cyclesrender
{

inline cinema::GeData GetParameterData(const cinema::BaseList2D* node, maxon::Int32 id)
{
	cinema::GeData data;
	if (node)
		node->GetParameter(CreateDescID(cinema::DescLevel(id)), data, cinema::DESCFLAGS_GET::NONE);
	return data;
}

inline maxon::Float64 GetFloatParameter(const cinema::BaseList2D* node, maxon::Int32 id, maxon::Float64 fallback)
{
	const cinema::GeData data = GetParameterData(node, id);
	return data.GetType() == cinema::DA_REAL ? data.GetFloat() : fallback;
}

inline maxon::Int32 GetIntParameter(const cinema::BaseList2D* node, maxon::Int32 id, maxon::Int32 fallback)
{
	const cinema::GeData data = GetParameterData(node, id);
	return data.GetType() == cinema::DA_LONG ? data.GetInt32() : fallback;
}

/// Booleans are stored as integers.
inline maxon::Bool GetBoolParameter(const cinema::BaseList2D* node, maxon::Int32 id, maxon::Bool fallback)
{
	const cinema::GeData data = GetParameterData(node, id);
	return data.GetType() == cinema::DA_LONG ? data.GetInt32() != 0 : fallback;
}

inline cinema::Vector GetVectorParameter(const cinema::BaseList2D* node, maxon::Int32 id, const cinema::Vector& fallback)
{
	const cinema::GeData data = GetParameterData(node, id);
	return data.GetType() == cinema::DA_VECTOR ? data.GetVector() : fallback;
}

inline maxon::String GetStringParameter(const cinema::BaseList2D* node, maxon::Int32 id)
{
	const cinema::GeData data = GetParameterData(node, id);
	return data.GetType() == cinema::DA_STRING ? maxon::String(data.GetString()) : maxon::String();
}

inline cinema::Filename GetFilenameParameter(const cinema::BaseList2D* node, maxon::Int32 id)
{
	const cinema::GeData data = GetParameterData(node, id);
	return data.GetType() == cinema::DA_FILENAME ? data.GetFilename() : cinema::Filename();
}

/// The shader linked in a parameter, or nullptr.
inline cinema::BaseShader* GetShaderParameter(const cinema::BaseList2D* node, maxon::Int32 id, const cinema::BaseDocument* doc)
{
	const cinema::GeData data = GetParameterData(node, id);
	// The link is handed out const because the document is; the exporters only read the shader,
	// but the SDK's shader calls they need are not const.
	cinema::BaseList2D* const link = const_cast<cinema::BaseList2D*>(data.GetLink(doc));
	return link && link->IsInstanceOf(Xbase) ? static_cast<cinema::BaseShader*>(link) : nullptr;
}

} // namespace cyclesrender

#endif // CYCLESRENDER_PARAMETERS_H__
