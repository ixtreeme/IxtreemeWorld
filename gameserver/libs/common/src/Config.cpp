#include "common/Config.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <string_view>

namespace gs::common {
namespace {

std::string Trim(std::string_view value)
{
    const auto first = std::ranges::find_if(value, [](unsigned char ch) {
        return !std::isspace(ch);
    });
    const auto last = std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
                          return !std::isspace(ch);
                      }).base();

    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

} // namespace

bool Config::Load(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        return false;
    }

    values_.clear();

    std::string line;
    while (std::getline(file, line)) {
        if (const auto comment_pos = line.find('#'); comment_pos != std::string::npos) {
            line.erase(comment_pos);
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        auto key = Trim(std::string_view(line).substr(0, separator));
        auto value = Trim(std::string_view(line).substr(separator + 1));
        if (!key.empty()) {
            values_[std::move(key)] = std::move(value);
        }
    }

    return true;
}

std::optional<std::string> Config::GetString(const std::string& key) const
{
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<int> Config::GetInt(const std::string& key) const
{
    const auto value = GetString(key);
    if (!value) {
        return std::nullopt;
    }

    int result = 0;
    const auto* begin = value->data();
    const auto* end = begin + value->size();
    const auto [ptr, error] = std::from_chars(begin, end, result);
    if (error != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return result;
}

} // namespace gs::common
