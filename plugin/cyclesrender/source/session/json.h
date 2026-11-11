// SPDX-License-Identifier: Apache-2.0
// JSON helpers for protocol headers, on top of the SDK's JSON parser.

#ifndef CYCLESRENDER_JSON_H__
#define CYCLESRENDER_JSON_H__

#include "c4d.h"
#include "maxon/datadictionary.h"

namespace cyclesrender
{

/// Parses a JSON object.
maxon::Result<maxon::DataDictionary> ParseJson(const maxon::String& text);

/// Writes a dictionary as compact JSON.
maxon::Result<maxon::String> WriteJson(const maxon::DataDictionary& dictionary);

/// Reads a number as an integer, whichever numeric type the parser stored. @p fallback if absent.
maxon::Int64 GetJsonInt(const maxon::DataDictionary& dictionary, const maxon::String& key, maxon::Int64 fallback);

/// Reads a string. Empty if absent or not a string.
maxon::String GetJsonString(const maxon::DataDictionary& dictionary, const maxon::String& key);

/// Reads a boolean. @p fallback if absent or not a boolean.
maxon::Bool GetJsonBool(const maxon::DataDictionary& dictionary, const maxon::String& key, maxon::Bool fallback);

/// Reads a list of strings. Empty if absent; entries that are not strings are skipped.
maxon::BaseArray<maxon::String> GetJsonStrings(const maxon::DataDictionary& dictionary, const maxon::String& key);

} // namespace cyclesrender

#endif // CYCLESRENDER_JSON_H__
