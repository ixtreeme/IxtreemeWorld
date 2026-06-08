#include "asset/AAssetManagerAssetReader.h"
#include "Debug.h"

#include <android/asset_manager.h>

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

std::string StripAssetsPrefix(std::string_view path)
{
    std::string normalized = NormalizeAssetPath(path);
    constexpr std::string_view assetsPrefix = "assets/";
    while (normalized.rfind(assetsPrefix, 0) == 0) {
        normalized.erase(0, assetsPrefix.size());
    }
    return normalized;
}

} // namespace

AAssetManagerAssetReader::AAssetManagerAssetReader(AAssetManager* manager)
    : m_manager(manager)
{
    Tracenf("[ASSET] AAssetManagerAssetReader created manager=%p", static_cast<void*>(m_manager));
    DumpAssetDirectory("");
    DumpAssetDirectory("xaml");
    DumpAssetDirectory("assets");
    DumpAssetDirectory("assets/ui");
}

void AAssetManagerAssetReader::DumpAssetDirectory(const std::string& directory) const
{
    if (!m_manager) {
        Tracenf("[ASSET] Cannot list directory '%s': asset manager is null", directory.c_str());
        return;
    }

    Tracenf("[ASSET] Listing AAssetManager directory: '%s'", directory.c_str());
    AAssetDir* assetDir = AAssetManager_openDir(m_manager, directory.c_str());
    if (!assetDir) {
        Tracenf("[ASSET]   (directory could not be opened)");
        return;
    }

    int count = 0;
    while (const char* filename = AAssetDir_getNextFileName(assetDir)) {
        Tracenf("[ASSET]   - %s", filename);
        ++count;
        if (count >= 50) {
            Tracenf("[ASSET]   ... (truncated after 50 entries)");
            break;
        }
    }

    if (count == 0) {
        Tracenf("[ASSET]   (empty)");
    }
    AAssetDir_close(assetDir);
}

std::optional<std::vector<std::uint8_t>> AAssetManagerAssetReader::ReadAll(
    std::string_view path) const
{
    if (!m_manager) {
        Tracenf("[ASSET] ReadAll failed: asset manager is null for path '%.*s'",
            static_cast<int>(path.size()), path.data());
        return std::nullopt;
    }

    const std::string normalized = NormalizeAssetPath(path);
    const std::string strippedPath = StripAssetsPrefix(path);
    Tracenf("[ASSET] ReadAll called with path: '%s'", normalized.c_str());
    Tracenf("[ASSET] After StripAssetsPrefix: '%s'", strippedPath.c_str());

    if (strippedPath.empty()) {
        Tracenf("[ASSET] AAssetManager_open skipped: empty path");
        return std::nullopt;
    }

    std::string openedPath = normalized;
    AAsset* asset = AAssetManager_open(m_manager, normalized.c_str(), AASSET_MODE_BUFFER);
    if (!asset && strippedPath != normalized) {
        Tracenf("[ASSET] AAssetManager_open FAILED for '%s'; retrying stripped path",
            normalized.c_str());
        openedPath = strippedPath;
        asset = AAssetManager_open(m_manager, strippedPath.c_str(), AASSET_MODE_BUFFER);
    }
    if (!asset) {
        Tracenf("[ASSET] AAssetManager_open FAILED for '%s'", openedPath.c_str());
        return std::nullopt;
    }

    const off_t length = AAsset_getLength(asset);
    Tracenf("[ASSET] Asset opened: '%s', length=%ld bytes", openedPath.c_str(),
        static_cast<long>(length));
    if (length < 0) {
        AAsset_close(asset);
        Tracenf("[ASSET] AAsset_getLength failed for '%s'", openedPath.c_str());
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    const int read = AAsset_read(asset, bytes.data(), static_cast<std::size_t>(length));
    AAsset_close(asset);

    if (read != length) {
        Tracenf("[ASSET] AAsset_read incomplete for '%s': got %d, expected %ld",
            openedPath.c_str(), read, static_cast<long>(length));
        return std::nullopt;
    }
    Tracenf("[ASSET] Read OK: '%s' (%d bytes)", openedPath.c_str(), read);
    return bytes;
}

} // namespace client::asset
