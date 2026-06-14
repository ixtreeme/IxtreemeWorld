#include "TreeTexturePalette.h"

#include "Debug.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace tree_tool
{
namespace
{
struct PaletteSpec
{
    const char* label;
    const char* relativePath;
    std::array<float, 4> previewColor;
    std::array<std::uint8_t, 4> baseColor;
    std::array<std::uint8_t, 4> accentColor;
};

constexpr std::array<PaletteSpec, 5> kBarkSpecs{{
    {"Oak", "textures/bark/oak_bark.png", {0.49f, 0.31f, 0.16f, 0.78f}, {110, 72, 35, 255}, {70, 45, 24, 255}},
    {"Birch", "textures/bark/birch_bark.png", {0.77f, 0.73f, 0.62f, 0.78f}, {194, 184, 156, 255}, {73, 65, 55, 255}},
    {"Pine", "textures/bark/pine_bark.png", {0.37f, 0.23f, 0.12f, 0.78f}, {85, 52, 27, 255}, {43, 29, 17, 255}},
    {"Willow", "textures/bark/willow_bark.png", {0.48f, 0.42f, 0.28f, 0.78f}, {120, 107, 75, 255}, {67, 72, 51, 255}},
    {"Ash", "textures/bark/ash_bark.png", {0.56f, 0.52f, 0.44f, 0.78f}, {142, 132, 111, 255}, {74, 70, 62, 255}},
}};

constexpr std::array<PaletteSpec, 5> kLeafSpecs{{
    {"Oak", "textures/leaves/oak_leaf.png", {0.25f, 0.52f, 0.22f, 0.82f}, {72, 142, 58, 230}, {37, 92, 34, 235}},
    {"Ash", "textures/leaves/ash_leaf.png", {0.34f, 0.61f, 0.30f, 0.82f}, {86, 157, 76, 230}, {42, 99, 45, 235}},
    {"Pine", "textures/leaves/pine_leaf.png", {0.14f, 0.41f, 0.24f, 0.82f}, {39, 105, 62, 235}, {18, 66, 42, 240}},
    {"Willow", "textures/leaves/willow_leaf.png", {0.43f, 0.61f, 0.31f, 0.82f}, {110, 156, 78, 225}, {67, 112, 51, 235}},
    {"Birch", "textures/leaves/birch_leaf.png", {0.51f, 0.69f, 0.30f, 0.82f}, {130, 176, 75, 225}, {81, 122, 37, 235}},
}};

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

std::uint32_t Adler32(const std::vector<std::uint8_t>& data)
{
    std::uint32_t a = 1u;
    std::uint32_t b = 0u;
    for (std::uint8_t byte : data)
    {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16u) | a;
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
}

void AppendChunk(std::vector<std::uint8_t>& png, const char type[4], const std::vector<std::uint8_t>& payload)
{
    AppendU32(png, static_cast<std::uint32_t>(payload.size()));
    const std::size_t typeOffset = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    const std::uint32_t crc = Crc32(png.data() + typeOffset, png.size() - typeOffset);
    AppendU32(png, crc);
}

bool WriteFallbackPng(const std::filesystem::path& path, const PaletteSpec& spec, bool leaf)
{
    constexpr std::uint32_t width = 256u;
    constexpr std::uint32_t height = 256u;
    std::vector<std::uint8_t> raw;
    raw.reserve((width * 4u + 1u) * height);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0u);
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::uint32_t n = (x * 37u + y * 53u + ((x ^ y) * 11u)) & 0xffu;
            const bool stripe = leaf
                ? (((x + y / 2u) % 47u) < 9u)
                : (((x / 7u + y * 3u) % 23u) < 4u);
            const auto& a = stripe ? spec.accentColor : spec.baseColor;
            const auto& b = stripe ? spec.baseColor : spec.accentColor;
            const std::uint32_t mix = leaf ? (n % 42u) : (n % 58u);
            raw.push_back(static_cast<std::uint8_t>((a[0] * (255u - mix) + b[0] * mix) / 255u));
            raw.push_back(static_cast<std::uint8_t>((a[1] * (255u - mix) + b[1] * mix) / 255u));
            raw.push_back(static_cast<std::uint8_t>((a[2] * (255u - mix) + b[2] * mix) / 255u));
            raw.push_back(leaf ? static_cast<std::uint8_t>(stripe ? 205u : 240u) : 255u);
        }
    }

    std::vector<std::uint8_t> zlib;
    zlib.push_back(0x78u);
    zlib.push_back(0x01u);
    std::size_t offset = 0;
    while (offset < raw.size())
    {
        const std::size_t block = std::min<std::size_t>(65535u, raw.size() - offset);
        const bool final = offset + block == raw.size();
        zlib.push_back(final ? 0x01u : 0x00u);
        const std::uint16_t len = static_cast<std::uint16_t>(block);
        const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
        zlib.push_back(static_cast<std::uint8_t>(len & 0xffu));
        zlib.push_back(static_cast<std::uint8_t>((len >> 8u) & 0xffu));
        zlib.push_back(static_cast<std::uint8_t>(nlen & 0xffu));
        zlib.push_back(static_cast<std::uint8_t>((nlen >> 8u) & 0xffu));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset), raw.begin() + static_cast<std::ptrdiff_t>(offset + block));
        offset += block;
    }
    AppendU32(zlib, Adler32(raw));

    std::vector<std::uint8_t> ihdr;
    AppendU32(ihdr, width);
    AppendU32(ihdr, height);
    ihdr.push_back(8u);
    ihdr.push_back(6u);
    ihdr.push_back(0u);
    ihdr.push_back(0u);
    ihdr.push_back(0u);

    std::vector<std::uint8_t> png{0x89u, 'P', 'N', 'G', 0x0du, 0x0au, 0x1au, 0x0au};
    AppendChunk(png, "IHDR", ihdr);
    AppendChunk(png, "IDAT", zlib);
    AppendChunk(png, "IEND", {});

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return out.good();
}

