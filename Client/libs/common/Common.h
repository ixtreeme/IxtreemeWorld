#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>

namespace ixtreeme::common
{
std::string ToLowerAscii(std::string value);
std::string EscapeJson(const std::string& value);
std::string TimestampUtc();
std::string JsonStringValue(const std::string& object, const std::string& key);
float JsonFloatValue(const std::string& object, const std::string& key, float fallback);
std::string GenericPath(const std::filesystem::path& path);
std::string CanonicalPathString(const std::filesystem::path& path);
bool HasAnyExtension(const std::filesystem::path& path, std::initializer_list<const char*> extensions);
}
