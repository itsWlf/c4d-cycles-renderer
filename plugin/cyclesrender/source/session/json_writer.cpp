// SPDX-License-Identifier: Apache-2.0
// Writes JSON text piece by piece (json_writer.h).

#include <stdio.h>

#include "c4d.h"

#include "json_writer.h"

namespace cyclesrender
{

static maxon::String Escaped(const maxon::String& value)
{
	maxon::String result = "\""_s;
	for (maxon::Int i = 0; i < value.GetLength(); ++i)
	{
		const maxon::Utf32Char c = value[i];
		if (c == '"')
			result += "\\\""_s;
		else if (c == '\\')
			result += "\\\\"_s;
		else if (c == '\n')
			result += "\\n"_s;
		else if (c == '\r')
			result += "\\r"_s;
		else if (c == '\t')
			result += "\\t"_s;
		else if (c < 0x20)
			result += " "_s;
		else
			result.AppendChar(c) iferr_ignore("Out of memory is reported when the frame is sent.");
	}
	result += "\""_s;
	return result;
}

void JsonWriter::Separator(const maxon::Char* key)
{
	if (_needComma)
		_text += ","_s;
	if (key)
		_text += "\""_s + maxon::String(key) + "\":"_s;
	_needComma = true;
}

void JsonWriter::AppendFloat(maxon::Float64 value)
{
	// JSON has no NaN or infinity; nine significant digits round-trip a 32-bit float.
	if (value != value || value > 1e300 || value < -1e300)
		value = 0.0;
	maxon::Char buffer[40];
	snprintf(buffer, sizeof(buffer), "%.9g", value);
	for (maxon::Char* c = buffer; *c; ++c)
	{
		if (*c == ',')
			*c = '.'; // in case the process runs with a comma as decimal separator
	}
	_text += maxon::String(buffer);
}

void JsonWriter::BeginObject(const maxon::Char* key)
{
	Separator(key);
	_text += "{"_s;
	_needComma = false;
}

void JsonWriter::EndObject()
{
	_text += "}"_s;
	_needComma = true;
}

void JsonWriter::BeginArray(const maxon::Char* key)
{
	Separator(key);
	_text += "["_s;
	_needComma = false;
}

void JsonWriter::EndArray()
{
	_text += "]"_s;
	_needComma = true;
}

void JsonWriter::String(const maxon::Char* key, const maxon::String& value)
{
	Separator(key);
	_text += Escaped(value);
}

void JsonWriter::Int(const maxon::Char* key, maxon::Int64 value)
{
	Separator(key);
	_text += maxon::String::IntToString(value);
}

void JsonWriter::Float(const maxon::Char* key, maxon::Float64 value)
{
	Separator(key);
	AppendFloat(value);
}

void JsonWriter::Bool(const maxon::Char* key, maxon::Bool value)
{
	Separator(key);
	_text += value ? "true"_s : "false"_s;
}

void JsonWriter::Raw(const maxon::Char* key, const maxon::String& json)
{
	Separator(key);
	_text += json;
}

void JsonWriter::Null(const maxon::Char* key)
{
	Separator(key);
	_text += "null"_s;
}

void JsonWriter::Floats(const maxon::Char* key, const maxon::Float64* values, maxon::Int count)
{
	Separator(key);
	_text += "["_s;
	for (maxon::Int i = 0; i < count; ++i)
	{
		if (i > 0)
			_text += ","_s;
		AppendFloat(values[i]);
	}
	_text += "]"_s;
}

} // namespace cyclesrender