void EnsureTextureFile(const std::filesystem::path& path, const PaletteSpec& spec, bool leaf)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
        return;
    if (WriteFallbackPng(path, spec, leaf))
    {
        Tracenf("[TREE-3] generated procedural fallback for %s",
            std::filesystem::relative(path, path.parent_path().parent_path().parent_path(), ec).generic_string().c_str());
    }
    else
    {
        TraceError("[TREE-3] failed to generate procedural fallback for %s", path.generic_string().c_str());
    }
}

int BarkIndex(ixtreemetree::BarkType type)
{
    return std::clamp(static_cast<int>(type), 0, static_cast<int>(kBarkSpecs.size() - 1u));
}

int LeafIndex(ixtreemetree::LeafType type)
{
    return std::clamp(static_cast<int>(type), 0, static_cast<int>(kLeafSpecs.size() - 1u));
}
}

TreeTexturePalette& TreeTexturePalette::Instance()
{
    static TreeTexturePalette palette;
    return palette;
}

void TreeTexturePalette::EnsureLoaded(const std::filesystem::path& internalRoot)
{
    Load(internalRoot);
}

const TreePaletteTexture& TreeTexturePalette::Bark(ixtreemetree::BarkType type) const
{
    return bark_[static_cast<std::size_t>(BarkIndex(type))];
}

const TreePaletteTexture& TreeTexturePalette::Leaf(ixtreemetree::LeafType type) const
{
    return leaves_[static_cast<std::size_t>(LeafIndex(type))];
}

void TreeTexturePalette::Load(const std::filesystem::path& internalRoot)
{
    const std::filesystem::path root = internalRoot.empty()
        ? (std::filesystem::current_path() / "assets" / "internal")
        : internalRoot;

    for (std::size_t i = 0; i < kBarkSpecs.size(); ++i)
    {
        const PaletteSpec& spec = kBarkSpecs[i];
        const std::filesystem::path path = root / std::filesystem::path(spec.relativePath);
        EnsureTextureFile(path, spec, false);
        bark_[i] = {AssetDatabase::Instance().getOrCreateGuid(path), path, spec.previewColor};
    }
    for (std::size_t i = 0; i < kLeafSpecs.size(); ++i)
    {
        const PaletteSpec& spec = kLeafSpecs[i];
        const std::filesystem::path path = root / std::filesystem::path(spec.relativePath);
        EnsureTextureFile(path, spec, true);
        leaves_[i] = {AssetDatabase::Instance().getOrCreateGuid(path), path, spec.previewColor};
    }

    loaded_ = true;
    loadedRoot_ = root;
    Tracenf("[TREE-3] palette_loaded bark={Oak=%s, Birch=%s, Pine=%s, Willow=%s, Ash=%s}",
        bark_[0].guid.toString().c_str(),
        bark_[1].guid.toString().c_str(),
        bark_[2].guid.toString().c_str(),
        bark_[3].guid.toString().c_str(),
        bark_[4].guid.toString().c_str());
    Tracenf("[TREE-3] palette_loaded leaves={Oak=%s, Ash=%s, Pine=%s, Willow=%s, Birch=%s}",
        leaves_[0].guid.toString().c_str(),
        leaves_[1].guid.toString().c_str(),
        leaves_[2].guid.toString().c_str(),
        leaves_[3].guid.toString().c_str(),
        leaves_[4].guid.toString().c_str());
}
}
