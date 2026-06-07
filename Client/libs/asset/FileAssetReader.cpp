#include "asset/FileAssetReader.h"

#include <fstream>

namespace client::asset {
namespace {

std::string NormalizeAssetPath(std::string_view path)
{
    std::string normalized(path);
    for (char& c : normalized) {
        if (c == '\\') {
            c = '/';
        }
    }

    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    return normalized;
}

} // namespace

FileAssetReader::FileAssetReader(std::filesystem::path root)
    : m_root(std::move(root))
{
}

std::optional<std::vector<std::uint8_t>> FileAssetReader::ReadAll(std::string_view path) const
{
    const std::filesystem::path resolved = Resolve(path);
    std::ifstream file(resolved, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    const auto end = file.tellg();
    if (end < 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    file.seekg(0);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            return std::nullopt;
        }
    }
    return bytes;
}

std::optional<std::filesystem::path> FileAssetReader::RootPath() const
{
    return m_root;
}

std::filesystem::path FileAssetReader::Resolve(std::string_view path) const
{
    return m_root / std::filesystem::path(NormalizeAssetPath(path));
}

} // namespace client::asset
