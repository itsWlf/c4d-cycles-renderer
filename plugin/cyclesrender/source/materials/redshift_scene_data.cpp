// SPDX-License-Identifier: Apache-2.0
// The Redshift nodes that read data of the scene by a name (redshift_nodes.h, SceneDataNodes): the
// Color, Scalar, Integer and Vector User Data nodes, the Vertex Attribute node and the Point
// Attribute node. Each becomes Attribute nodes of Blender that read what the exporter sends under a
// plain name of its own (scene/element_data.h; protocol/protocol.md, "Attributes a material
// reads"); telling the exporter (MaterialExporter::UseAttribute) is what makes it send the data, and
// only with what wears this material. What each Redshift name gives was measured on Redshift's
// renders (docs/reference/c4d-sdk.md, "Redshift nodes that read the scene").

#include "c4d.h"
#include "maxon/graph_basics.h"
#include "maxon/nodesgraph.h"

#include "../scene/coords.h"
#include "../scene/element_data.h"
#include "../sdk_compat.h"
#include "material_export.h"
#include "redshift_nodes.h"

namespace cyclesrender
{

// How a value sent in Blender's terms becomes the one Redshift gives.
enum class CONVERT
{
	NONE,
	LENGTH,        // metres to document units
	VELOCITY,      // Blender's axes and metres per second to Cinema 4D's axes and document units per second
	ONE_MINUS      // 1 - value
} MAXON_ENUM_LIST(CONVERT);

// A name Redshift's user data nodes know (measured), and the attribute that holds it here.
struct RedshiftAttribute
{
	const maxon::Char* redshift;
	const maxon::Char* ours;
	maxon::Int size;            // 1 a number, 3 a vector or colour, 4 a colour whose alpha is `alpha`
	maxon::Bool vector;         // stored as a vector (the Attribute node's Vector output), not a colour
	maxon::Bool perElement;     // per particle or clone: on a point cloud's points or on the copies of an instancer
	CONVERT convert;
	const maxon::Char* alpha;   // size 4: the number that holds the alpha
	maxon::Int component;       // size 1 read from a vector: which of its three numbers; NOTOK otherwise
};

static const RedshiftAttribute REDSHIFT_ATTRIBUTES[] =
{
	// Particles (measured on a particle group rendered as spheres and as custom objects).
	{ "RSPAge", "particle_age", 1, false, true, CONVERT::NONE, nullptr, NOTOK },
	{ "RSPAgeNormalized", "particle_age_normalized", 1, false, true, CONVERT::NONE, nullptr, NOTOK },
	{ "RSPColor", "particle_color", 3, false, true, CONVERT::NONE, nullptr, NOTOK },
	{ "RSPColorAlpha", "particle_color", 4, false, true, CONVERT::NONE, "particle_alpha", NOTOK },
	{ "RSPDistanceTravelled", "particle_distance", 1, false, true, CONVERT::LENGTH, nullptr, NOTOK },
	{ "RSPID", "particle_id", 1, false, true, CONVERT::NONE, nullptr, NOTOK },
	{ "RSPLife", "particle_lifetime", 1, false, true, CONVERT::NONE, nullptr, NOTOK },
	{ "RSPSize", "particle_radius", 1, false, true, CONVERT::LENGTH, nullptr, NOTOK },
	{ "RSPTransparency", "particle_alpha", 1, false, true, CONVERT::ONE_MINUS, nullptr, NOTOK },
	{ "RSPVelocity", "particle_velocity", 3, true, true, CONVERT::VELOCITY, nullptr, NOTOK },
	// MoGraph (measured on a Cloner's clones in its three instance modes): the colour as it is stored, the
	// index, index / count, 1 - UVW.x (what Redshift calls the index ratio: 1 for the first clone of a
	// linear Cloner, 0 for the last), the weight, the UVW and the time offset.
	{ "RSMGColor", "mograph_color_raw", 3, false, true, CONVERT::NONE, nullptr, NOTOK },
	{ "RSMGID", "mograph_index", 1, false, true, CONVERT::NONE, nullptr, NOTOK },
	{ "RSMGIDNorm", "mograph_index_normalized", 1, false, true, CONVERT::NONE, nullptr, NOTOK },
	{ "RSMGIDRatio", "mograph_uvw", 1, true, true, CONVERT::ONE_MINUS, nullptr, 0 },
	{ "RSMGIDRatioColor", "mograph_uvw", 1, true, true, CONVERT::ONE_MINUS, nullptr, 0 },
	{ "RSMGWeight", "mograph_weight", 1, false, true, CONVERT::NONE, nullptr, NOTOK },
	{ "RSMGUVW", "mograph_uvw", 3, true, true, CONVERT::NONE, nullptr, NOTOK },
	{ "RSMGTimeOffset", "mograph_time_offset", 1, false, true, CONVERT::NONE, nullptr, NOTOK },
	// The object: the Object ID of its Redshift Object tag, and its display colour (RSDisplayColor gives the same:
	// measured on an object set to use its display colour always, 0.3.0).
	{ "RSObjectID", OBJECT_ID_KEY, 1, false, false, CONVERT::NONE, nullptr, NOTOK },
	{ "RSObjectColor", OBJECT_COLOR_ATTRIBUTE, 3, false, false, CONVERT::NONE, nullptr, NOTOK },
	{ "RSDisplayColor", OBJECT_COLOR_ATTRIBUTE, 3, false, false, CONVERT::NONE, nullptr, NOTOK },
};

// The text a string port holds.
static maxon::String PortText(const maxon::GraphNode& port)
{
	if (!port.IsValid())
		return maxon::String();
	ifnoerr (const maxon::Opt<const maxon::String&> text = port.GetEffectivePortValue<maxon::String>())
	{
		if (text)
			return *text;
	}
	return maxon::String();
}

//----------------------------------------------------------------------------------------
// Measured in Blender 5.0 to 5.3, Cycles and Eevee (docs/reference/blender.md, "Attributes a
// material reads"): an Attribute node reads 0 from its Color, Vector and Factor outputs where the
// named attribute is missing, of the geometry and of the instancer alike. So the two readings added
// up are right on a point cloud (the points' own attribute) and on instances (the copies' attribute,
// or failing that the object's property) with one material. The Alpha output cannot be added (it is
// 1 for a missing attribute of the geometry): an alpha is sent as a number of its own.
//----------------------------------------------------------------------------------------
maxon::Result<RedshiftNodes::Term> RedshiftNodes::AttributeTerm(const maxon::String& name, const maxon::Char* socket, maxon::Bool both)
{
	iferr_scope;
	const maxon::Int instancer = _graph.Add("ShaderNodeAttribute") iferr_return;
	_graph.SetProperty(instancer, "attribute_type", "INSTANCER"_s);
	_graph.SetProperty(instancer, "attribute_name", name);
	const Term ofInstancer = Term::Of(NodeOutput { instancer, maxon::String(socket) });
	if (!both)
		return ofInstancer;
	const maxon::Int geometry = _graph.Add("ShaderNodeAttribute") iferr_return;
	_graph.SetProperty(geometry, "attribute_type", "GEOMETRY"_s);
	_graph.SetProperty(geometry, "attribute_name", name);
	const Term ofGeometry = Term::Of(NodeOutput { geometry, maxon::String(socket) });
	if (maxon::CString(socket) == "Fac"_cs)
		return Math("ADD", ofGeometry, ofInstancer);
	return VMath("ADD", ofGeometry, ofInstancer);
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::Present(const Term& has, const Term& read, const Term& fallback, maxon::Bool scalar)
{
	iferr_scope;
	if (scalar)
	{
		// fallback + has * (read - fallback)
		const Term difference = Math("SUBTRACT", read, fallback) iferr_return;
		return Math("MULTIPLY_ADD", has, difference, fallback);
	}
	return MixColor("MIX", has, fallback, read);
}

maxon::Result<RedshiftNodes::SceneValue> RedshiftNodes::ReadUserData(const maxon::GraphNode& rsNode, const maxon::Char* what)
{
	iferr_scope;
	SceneValue result;
	const maxon::GraphNode port = Port(rsNode, "attribute") iferr_return;
	const maxon::String name = PortText(port);
	const maxon::String node = ShortName(rsNode) iferr_return;
	if (name.IsEmpty())
	{
		Warn(FormatString("a user data node (\"@\") plugged into @ names no attribute; its default is used.", node, maxon::String(what)));
		return result;
	}

	const RedshiftAttribute* known = nullptr;
	for (const RedshiftAttribute& entry : REDSHIFT_ATTRIBUTES)
	{
		if (name == maxon::String(entry.redshift))
			known = &entry;
	}
	if (!known)
	{
		// Redshift's own names that are not sent: named, the default used. Any other name is the user's.
		if (name.StartsWith("RSP"_s) || name.StartsWith("RSMG"_s) || name.StartsWith("RSGeom"_s) || name.StartsWith("RSObject"_s) || name.StartsWith("RSHair"_s))
		{
			Warn(FormatString("the user data node (\"@\") plugged into @ reads \"@\", which is not translated; its default is used.", node, maxon::String(what), name));
			return result;
		}
		// An object's own user data of that name: an object property (measured: Redshift's user data nodes read
		// user data of the object or of an object above it, and no vertex map or vertex colour).
		_exporter.UseAttribute(name, true);
		result.found = true;
		result.size = 3;
		result.value = AttributeTerm(name, "Color", false) iferr_return;
		result.number = AttributeTerm(name, "Fac", false) iferr_return;
		result.alpha = Term::Number(1.0);
		result.has = AttributeTerm(PresenceAttribute(name), "Fac", false) iferr_return;
		return result;
	}

	const maxon::String ours(known->ours);
	_exporter.UseAttribute(ours, true);
	result.found = true;
	result.size = known->size == 1 ? 1 : 3;
	result.has = AttributeTerm(PresenceAttribute(ours), "Fac", known->perElement) iferr_return;
	if (known->perElement)
	{
		// (Read from the points and from the instancer and added up, as the value: 1 at most.)
		result.has = Math("MINIMUM", result.has, Term::Number(1.0)) iferr_return;
	}
	const maxon::Float64 perMetre = 1.0 / Coords::FromDocument(_exporter.GetDocument()).GetScale();
	if (known->size == 1)
	{
		Term number;
		if (known->component != NOTOK)
		{
			const Term whole = AttributeTerm(ours, "Vector", known->perElement) iferr_return;
			number = Component(whole, known->component) iferr_return;
		}
		else
		{
			number = AttributeTerm(ours, "Fac", known->perElement) iferr_return;
		}
		if (known->convert == CONVERT::LENGTH)
		{
			number = Math("MULTIPLY", number, Term::Number(perMetre)) iferr_return;
		}
		else if (known->convert == CONVERT::ONE_MINUS)
		{
			number = Math("SUBTRACT", Term::Number(1.0), number) iferr_return;
		}
		result.value = number;
		result.number = number;
		result.alpha = number;      // (measured: a Color User Data node's alpha of a number is the number)
		return result;
	}
	result.value = AttributeTerm(ours, known->vector ? "Vector" : "Color", known->perElement) iferr_return;
	if (known->convert == CONVERT::VELOCITY)
	{
		result.value = ToCinema(result.value, true, 0) iferr_return;
	}
	// As a number: the mean of the three (measured on a colour and on the velocity).
	result.number = VMath("DOT_PRODUCT", result.value, Term::Of(cinema::Vector(1.0 / 3.0))) iferr_return;
	result.alpha = Term::Number(1.0);
	if (known->alpha)
	{
		_exporter.UseAttribute(maxon::String(known->alpha), true);
		result.alpha = AttributeTerm(maxon::String(known->alpha), "Fac", known->perElement) iferr_return;
	}
	return result;
}

maxon::Result<NodeOutput> RedshiftNodes::SceneDataNodes(const Source& source, const maxon::String& name, const maxon::Char* what, maxon::Bool& handled)
{
	iferr_scope;
	const maxon::GraphNode& rs = source.node;
	handled = true;

	// The user data nodes: a colour, a number, a whole number, a vector.
	const maxon::Bool color = name == "rsuserdatacolor"_s;
	const maxon::Bool scalar = name == "rsuserdatascalar"_s;
	const maxon::Bool integer = name == "rsuserdatainteger"_s;
	const maxon::Bool vector = name == "rsuserdatavector"_s;
	if (color || scalar || integer || vector)
	{
		const SceneValue read = ReadUserData(rs, what) iferr_return;
		// The node's own default (a colour's is rendered times its alpha by the Redshift of 2027, measured).
		Term fallback = Input(rs, "default", false, what, 0) iferr_return;
		if (color && REDSHIFT_STRAIGHT_COLORS && fallback.IsConstant())
			fallback.value = fallback.value * fallback.alpha;
		if (!read.found)
			return Output(fallback, scalar || integer);
		// Where the object has no such data, Redshift gives the node's default too (measured, 0.3.0: user data, RSP and
		// RSMG names on an object that has none); Blender's Attribute node would read 0 there (before 0.3.0 it did).
		if (integer)
		{
			// (Measured: a whole-number node reading a number gives the nearest whole number.)
			const Term rounded = Math("ROUND", read.number) iferr_return;
			const Term chosen = Present(read.has, rounded, fallback, true) iferr_return;
			return Output(chosen, true);
		}
		if (scalar)
		{
			const Term chosen = Present(read.has, read.number, fallback, true) iferr_return;
			return Output(chosen, true);
		}
		// (A number read by a Color or Vector node is its grey: the mix takes it as one.)
		const Term chosen = Present(read.has, read.value, fallback, false) iferr_return;
		return Output(chosen, false);
	}

	// Vertex Attribute and Point Attribute: a vertex map or vertex colour tag of the object, by the tag's
	// name, as the mesh exporter names them. Measured: both read the same data; where the object has
	// no such tag the Point Attribute gives 0, as Blender does, and the Vertex Attribute magenta.
	if (name == "vertexattributelookup"_s || name == "particleattributelookup"_s)
	{
		const maxon::GraphNode port = Port(rs, "attribute") iferr_return;
		const maxon::String attribute = PortText(port);
		if (attribute.IsEmpty())
		{
			Warn(FormatString("a @ node plugged into @ names no attribute; the value set on the input is used.",
				name == "vertexattributelookup"_s ? "Vertex Attribute"_s : "Point Attribute"_s, maxon::String(what)));
			return NodeOutput();
		}
		const maxon::Int node = _graph.Add("ShaderNodeAttribute") iferr_return;
		_graph.SetProperty(node, "attribute_type", "GEOMETRY"_s);
		_graph.SetProperty(node, "attribute_name", attribute);
		return NodeOutput { node, source.output == "outscalar"_s ? "Fac"_s : "Color"_s };
	}

	handled = false;
	return NodeOutput();
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::AlphaOf(const Source& source, const maxon::Char* what, maxon::Bool& known)
{
	iferr_scope;
	known = true;
	const maxon::String name = ShortName(source.node) iferr_return;
	if (name == "rsuserdatacolor"_s)
	{
		const SceneValue read = ReadUserData(source.node, what) iferr_return;
		const Term fallback = Input(source.node, "default", false, what, 0) iferr_return;
		if (read.found)
			return Present(read.has, read.alpha, Term::Number(fallback.alpha), true);
		return Term::Number(fallback.alpha);
	}
	if (name == "vertexattributelookup"_s || name == "particleattributelookup"_s)
	{
		// The vertex colour's own alpha (1 for a vertex map, and where the object has no such tag).
		const maxon::GraphNode port = Port(source.node, "attribute") iferr_return;
		const maxon::String attribute = PortText(port);
		if (attribute.IsPopulated())
		{
			const maxon::Int node = _graph.Add("ShaderNodeAttribute") iferr_return;
			_graph.SetProperty(node, "attribute_type", "GEOMETRY"_s);
			_graph.SetProperty(node, "attribute_name", attribute);
			return Term::Of(NodeOutput { node, "Alpha"_s });
		}
		return Term::Number(1.0);
	}
	known = false;
	return Term::Number(1.0);
}

} // namespace cyclesrender
