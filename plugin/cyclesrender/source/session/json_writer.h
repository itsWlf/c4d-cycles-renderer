// SPDX-License-Identifier: Apache-2.0
// Builds the JSON header of a request piece by piece.
//
// Scene headers hold nested lists (items, matrices), which are simpler and cheaper to write
// directly than to assemble as SDK dictionaries. Replies are still parsed with the SDK's parser.

#ifndef CYCLESRENDER_JSON_WRITER_H__
#define CYCLESRENDER_JSON_WRITER_H__

#include "c4d.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// Appends JSON text in document order. The caller is responsible for balanced Begin/End
/// calls; commas and string escaping are handled here.
///
///   JsonWriter w;
///   w.BeginObject();
///   w.String("op", "scene"_s);
///   w.BeginArray("items");
///   w.BeginObject(); w.String("kind", "mesh"_s); w.Int("points", 0); w.EndObject();
///   w.EndArray();
///   // left open: BridgeClient::Request adds the request id and closes the object.
//----------------------------------------------------------------------------------------
class JsonWriter
{
public:
	/// Opens an object or array. Pass a key when it is a member of an object.
	void BeginObject(const maxon::Char* key = nullptr);
	void EndObject();
	void BeginArray(const maxon::Char* key = nullptr);
	void EndArray();

	/// Members of the current object. Pass nullptr as key to append an element to the
	/// current array instead.
	void String(const maxon::Char* key, const maxon::String& value);
	void Int(const maxon::Char* key, maxon::Int64 value);
	void Float(const maxon::Char* key, maxon::Float64 value);
	void Bool(const maxon::Char* key, maxon::Bool value);
	void Null(const maxon::Char* key);
	/// A member holding an array of numbers, e.g. a matrix.
	void Floats(const maxon::Char* key, const maxon::Float64* values, maxon::Int count);
	/// A member whose value is JSON text written elsewhere, inserted as it is.
	void Raw(const maxon::Char* key, const maxon::String& json);

	const maxon::String& GetText() const { return _text; }

private:
	void Separator(const maxon::Char* key);
	void AppendFloat(maxon::Float64 value);

	maxon::String _text;
	maxon::Bool _needComma = false;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_JSON_WRITER_H__
