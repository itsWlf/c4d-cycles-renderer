// SPDX-License-Identifier: Apache-2.0
// JSON helpers on top of the SDK's parser (json.h).

#include "c4d.h"
#include "maxon/parser.h"
#include "maxon/valuereceiver.h"

#include "json.h"

namespace cyclesrender
{

maxon::Result<maxon::DataDictionary> ParseJson(const maxon::String& text)
{
	iferr_scope;

	const maxon::ParserRef parser = maxon::ParserClasses::JsonParser().Create() iferr_return;
	maxon::SingleValueReceiver<const maxon::DataDictionary&> receiver;
	parser.ReadString(text, maxon::PARSERFLAGS::NONE, maxon::GetUtf8DefaultDecoder(), receiver) iferr_return;
	return receiver.Get().ToResult();
}

maxon::Result<maxon::String> WriteJson(const maxon::DataDictionary& dictionary)
{
	iferr_scope;

	const maxon::ParserRef parser = maxon::ParserClasses::JsonParser().Create() iferr_return;
	maxon::String text;
	parser.Write(dictionary, text, false) iferr_return;
	return text;
}

maxon::Int64 GetJsonInt(const maxon::DataDictionary& dictionary, const maxon::String& key, maxon::Int64 fallback)
{
	// The parser decides which numeric type a JSON number becomes; accept all of them.
	maxon::Result<maxon::Int64> asInt64 = dictionary.Get<maxon::Int64>(key);
	if (asInt64 == maxon::OK)
		return asInt64.GetValue();
	maxon::Result<maxon::Int32> asInt32 = dictionary.Get<maxon::Int32>(key);
	if (asInt32 == maxon::OK)
		return asInt32.GetValue();
	maxon::Result<maxon::Float64> asFloat = dictionary.Get<maxon::Float64>(key);
	if (asFloat == maxon::OK)
		return maxon::Int64(asFloat.GetValue());
	return fallback;
}

maxon::String GetJsonString(const maxon::DataDictionary& dictionary, const maxon::String& key)
{
	maxon::Result<maxon::String> value = dictionary.Get<maxon::String>(key);
	return value == maxon::OK ? value.GetValue() : maxon::String();
}

maxon::Bool GetJsonBool(const maxon::DataDictionary& dictionary, const maxon::String& key, maxon::Bool fallback)
{
	maxon::Result<maxon::Bool> value = dictionary.Get<maxon::Bool>(key);
	return value == maxon::OK ? value.GetValue() : fallback;
}

maxon::BaseArray<maxon::String> GetJsonStrings(const maxon::DataDictionary& dictionary, const maxon::String& key)
{
	maxon::BaseArray<maxon::String> result;
	maxon::BaseArray<maxon::Data> entries;
	iferr (dictionary.GetCopy(key, entries))
		return result;
	for (const maxon::Data& entry : entries)
	{
		const maxon::Result<maxon::String> text = entry.Get<maxon::String>();
		if (text == maxon::OK)
			result.Append(text.GetValue()) iferr_ignore("A shorter list is acceptable when memory is out.");
	}
	return result;
}

} // namespace cyclesrender
