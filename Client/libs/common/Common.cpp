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
bool IsValidUtf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size())
    {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        char32_t codePoint = 0;
        if (lead < 0x80)
        {
            ++i;
            continue;
        }
        if ((lead & 0xE0u) == 0xC0u) { length = 2; codePoint = lead & 0x1Fu; }
        else if ((lead & 0xF0u) == 0xE0u) { length = 3; codePoint = lead & 0x0Fu; }
        else if ((lead & 0xF8u) == 0xF0u) { length = 4; codePoint = lead & 0x07u; }
        else
            return false;
        if (i + length > text.size())
            return false;
        for (std::size_t k = 1; k < length; ++k)
        {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0u) != 0x80u)
                return false;
            codePoint = (codePoint << 6) | (next & 0x3Fu);
        }
        // Overlong forms, surrogates and values past U+10FFFF are not UTF-8.
        static constexpr char32_t kMinimum[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (codePoint < kMinimum[length] || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return false;
        i += length;
    }
    return true;
}

std::string LegacyTextToUtf8(std::string text)
{
    if (IsValidUtf8(text))
        return text;
    // Windows-1250, 0x80-0xFF (the bytes it leaves undefined kept as the same code point).
    static constexpr char16_t kWindows1250[128] = {
        0x20AC, 0x0081, 0x201A, 0x0083, 0x201E, 0x2026, 0x2020, 0x2021,
        0x0088, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x0098, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
        0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
        0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
        0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
        0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
        0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
        0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
        0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
        0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
        0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
        0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
        0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
        0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
    };
    std::string out;
    out.reserve(text.size() + text.size() / 2);
    for (const char ch : text)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x80)
        {
            out.push_back(ch);
            continue;
        }
        const char16_t codePoint = kWindows1250[byte - 0x80];
        if (codePoint < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }
    return out;
}

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
            return LegacyTextToUtf8(std::move(out));
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

bool JsonBoolValue(const std::string& object, const std::string& key, bool fallback)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return fallback;
    const size_t colon = object.find(':', keyPos + needle.size());
    if (colon == std::string::npos)
        return fallback;
    const size_t begin = object.find_first_not_of(" \t\r\n", colon + 1);
    if (begin == std::string::npos)
        return fallback;
    if (object.compare(begin, 4, "true") == 0)
        return true;
    if (object.compare(begin, 5, "false") == 0)
        return false;
    return fallback;
}

void JsonFloatArrayValue(const std::string& object, const std::string& key, float* values, std::size_t count)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return;
    const size_t open = object.find('[', keyPos + needle.size());
    const size_t close = open == std::string::npos ? std::string::npos : object.find(']', open + 1);
    if (open == std::string::npos || close == std::string::npos)
        return;

    const std::string arrayText = object.substr(open + 1, close - open - 1);
    const char* cursor = arrayText.c_str();
    for (std::size_t i = 0; i < count; ++i)
    {
        char* end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end == cursor)
            return;
        values[i] = value;
        cursor = end;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',')
            ++cursor;
    }
}

void JsonBoolArrayValue(const std::string& object, const std::string& key, bool* values, std::size_t count)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return;
    const size_t open = object.find('[', keyPos + needle.size());
    const size_t close = open == std::string::npos ? std::string::npos : object.find(']', open + 1);
    if (open == std::string::npos || close == std::string::npos)
        return;

    size_t cursor = open + 1;
    for (std::size_t i = 0; i < count && cursor < close; ++i)
    {
        cursor = object.find_first_not_of(" \t\r\n,", cursor);
        if (cursor == std::string::npos || cursor >= close)
            return;
        if (object.compare(cursor, 4, "true") == 0)
        {
            values[i] = true;
            cursor += 4;
        }
        else if (object.compare(cursor, 5, "false") == 0)
        {
            values[i] = false;
            cursor += 5;
        }
        else
        {
            return;
        }
    }
}

std::vector<std::string> JsonStringArrayValue(const std::string& object, const std::string& key)
{
    std::vector<std::string> values;
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos)
        return values;
    const size_t open = object.find('[', keyPos + needle.size());
    const size_t close = open == std::string::npos ? std::string::npos : object.find(']', open + 1);
    if (open == std::string::npos || close == std::string::npos)
        return values;

    size_t cursor = open + 1;
    while (cursor < close)
    {
        const size_t begin = object.find('"', cursor);
        if (begin == std::string::npos || begin >= close)
            break;
        const size_t end = object.find('"', begin + 1);
        if (end == std::string::npos || end > close)
            break;
        values.push_back(LegacyTextToUtf8(object.substr(begin + 1, end - begin - 1)));
        cursor = end + 1;
    }
    return values;
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
