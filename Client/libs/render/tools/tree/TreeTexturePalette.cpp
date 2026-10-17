#include "TreeTexturePalette.h"

#include "Debug.h"
#include "math/IXMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace tree_tool
{
namespace
{
namespace xm = ixtreeme::math;

enum class BarkStyle { Stripes, Rings, Fissures };

struct PaletteSpec
{
    const char* label;
    const char* relativePath;
    std::array<float, 4> previewColor;
    std::array<std::uint8_t, 4> baseColor;
    std::array<std::uint8_t, 4> accentColor;
    int gridX = 2;       // leaf atlas cells across and down (the image stays 512 x 512)
    int gridY = 2;
    BarkStyle barkStyle = BarkStyle::Stripes;
};

constexpr PaletteSpec kBarkSpecs[]{
    {"Oak", "textures/bark/oak_bark.png", {0.49f, 0.31f, 0.16f, 0.78f}, {110, 72, 35, 255}, {70, 45, 24, 255}},
    {"Birch", "textures/bark/birch_bark.png", {0.77f, 0.73f, 0.62f, 0.78f}, {194, 184, 156, 255}, {73, 65, 55, 255}},
    {"Pine", "textures/bark/pine_bark.png", {0.37f, 0.23f, 0.12f, 0.78f}, {85, 52, 27, 255}, {43, 29, 17, 255}},
    {"Willow", "textures/bark/willow_bark.png", {0.48f, 0.42f, 0.28f, 0.78f}, {120, 107, 75, 255}, {67, 72, 51, 255}},
    {"Ash", "textures/bark/ash_bark.png", {0.56f, 0.52f, 0.44f, 0.78f}, {142, 132, 111, 255}, {74, 70, 62, 255}},
    {"Palm", "textures/bark/palm_bark.png", {0.52f, 0.42f, 0.30f, 0.78f}, {138, 112, 80, 255}, {74, 58, 40, 255}, 2, 2, BarkStyle::Rings},
    {"Larch", "textures/bark/larch_bark.png", {0.72f, 0.58f, 0.39f, 1.0f}, {196, 163, 117, 255}, {110, 88, 57, 255}, 2, 2, BarkStyle::Fissures},
};

constexpr PaletteSpec kLeafSpecs[]{
    {"Oak", "textures/leaves/oak_leaf.png", {0.25f, 0.52f, 0.22f, 0.82f}, {72, 142, 58, 230}, {37, 92, 34, 235}},
    {"Ash", "textures/leaves/ash_leaf.png", {0.34f, 0.61f, 0.30f, 0.82f}, {86, 157, 76, 230}, {42, 99, 45, 235}},
    {"Pine", "textures/leaves/pine_leaf.png", {0.14f, 0.41f, 0.24f, 0.82f}, {39, 105, 62, 235}, {18, 66, 42, 240}},
    {"Willow", "textures/leaves/willow_leaf.png", {0.43f, 0.61f, 0.31f, 0.82f}, {110, 156, 78, 225}, {67, 112, 51, 235}},
    {"Birch", "textures/leaves/birch_leaf.png", {0.51f, 0.69f, 0.30f, 0.82f}, {130, 176, 75, 225}, {81, 122, 37, 235}},
    {"Palm", "textures/leaves/palm_frond.png", {0.27f, 0.52f, 0.22f, 0.82f}, {72, 138, 54, 235}, {36, 88, 32, 240}, 2, 1},
    {"Blossom", "textures/leaves/blossom_leaf.png", {0.93f, 0.66f, 0.77f, 0.82f}, {246, 178, 206, 235}, {206, 96, 146, 240}},
    {"SnowyPine", "textures/leaves/snowy_pine_leaf.png", {0.55f, 0.66f, 0.62f, 0.82f}, {39, 105, 62, 235}, {18, 66, 42, 240}},
    {"Larch", "textures/leaves/golden_larch.png", {0.83f, 0.65f, 0.18f, 1.0f}, {222, 180, 43, 255}, {117, 120, 34, 255}},
};
static_assert(std::size(kBarkSpecs) == ixtreemetree::kBarkTypeCount);
static_assert(std::size(kLeafSpecs) == ixtreemetree::kLeafTypeCount);

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

std::uint8_t MixByte(std::uint8_t a, std::uint8_t b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(static_cast<float>(a) * (1.0f - t) + static_cast<float>(b) * t + 0.5f);
}

float SmoothStep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / std::max(0.0001f, edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A palm frond on a tall cell (x across, -1..1; y down, -1 its tip, 1 its base): a midrib, a bare
// stalk at the base, and narrow leaflets slanting toward the tip, longest at the middle.
float FrondSilhouette(int variant, float x, float y)
{
    const float s = (1.0f - y) * 0.5f;  // 0 at the base, 1 at the tip
    const float ax = xm::Abs(x);
    const float midrib = (ax < 0.05f * (1.0f - 0.6f * s) && s < 0.985f) ? 1.0f : 0.0f;
    if (s < 0.1f)
        return midrib;
    const float slant = 0.18f + 0.03f * static_cast<float>(variant % 2);
    const float s0 = s - slant * ax;  // where this point's leaflet leaves the midrib
    if (s0 < 0.1f || s0 > 0.97f)
        return midrib;
    const float reach = 0.97f * xm::Pow(xm::Sin(xm::Pi * (s0 - 0.1f) / 0.87f), 0.55f);
    if (ax > reach)
        return midrib;
    const float period = 0.021f;
    const float phase = s0 / period - std::floor(s0 / period);
    const float thickness = 0.42f * xm::Pow(1.0f - ax / std::max(reach, 0.001f), 0.35f);
    const float leaflet = 1.0f - SmoothStep(thickness * 0.5f - 0.06f, thickness * 0.5f + 0.02f, xm::Abs(phase - 0.5f));
    return std::max(midrib, leaflet);
}

// Five-petalled flowers scattered over the cell, the variant choosing where.
float BlossomSilhouette(int variant, float x, float y, float& centre)
{
    float mask = 0.0f;
    centre = 0.0f;
    std::uint32_t seed = 2654435761u * static_cast<std::uint32_t>(variant + 1);
    for (int flower = 0; flower < 7; ++flower)
    {
        seed = seed * 1664525u + 1013904223u;
        const float fx = (static_cast<float>(seed >> 8 & 0xffffu) / 65535.0f) * 1.3f - 0.65f;
        seed = seed * 1664525u + 1013904223u;
        const float fy = (static_cast<float>(seed >> 8 & 0xffffu) / 65535.0f) * 1.3f - 0.65f;
        const float dx = x - fx;
        const float dy = y - fy;
        const float r = std::sqrt(dx * dx + dy * dy);
        const float petals = 0.2f * (0.55f + 0.45f * xm::Abs(xm::Cos(2.5f * std::atan2(dy, dx) + static_cast<float>(flower))));
        mask = std::max(mask, 1.0f - SmoothStep(petals - 0.02f, petals + 0.015f, r));
        centre = std::max(centre, 1.0f - SmoothStep(0.035f, 0.06f, r));
    }
    return mask;
}

float LeafSilhouette(const char* label, int variant, float x, float y)
{
    const float bend = 0.08f * xm::Sin((static_cast<float>(variant) + 1.0f) * 1.7f);
    x += bend * y * y;
    const float absY = xm::Abs(y);
    const float vein = xm::Abs(x);

    float width = 0.0f;
    if (std::strcmp(label, "Pine") == 0 || std::strcmp(label, "SnowyPine") == 0)
    {
        const float centerNeedle = std::max(0.0f, 1.0f - xm::Abs(x) / 0.07f) * std::max(0.0f, 1.0f - absY / 0.88f);
        const float sideNeedleA = std::max(0.0f, 1.0f - xm::Abs(x - 0.16f * y) / 0.055f) * std::max(0.0f, 1.0f - absY / 0.78f);
        const float sideNeedleB = std::max(0.0f, 1.0f - xm::Abs(x + 0.15f * y) / 0.055f) * std::max(0.0f, 1.0f - absY / 0.78f);
        return std::max({centerNeedle, sideNeedleA, sideNeedleB});
    }
    if (std::strcmp(label, "Willow") == 0)
    {
        width = 0.12f + 0.11f * (1.0f - absY);
        const float taper = 1.0f - SmoothStep(0.64f, 0.95f, absY);
        return (1.0f - SmoothStep(width * taper, width * taper + 0.04f, vein)) * taper;
    }
    if (std::strcmp(label, "Oak") == 0)
    {
        width = 0.22f + 0.33f * (1.0f - y * y);
        width += 0.08f * xm::Sin((y * 18.0f) + static_cast<float>(variant));
        const float body = (1.0f - SmoothStep(width, width + 0.045f, vein)) * (1.0f - SmoothStep(0.83f, 0.99f, absY));
        const float stemCut = SmoothStep(-0.96f, -0.74f, y);
        return body * stemCut;
    }
    if (std::strcmp(label, "Birch") == 0)
    {
        width = 0.16f + 0.37f * (1.0f - xm::Pow(absY, 1.45f));
        width *= (y > 0.0f) ? (1.0f - 0.25f * y) : (1.0f + 0.12f * y);
        return (1.0f - SmoothStep(width, width + 0.04f, vein)) * (1.0f - SmoothStep(0.82f, 0.98f, absY));
    }

    width = 0.14f + 0.32f * (1.0f - absY);
    width *= 1.0f + 0.08f * xm::Sin(y * 22.0f + static_cast<float>(variant));
    return (1.0f - SmoothStep(width, width + 0.04f, vein)) * (1.0f - SmoothStep(0.86f, 0.99f, absY));
}

void LeafPixel(const PaletteSpec& spec, std::uint32_t x, std::uint32_t y, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, std::uint8_t& a)
{
    const std::uint32_t cellWidth = 512u / static_cast<std::uint32_t>(std::max(1, spec.gridX));
    const std::uint32_t cellHeight = 512u / static_cast<std::uint32_t>(std::max(1, spec.gridY));
    const int cellX = static_cast<int>(x / cellWidth);
    const int cellY = static_cast<int>(y / cellHeight);
    const int variant = cellY * spec.gridX + cellX;
    const float localX = ((static_cast<float>(x % cellWidth) + 0.5f) / static_cast<float>(cellWidth)) * 2.0f - 1.0f;
    const float localY = ((static_cast<float>(y % cellHeight) + 0.5f) / static_cast<float>(cellHeight)) * 2.0f - 1.0f;
    if (std::strcmp(spec.label, "Larch") == 0)
    {
        // Own needle spray: a curved shoot with alternating short-shoot rosettes.
        auto distance = [&](float ax, float ay, float bx, float by) {
            const float dx=bx-ax, dy=by-ay;
            const float t=std::clamp(((localX-ax)*dx+(localY-ay)*dy)/(dx*dx+dy*dy+1e-8f),0.0f,1.0f);
            return std::hypot(localX-ax-t*dx,localY-ay-t*dy);
        };
        const float stem = 0.035f * std::sin((1-localY)*2 + variant);
        float d = (localY > -0.92f && localY < 0.94f) ? std::abs(localX-stem) : 10.0f;
        const bool wood = d < 0.012f;
        for (int shoot=0; shoot<14; ++shoot)
        {
            const float s=(shoot+1.0f)/15.0f;
            const float sign=shoot%2 ? -1.0f : 1.0f;
            const float sy=0.82f-1.64f*s;
            const float sx=0.035f*std::sin((1-sy)*2+variant);
            const float reach=0.22f+0.37f*std::sin(s*xm::Pi);
            for (int needle=0; needle<7; ++needle)
            {
                const float angle=-0.9f+needle*0.22f+0.08f*variant;
                const float endX=sx+sign*reach*std::cos(angle);
                const float endY=sy-reach*std::sin(angle)-0.12f;
                d=std::min(d,distance(sx,sy,endX,endY));
            }
        }
        const float mask=1-SmoothStep(0.006f,0.016f,d);
        const float variation=0.5f+0.5f*std::sin(localY*31+variant*7+localX*9);
        r=wood ? 109 : MixByte(178,249,variation);
        g=wood ? 83 : MixByte(146,207,variation);
        b=wood ? 39 : MixByte(27,71,variation);
        a=static_cast<std::uint8_t>(255*mask);
        return;
    }
    const bool frond = std::strcmp(spec.label, "Palm") == 0;
    const bool blossom = std::strcmp(spec.label, "Blossom") == 0;
    if (frond || blossom)
    {
        // A frond stays upright on its cell (the generator puts its midrib down the middle).
        float centre = 0.0f;
        const float mask = frond ? FrondSilhouette(variant, localX, localY) : BlossomSilhouette(variant, localX, localY, centre);
        if (mask <= 0.01f)
        {
            r = g = b = a = 0u;
            return;
        }
        const std::uint32_t n = (x * 37u + y * 53u + static_cast<std::uint32_t>(variant * 79)) & 0xffu;
        const float noise = static_cast<float>(n) / 255.0f;
        const float rib = frond ? std::max(0.0f, 1.0f - xm::Abs(localX) / 0.06f) : 0.0f;
        const float accent = std::clamp((1.0f - mask) * 0.8f + rib * 0.6f + noise * 0.15f, 0.0f, 1.0f);
        r = MixByte(spec.baseColor[0], spec.accentColor[0], accent);
        g = MixByte(spec.baseColor[1], spec.accentColor[1], accent);
        b = MixByte(spec.baseColor[2], spec.accentColor[2], accent);
        if (blossom)
        {
            const float pale = 0.45f * (1.0f - centre) * (0.5f + 0.5f * xm::Sin(localY * 9.0f + variant));
            r = MixByte(r, 255, pale);
            g = MixByte(g, 246, pale);
            b = MixByte(b, 250, pale);
        }
        if (blossom && centre > 0.0f)  // a yellow eye in each flower
        {
            r = MixByte(r, 250, centre);
            g = MixByte(g, 214, centre);
            b = MixByte(b, 90, centre);
        }
        a = static_cast<std::uint8_t>(std::clamp(mask, 0.0f, 1.0f) * 235.0f + 0.5f);
        return;
    }

    const float rotation = (static_cast<float>(variant) - 1.5f) * 0.18f;
    const float cr = xm::Cos(rotation);
    const float sr = xm::Sin(rotation);
    const float rx = localX * cr - localY * sr;
    const float ry = localX * sr + localY * cr;

    const float mask = LeafSilhouette(spec.label, variant, rx, ry);
    if (mask <= 0.01f)
    {
        r = g = b = a = 0u;
        return;
    }

    const float vein = std::max(0.0f, 1.0f - xm::Abs(rx) / 0.045f) * std::max(0.0f, 1.0f - xm::Abs(ry) / 0.86f);
    const float edge = 1.0f - SmoothStep(0.04f, 0.16f, mask);
    const std::uint32_t n = (x * 37u + y * 53u + static_cast<std::uint32_t>(variant * 79)) & 0xffu;
    const float noise = static_cast<float>(n) / 255.0f;
    const float accent = std::clamp(edge * 0.65f + vein * 0.45f + noise * 0.12f, 0.0f, 1.0f);

    r = MixByte(spec.baseColor[0], spec.accentColor[0], accent);
    g = MixByte(spec.baseColor[1], spec.accentColor[1], accent);
    b = MixByte(spec.baseColor[2], spec.accentColor[2], accent);
    if (std::strcmp(spec.label, "SnowyPine") == 0)
    {
        // Snow lies on the upper side of the needles.
        const float snow = (1.0f - SmoothStep(-0.55f, 0.25f, ry)) * (0.65f + 0.35f * noise);
        r = MixByte(r, 236, snow);
        g = MixByte(g, 242, snow);
        b = MixByte(b, 248, snow);
    }
    a = static_cast<std::uint8_t>(std::clamp(mask, 0.0f, 1.0f) * 235.0f + 0.5f);
}

std::pair<std::uint32_t, std::uint32_t> PngDimensions(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::uint8_t header[24]{};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in || header[0] != 0x89u || header[1] != 'P' || header[2] != 'N' || header[3] != 'G')
        return {};
    const auto readU32 = [](const std::uint8_t* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24u) |
            (static_cast<std::uint32_t>(p[1]) << 16u) |
            (static_cast<std::uint32_t>(p[2]) << 8u) |
            static_cast<std::uint32_t>(p[3]);
    };
    return {readU32(header + 16), readU32(header + 20)};
}

bool WriteFallbackPng(const std::filesystem::path& path, const PaletteSpec& spec, bool leaf)
{
    const std::uint32_t width = leaf ? 512u : 256u;
    const std::uint32_t height = leaf ? 512u : 256u;
    std::vector<std::uint8_t> raw;
    raw.reserve((width * 4u + 1u) * height);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0u);
        for (std::uint32_t x = 0; x < width; ++x)
        {
            if (leaf)
            {
                std::uint8_t r = 0, g = 0, b = 0, a = 0;
                LeafPixel(spec, x, y, r, g, b, a);
                raw.push_back(r);
                raw.push_back(g);
                raw.push_back(b);
                raw.push_back(a);
                continue;
            }

            if (spec.barkStyle == BarkStyle::Fissures)
            {
                const float warp = 2.1f * std::sin(y * 0.02454369f) + 0.7f * std::sin(y * 0.09817477f);
                const float crease = std::pow(0.5f + 0.5f * std::sin((x + warp) * 0.19634954f), 18.0f);
                const float grain = 0.5f + 0.5f * std::sin(x * 1.5707963f + std::sin(y * 0.04908738f));
                const float amount = std::clamp(crease * 0.85f + grain * 0.12f, 0.0f, 1.0f);
                for (int channel=0; channel<3; ++channel)
                    raw.push_back(MixByte(spec.baseColor[channel],spec.accentColor[channel],amount));
                raw.push_back(255u);
                continue;
            }
            const std::uint32_t n = (x * 37u + y * 53u + ((x ^ y) * 11u)) & 0xffu;
            const bool stripe = spec.barkStyle == BarkStyle::Rings
                ? ((y + (x / 16u) % 3u) % 24u) < 6u : (((x / 7u + y * 3u) % 23u) < 4u);
            const auto& a = stripe ? spec.accentColor : spec.baseColor;
            const auto& b = stripe ? spec.baseColor : spec.accentColor;
            const std::uint32_t mix = n % 58u;
            raw.push_back(static_cast<std::uint8_t>((a[0] * (255u - mix) + b[0] * mix) / 255u));
            raw.push_back(static_cast<std::uint8_t>((a[1] * (255u - mix) + b[1] * mix) / 255u));
            raw.push_back(static_cast<std::uint8_t>((a[2] * (255u - mix) + b[2] * mix) / 255u));
            raw.push_back(255u);
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
    {
        if (!leaf)
            return;
        const auto [width, height] = PngDimensions(path);
        if (width == 512u && height == 512u)
            return;
    }
    if (WriteFallbackPng(path, spec, leaf))
    {
        Tracenf("[TREE-3] generated procedural %s for %s",
            leaf ? "leaf atlas" : "fallback",
            std::filesystem::relative(path, path.parent_path().parent_path().parent_path(), ec).generic_string().c_str());
    }
    else
    {
        TraceError("[TREE-3] failed to generate procedural fallback for %s", path.generic_string().c_str());
    }
}

int BarkIndex(ixtreemetree::BarkType type)
{
    return std::clamp(static_cast<int>(type), 0, static_cast<int>(std::size(kBarkSpecs) - 1u));
}

int LeafIndex(ixtreemetree::LeafType type)
{
    return std::clamp(static_cast<int>(type), 0, static_cast<int>(std::size(kLeafSpecs) - 1u));
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

    for (std::size_t i = 0; i < std::size(kBarkSpecs); ++i)
    {
        const PaletteSpec& spec = kBarkSpecs[i];
        const std::filesystem::path path = root / std::filesystem::path(spec.relativePath);
        EnsureTextureFile(path, spec, false);
        bark_[i] = {AssetDatabase::Instance().getOrCreateGuid(path), path, spec.previewColor};
        Tracenf("[TREE-3] palette_loaded bark=%s guid=%s", spec.label, bark_[i].guid.toString().c_str());
    }
    for (std::size_t i = 0; i < std::size(kLeafSpecs); ++i)
    {
        const PaletteSpec& spec = kLeafSpecs[i];
        const std::filesystem::path path = root / std::filesystem::path(spec.relativePath);
        EnsureTextureFile(path, spec, true);
        leaves_[i] = {AssetDatabase::Instance().getOrCreateGuid(path), path, spec.previewColor, spec.gridX, spec.gridY};
        Tracenf("[TREE-3] palette_loaded leaves=%s guid=%s atlas=%dx%d", spec.label,
            leaves_[i].guid.toString().c_str(), spec.gridX, spec.gridY);
    }

    loaded_ = true;
    loadedRoot_ = root;
    Tracenf("[TREE-LEAF-FORM-1] atlas_grid=2x2 cardsPerCluster_default=3");
}
}
