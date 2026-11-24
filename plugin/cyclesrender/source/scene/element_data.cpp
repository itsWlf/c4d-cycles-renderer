// SPDX-License-Identifier: Apache-2.0
// Data per particle and per MoGraph clone that a material can read (element_data.h).

#include "c4d.h"
#include "c4d_baseeffectordata.h"
#include "lib_particlegroupobject.h"

#include "lib_description.h"
#include "obase.h"

#include "element_data.h"
#include "parameters.h"

namespace cyclesrender
{

const ElementAttribute ELEMENT_ATTRIBUTES[maxon::Int(ELEMENT::COUNT)] =
{
	{ "particle_color", "color", 4, true },
	{ "particle_alpha", "float", 1, true },
	{ "particle_id", "float", 1, true },
	{ "particle_age", "float", 1, true },
	{ "particle_lifetime", "float", 1, true },
	{ "particle_age_normalized", "float", 1, true },
	{ "particle_radius", "float", 1, true },
	{ "particle_distance", "float", 1, true },
	{ "particle_velocity", "vector", 3, true },
	{ "mograph_index", "float", 1, false },
	{ "mograph_index_ratio", "float", 1, false },
	{ "mograph_index_normalized", "float", 1, false },
	{ "mograph_color_raw", "color", 4, false },
	{ "mograph_weight", "float", 1, false },
	{ "mograph_uvw", "vector", 3, false },
	{ "mograph_time_offset", "float", 1, false },
};

ELEMENT FindElementAttribute(const maxon::String& name)
{
	for (maxon::Int i = 0; i < maxon::Int(ELEMENT::COUNT); ++i)
	{
		if (name == maxon::String(ELEMENT_ATTRIBUTES[i].name))
			return ELEMENT(i);
	}
	return ELEMENT::COUNT;
}

const ElementReader::Source* ElementReader::Find(const cinema::BaseObject* object)
{
	if (!object)
		return nullptr;
	if (_last != NOTOK && _sources[_last].object == object)
		return &_sources[_last];
	for (maxon::Int i = 0; i < _sources.GetCount(); ++i)
	{
		if (_sources[i].object == object)
		{
			_last = i;
			return &_sources[i];
		}
	}

	Source source;
	source.object = object;
	if (object->GetType() == Ofpgroup)
	{
		const cinema::ParticleGroupObject* const group = static_cast<const cinema::ParticleGroupObject*>(object);
		source.particles = true;
		source.count = group->GetParticlePositionsR().GetCount();
		// An array counts only when it has one entry per particle.
		source.colors = group->GetParticleColorsR();
		source.ids = group->GetParticleUniqueIdsR();
		source.ages = group->GetParticleAgesR();
		source.lifetimes = group->GetParticleLifetimesR();
		source.radii = group->GetParticleRadiiR();
		source.distances = group->GetParticleDistancesTraversedR();
		source.velocities = group->GetParticleVelocitiesR();
	}
	else
	{
		// (Message() is not const; reading MoData changes nothing of the object.)
		cinema::BaseTag* const tag = const_cast<cinema::BaseObject*>(object)->GetTag(ID_MOTAGDATA);
		cinema::GetMoDataMessage message;
		if (tag && tag->Message(MSG_GET_MODATA, &message) && message.modata)
		{
			source.modata = message.modata;
			source.count = source.modata->GetCount();
			source.weights = source.modata->GetRealArray(cinema::MODATA_WEIGHT, 0.0);
			source.uvws = source.modata->GetVectorArray(cinema::MODATA_UVW);
			source.times = source.modata->GetRealArray(cinema::MODATA_TIME, 0.0);
			source.rawColors = source.modata->GetVectorArray(cinema::MODATA_COLOR);
		}
	}
	iferr (_sources.Append(std::move(source)))
		return nullptr;
	_last = _sources.GetCount() - 1;
	return &_sources[_last];
}

maxon::Int ElementReader::GetCount(const cinema::BaseObject* source)
{
	const Source* const found = Find(source);
	return found ? found->count : 0;
}

maxon::Bool ElementReader::Read(const ElementRef& element, ELEMENT which, maxon::Float32* out)
{
	const ElementAttribute& attribute = ELEMENT_ATTRIBUTES[maxon::Int(which)];
	for (maxon::Int i = 0; i < attribute.size; ++i)
		out[i] = 0.0f;
	const Source* const source = Find(element.source);
	const maxon::Int index = element.index;
	if (!source || source->particles != attribute.particle || index < 0 || index >= source->count)
		return false;

	switch (which)
	{
		case ELEMENT::PARTICLE_COLOR:
			if (source->colors.GetCount() != source->count)
				return false;
			out[0] = source->colors[index].r;
			out[1] = source->colors[index].g;
			out[2] = source->colors[index].b;
			out[3] = source->colors[index].a;
			return true;
		case ELEMENT::PARTICLE_ALPHA:
			if (source->colors.GetCount() != source->count)
				return false;
			out[0] = source->colors[index].a;
			return true;
		case ELEMENT::PARTICLE_ID:
			if (source->ids.GetCount() != source->count)
				return false;
			// (A 32-bit float holds whole numbers up to 2^24 exactly; ids count up from 0.)
			out[0] = maxon::Float32(source->ids[index]);
			return true;
		case ELEMENT::PARTICLE_AGE:
			if (source->ages.GetCount() != source->count)
				return false;
			out[0] = source->ages[index];
			return true;
		case ELEMENT::PARTICLE_LIFETIME:
			if (source->lifetimes.GetCount() != source->count)
				return false;
			out[0] = source->lifetimes[index];
			return true;
		case ELEMENT::PARTICLE_AGE_NORMALIZED:
			if (source->ages.GetCount() != source->count || source->lifetimes.GetCount() != source->count)
				return false;
			out[0] = source->lifetimes[index] > 0.0f ? source->ages[index] / source->lifetimes[index] : 0.0f;
			return true;
		case ELEMENT::PARTICLE_RADIUS:
			if (source->radii.GetCount() != source->count)
				return false;
			out[0] = maxon::Float32(_coords.Length(maxon::Float64(source->radii[index])));
			return true;
		case ELEMENT::PARTICLE_DISTANCE:
			if (source->distances.GetCount() != source->count)
				return false;
			out[0] = maxon::Float32(_coords.Length(maxon::Float64(source->distances[index])));
			return true;
		case ELEMENT::PARTICLE_VELOCITY:
		{
			if (source->velocities.GetCount() != source->count)
				return false;
			const cinema::Vector32& velocity = source->velocities[index];
			_coords.Point(cinema::Vector(velocity.x, velocity.y, velocity.z), out);
			return true;
		}
		case ELEMENT::MOGRAPH_INDEX:
			out[0] = maxon::Float32(index);
			return true;
		case ELEMENT::MOGRAPH_INDEX_RATIO:
			out[0] = source->count > 1 ? maxon::Float32(maxon::Float64(index) / maxon::Float64(source->count - 1)) : 0.0f;
			return true;
		case ELEMENT::MOGRAPH_INDEX_NORMALIZED:
			out[0] = maxon::Float32(maxon::Float64(index) / maxon::Float64(source->count));
			return true;
		case ELEMENT::MOGRAPH_COLOR_RAW:
			out[0] = maxon::Float32(source->rawColors[index].x);
			out[1] = maxon::Float32(source->rawColors[index].y);
			out[2] = maxon::Float32(source->rawColors[index].z);
			out[3] = 1.0f;
			return true;
		case ELEMENT::MOGRAPH_WEIGHT:
			out[0] = maxon::Float32(source->weights[index]);
			return true;
		case ELEMENT::MOGRAPH_UVW:
			out[0] = maxon::Float32(source->uvws[index].x);
			out[1] = maxon::Float32(source->uvws[index].y);
			out[2] = maxon::Float32(source->uvws[index].z);
			return true;
		case ELEMENT::MOGRAPH_TIME_OFFSET:
			out[0] = maxon::Float32(source->times[index]);
			return true;
		case ELEMENT::COUNT:
			break;
	}
	return false;
}

// The user data of one object by name; false when it has none of that name that is a number, a vector or a colour.
static maxon::Bool OwnUserData(const cinema::BaseObject* op, const maxon::String& name, maxon::Float64* values, maxon::Int& count, maxon::Bool& whole)
{
	const cinema::DynamicDescription* const description = op->GetDynamicDescription();
	if (!description)
		return false;
	const void* handle = description->BrowseInit();
	if (!handle)
		return false;
	maxon::Bool found = false;
	cinema::DescID id;
	const cinema::BaseContainer* settings = nullptr;
	while (!found && description->BrowseGetNext(handle, &id, &settings))
	{
		if (!settings || settings->GetString(cinema::DESC_NAME) != name)
			continue;
		cinema::GeData data;
		if (!op->GetParameter(id, data, cinema::DESCFLAGS_GET::NONE))
			continue;
		switch (data.GetType())
		{
			case cinema::DA_REAL:
				values[0] = data.GetFloat();
				count = 1;
				whole = false;
				found = true;
				break;
			case cinema::DA_LONG:
			case cinema::DA_LLONG:
				// (GetInt64() of a 32-bit value answered 0, measured: each is read as what it is.)
				values[0] = data.GetType() == cinema::DA_LONG ? maxon::Float64(data.GetInt32()) : maxon::Float64(data.GetInt64());
				count = 1;
				whole = true;
				found = true;
				break;
			case cinema::DA_VECTOR:
			{
				const cinema::Vector vector = data.GetVector();
				values[0] = vector.x;
				values[1] = vector.y;
				values[2] = vector.z;
				count = 3;
				whole = false;
				found = true;
				break;
			}
			default:
				break;   // a text, a link, a file: nothing a material can read
		}
	}
	description->BrowseFree(handle);
	return found;
}

maxon::Bool FindUserData(const cinema::BaseObject* op, const maxon::String& name, maxon::Float64 defaultGrey, maxon::Float64* values, maxon::Int& count,
	maxon::Bool& whole)
{
	if (name == maxon::String(OBJECT_COLOR_ATTRIBUTE))
	{
		if (!op)
			return false;
		// What Redshift's RSObjectColor gives (measured): the display colour of an object set to use it always,
		// else the grey Cinema 4D renders an object without a material in (0.604 in 2027, 0.8 in 2024: @p defaultGrey;
		// the object's colour parameter is another value then). "Automatic" and "Layer" were not measured and count as off.
		const maxon::Bool always = GetIntParameter(op, ID_BASEOBJECT_USECOLOR, ID_BASEOBJECT_USECOLOR_OFF) == ID_BASEOBJECT_USECOLOR_ALWAYS;
		const cinema::Vector color = always ? GetParameterData(op, ID_BASEOBJECT_COLOR).GetVector() : cinema::Vector(defaultGrey);
		values[0] = color.x;
		values[1] = color.y;
		values[2] = color.z;
		count = 3;
		whole = false;
		return true;
	}
	// Upwards: the parents, and from the top of a generator's cache the generator.
	maxon::Int guard = 0;
	while (op && guard++ < 4096)
	{
		if (OwnUserData(op, name, values, count, whole))
			return true;
		const cinema::BaseObject* const up = op->GetUp();
		op = up ? up : op->GetCacheParent();
	}
	return false;
}

maxon::Bool HasVertexData(const cinema::BaseObject* op, const maxon::String& name)
{
	if (!op)
		return false;
	for (const cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
	{
		if ((tag->GetType() == Tvertexmap || tag->GetType() == Tvertexcolor) && tag->GetName() == name)
			return true;
	}
	return false;
}

} // namespace cyclesrender
