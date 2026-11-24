// SPDX-License-Identifier: Apache-2.0
// Data per particle and per MoGraph clone that a material can read: the plain names it goes by on the
// wire (protocol/protocol.md, "Attributes a material reads"), and a reader that gives the numbers of
// one particle or clone. A material that reads one of them says so while it is translated
// (MaterialExporter::UseAttribute); the exporter then sends it with the point clouds, instancers and
// objects that wear the material, and with nothing else: a scene whose materials read nothing pays
// nothing.

#ifndef CYCLESRENDER_ELEMENT_DATA_H__
#define CYCLESRENDER_ELEMENT_DATA_H__

#include "c4d.h"
#include "c4d_baseeffectordata.h"
#include "lib_particlegroupobject.h"
#include "maxon/hashmap.h"

#include "coords.h"

namespace cyclesrender
{

/// What is known per particle or per clone. The order is the order of ELEMENT_ATTRIBUTES.
enum class ELEMENT
{
	PARTICLE_COLOR,           ///< r, g, b, a as the particle has them
	PARTICLE_ALPHA,           ///< the colour's alpha as a number of its own (an Attribute node's Alpha output cannot be added up)
	PARTICLE_ID,              ///< the particle's unique id
	PARTICLE_AGE,             ///< seconds
	PARTICLE_LIFETIME,        ///< seconds
	PARTICLE_AGE_NORMALIZED,  ///< age / lifetime
	PARTICLE_RADIUS,          ///< metres
	PARTICLE_DISTANCE,        ///< the way it has gone, metres
	PARTICLE_VELOCITY,        ///< Blender's axes, metres per second
	MOGRAPH_INDEX,            ///< the clone's index in its generator
	MOGRAPH_INDEX_RATIO,      ///< index / (count - 1): 0 for the first clone, 1 for the last
	MOGRAPH_INDEX_NORMALIZED, ///< index / count
	MOGRAPH_COLOR_RAW,        ///< MoGraph's colour as it is stored, r, g, b and an alpha of 1 (the instancer's `colors`, read as
	                          ///< `mograph_color`, is that colour as Cinema 4D's Color shader shows it: held to 0..1 and
	                          ///< converted from sRGB to the render space)
	MOGRAPH_WEIGHT,           ///< MoGraph's weight
	MOGRAPH_UVW,              ///< MoGraph's UVW, three numbers
	MOGRAPH_TIME_OFFSET,      ///< MoGraph's time offset
	COUNT
} MAXON_ENUM_LIST(ELEMENT);

/// One kind of element data: its name on the wire, the `type` of the protocol's `attributes`
/// ("float", "vector", "color") and how many numbers an element has (1, 3, 4).
struct ElementAttribute
{
	const maxon::Char* name;
	const maxon::Char* type;
	maxon::Int size;
	maxon::Bool particle;      ///< particles have it (else MoGraph clones)
};

/// Indexed by ELEMENT.
extern const ElementAttribute ELEMENT_ATTRIBUTES[maxon::Int(ELEMENT::COUNT)];

/// The kind of element data that goes by @p name on the wire, or ELEMENT::COUNT for any other name.
ELEMENT FindElementAttribute(const maxon::String& name);

/// One particle of a particle group, or one clone of a MoGraph generator (the index of its MoData).
struct ElementRef
{
	const cinema::BaseObject* source = nullptr;    ///< an Ofpgroup, or an object with a MoGraph data tag; nullptr: nothing known
	maxon::Int32 index = 0;
};

//----------------------------------------------------------------------------------------
/// Reads element data. It keeps what it looked up for a source (the particle group's arrays, the
/// generator's MoData), so make one per export and ask it element by element.
//----------------------------------------------------------------------------------------
class ElementReader
{
public:
	explicit ElementReader(const Coords& coords) : _coords(coords) { }

	/// The numbers of @p which for @p element into @p out (ELEMENT_ATTRIBUTES[which].size of them).
	/// Zeros, and false, when the element's source does not have this kind of data (a clone asked
	/// for a particle's age), has no such element, or is nullptr.
	maxon::Bool Read(const ElementRef& element, ELEMENT which, maxon::Float32* out);

	/// Number of elements of @p source (particles, or MoData entries); 0 when it has none.
	maxon::Int GetCount(const cinema::BaseObject* source);

private:
	struct Source
	{
		const cinema::BaseObject* object = nullptr;
		maxon::Bool particles = false;
		maxon::Int count = 0;
		// A particle group's arrays (empty where the group has none).
		maxon::Block<const maxon::ColorA32> colors;
		maxon::Block<const maxon::UInt32> ids;
		maxon::Block<const maxon::Float32> ages;
		maxon::Block<const maxon::Float32> lifetimes;
		maxon::Block<const maxon::Float32> radii;
		maxon::Block<const maxon::Float32> distances;
		maxon::StridedBlock<const cinema::Vector32> velocities;
		// A MoGraph generator's arrays.
		cinema::MoData* modata = nullptr;
		cinema::MDArray<maxon::Float> weights;
		cinema::MDArray<cinema::Vector> uvws;
		cinema::MDArray<maxon::Float> times;
		cinema::MDArray<cinema::Vector> rawColors;
	};

	/// The source for @p object, looked up once.
	const Source* Find(const cinema::BaseObject* object);

	Coords _coords;
	maxon::BaseArray<Source> _sources;
	maxon::Int _last = NOTOK;        ///< the source asked for last: elements of one source follow each other
};

/// The object property a material reads an object's display colour by (Redshift's RSObjectColor): three
/// numbers, sent like user data for the objects whose material reads it.
static const maxon::Char* const OBJECT_COLOR_ATTRIBUTE = "object_color";

//----------------------------------------------------------------------------------------
/// An object's own user data (Attribute Manager, "User Data") by its name, as numbers: a float, a
/// whole number or a switch is one number, a vector or a colour three (as they are typed in: no
/// colour conversion, measured against Redshift). The object counts first, then the objects above
/// it, and for what a generator made the generator and what is above that: Redshift hands user data
/// down the same way (measured; docs/reference/c4d-sdk.md, "Redshift nodes that read the scene").
/// OBJECT_COLOR_ATTRIBUTE answers with the object's own display colour when the object is set to use
/// it always, else with @p defaultGrey three times.
/// @param[in] defaultGrey  The grey of an object without a material in this document (MaterialExporter's default material).
/// @param[out] values  One or three numbers.
/// @param[out] count   How many.
/// @param[out] whole   The value is a whole number.
/// @return             False when no object on the way has user data of that name and type.
//----------------------------------------------------------------------------------------
maxon::Bool FindUserData(const cinema::BaseObject* op, const maxon::String& name, maxon::Float64 defaultGrey, maxon::Float64* values, maxon::Int& count,
	maxon::Bool& whole);

/// True when @p op has a vertex map or vertex colour tag of that name: a mesh's attribute, which a
/// material reads from the geometry.
maxon::Bool HasVertexData(const cinema::BaseObject* op, const maxon::String& name);

} // namespace cyclesrender

#endif // CYCLESRENDER_ELEMENT_DATA_H__
