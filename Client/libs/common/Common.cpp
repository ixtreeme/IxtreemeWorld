#include "Common.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace ixtreeme::common
{
std::string ToLowerAscii(std::string value)
{
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string EscapeJson(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string TimestampUtc()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string JsonStringValue(const std::string& object, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return {};
    const size_t colon = object.find(':', keyPos + needle.size());
    if (colon == std::string::npos)
        return {};
    const size_t firstQuote = object.find('"', colon + 1);
    if (firstQuote == std::string::npos)
        return {};

    std::string out;
    bool escaping = false;
    for (size_t i = firstQuote + 1; i < object.size(); ++i)
    {
        const char ch = object[i];
        if (escaping)
        {
            out += ch;
            escaping = false;
            continue;
        }
        if (ch == '\\')
        {
            escaping = true;
            continue;
        }
        if (ch == '"')
            return out;
        out += ch;
    }
    return {};
}

float JsonFloatValue(const std::string& object, const std::string& key, float fallback)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return fallback;
    const size_t colon = object.find(':', keyPos + needle.size());
    if (colon == std::string::npos)
        return fallback;
    const char* begin = object.c_str() + colon + 1;
    char* end = nullptr;
    const float value = std::strtof(begin, &end);
    return end != begin ? value : fallback;
}

std::string GenericPath(const std::filesystem::path& path)
{
    return path.generic_string();
}

std::string CanonicalPathString(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec)
        return canonical.generic_string();
    ec.clear();
    const auto absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).generic_string();
}

bool HasAnyExtension(const std::filesystem::path& path, std::initializer_list<const char*> extensions)
{
    const std::string ext = ToLowerAscii(path.extension().string());
    return std::any_of(extensions.begin(), extensions.end(),
        [&](const char* candidate) { return ext == candidate; });
}
}
