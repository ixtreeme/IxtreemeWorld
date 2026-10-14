#pragma once

#include <filesystem>
#include <initializer_list>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ixtreeme::common
{
// Text in the engine's files and narrow strings is UTF-8 (on Windows too: see the executable's
// manifest). A string read back that is not valid UTF-8 was saved before that, as Windows-1250
// bytes (the code page the earlier files were written in): converted to UTF-8.
bool IsValidUtf8(std::string_view text);
std::string LegacyTextToUtf8(std::string text);
std::string ToLowerAscii(std::string value);
std::string EscapeJson(const std::string& value);
std::string TimestampUtc();
std::string JsonStringValue(const std::string& object, const std::string& key);
float JsonFloatValue(const std::string& object, const std::string& key, float fallback);
bool JsonBoolValue(const std::string& object, const std::string& key, bool fallback);
void JsonFloatArrayValue(const std::string& object, const std::string& key, float* values, std::size_t count);
void JsonBoolArrayValue(const std::string& object, const std::string& key, bool* values, std::size_t count);
std::vector<std::string> JsonStringArrayValue(const std::string& object, const std::string& key);
std::string GenericPath(const std::filesystem::path& path);
std::string CanonicalPathString(const std::filesystem::path& path);
bool HasAnyExtension(const std::filesystem::path& path, std::initializer_list<const char*> extensions);
}
