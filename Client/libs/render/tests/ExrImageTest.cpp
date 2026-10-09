#include "asset/ExrImage.h"
#include "asset/AssetDatabase.h"
#include "asset/AssetLibrary.h"
#include "asset/MaterialAssetManager.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfTiledOutputFile.h>
#include <OpenEXR/ImfTileDescription.h>
#include <Imath/half.h>
#include <stb_image.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace
{
namespace exr = OPENEXR_IMF_NAMESPACE;
namespace asset = client::asset;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void Check(const char* name, bool value)
{
    ++checks;
    failures += !value;
    std::cout << "EXR " << name << ": " << (value ? "PASS" : "FAIL") << '\n';
}
struct Workspace
{
    fs::path parent = fs::weakly_canonical(fs::temp_directory_path());
    fs::path root = parent / ("ixw_exr_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Workspace() { if (!fs::create_directory(root)) throw std::runtime_error("cannot create EXR test directory"); }
    ~Workspace()
    {
        if (root.parent_path() == parent && root.filename().string().starts_with("ixw_exr_test_"))
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }
};

const std::vector<float> pixels = {
    4.0f, 0.25f, 0.0625f, 0.5f, 0.125f, 8.0f, 0.5f, 1.0f,
    0.75f, 0.5f, 16.0f, 0.25f, 2.0f, 1.0f, 0.125f, 0.0f};

void Write(const fs::path& path, bool tiled, exr::PixelType type, exr::Compression compression,
           IMATH_NAMESPACE::V2i origin = {0, 0}, bool alpha = true, bool luminance = false, bool layered = false, int width = 2)
{
    exr::Header header(width, 2);
    header.dataWindow() = {origin, {origin.x + width - 1, origin.y + 1}};
    header.compression() = compression;
    const char* names[] = {"R", "G", "B", "A"};
    exr::FrameBuffer buffer;
    std::vector<float> source = pixels;
    if (width == 4) source.insert(source.end(), pixels.begin(), pixels.end());
    const std::vector<IMATH_NAMESPACE::half> halfPixels(source.begin(), source.end());
    const std::size_t sampleBytes = type == exr::HALF ? sizeof(IMATH_NAMESPACE::half) : sizeof(float);
    for (int c = 0; c < (alpha ? 4 : 3); ++c)
    {
        if (luminance && (c == 1 || c == 2)) continue;
        std::string name = luminance && c == 0 ? "Y" : names[c];
        if (layered) name = "beauty." + name;
        header.channels().insert(name, exr::Channel(type));
        const void* samples = type == exr::HALF ? static_cast<const void*>(halfPixels.data() + c) : static_cast<const void*>(source.data() + c);
        buffer.insert(name, exr::Slice::Make(type, samples, header.dataWindow(), 4 * sampleBytes, static_cast<std::size_t>(width) * 4 * sampleBytes));
    }
    if (tiled)
    {
        header.setTileDescription(exr::TileDescription(2, 2, exr::ONE_LEVEL));
        exr::TiledOutputFile output(path.string().c_str(), header);
        output.setFrameBuffer(buffer);
        output.writeTiles(0, output.numXTiles() - 1, 0, output.numYTiles() - 1);
    }
    else
    {
        exr::OutputFile output(path.string().c_str(), header);
        output.setFrameBuffer(buffer);
        output.writePixels(2);
    }
}

std::vector<std::uint8_t> Read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()};
}
}

int main(int argc, char** argv)
{
    try
    {
        // Also permits a persistent fixture for the engine's Vulkan smoke run.
        if (argc >= 2)
        {
            fs::create_directories(fs::path(argv[1]).parent_path());
            Write(argv[1], false, exr::HALF, exr::ZIP_COMPRESSION, {}, true, false, false, argc == 3 ? 4 : 2);
            return 0;
        }
        Workspace workspace;
        std::string error;
        const auto imagePath = workspace.root / "radiance.EXR";
        for (bool tiled : {false, true})
        {
            for (auto type : {exr::HALF, exr::FLOAT})
            {
                for (auto compression : {exr::NO_COMPRESSION, exr::ZIP_COMPRESSION, exr::PIZ_COMPRESSION})
                {
                    Write(imagePath, tiled, type, compression, {-5, 7});
                    auto image = asset::LoadExr(imagePath, error);
                    Check("scanline/tiled HALF/FLOAT compressed image and offset window", image && image->width == 2 && image->height == 2 && image->rgba == pixels);
                }
            }
        }
        Write(imagePath, false, exr::HALF, exr::DWAA_COMPRESSION);
        auto dwa = asset::LoadExr(imagePath, error);
        bool dwaMatches = dwa && dwa->rgba.size() == pixels.size();
        if (dwaMatches)
            for (std::size_t i = 0; i < pixels.size(); ++i)
                dwaMatches &= std::abs(dwa->rgba[i] - pixels[i]) < 0.05f;
        Check("DWAA HDRI compression decodes", dwaMatches);
        Write(imagePath, false, exr::FLOAT, exr::ZIP_COMPRESSION, {12, -9}, false);
        auto rgb = asset::LoadExr(imagePath, error);
        Check("RGB defaults alpha to one", rgb && rgb->rgba[3] == 1.0f && rgb->rgba[15] == 1.0f && rgb->rgba[10] == 16.0f);
        Write(imagePath, true, exr::HALF, exr::ZIP_COMPRESSION, {}, false, true);
        auto gray = asset::LoadExr(imagePath, error);
        Check("Y luminance replicated to RGB", gray && gray->rgba[0] == 4.0f && gray->rgba[1] == 4.0f && gray->rgba[2] == 4.0f && gray->rgba[3] == 1.0f);
        Write(imagePath, false, exr::FLOAT, exr::ZIP_COMPRESSION, {}, true, false, true);
        Check("layered EXR rejected with explanation", !asset::LoadExr(imagePath, error) && error.find("channels") != std::string::npos);
        Write(imagePath, false, exr::HALF, exr::ZIP_COMPRESSION);
        auto encoded = Read(imagePath);
        Check("EXR magic and case-insensitive extension", asset::IsExr(encoded) && asset::IsExrPath(imagePath));
        int width = 0, height = 0;
        Check("header-only metadata resolution", asset::ReadExrResolution(imagePath, width, height, error) && width == 2 && height == 2);
        auto image = asset::DecodeExr(encoded, error);
        Check("memory decode matches file decode", image && image->rgba == pixels);
        Check("empty image rejected", !asset::DecodeExr({}, error) && !error.empty());
        Check("truncated header rejected", !asset::DecodeExr(std::span(encoded).first(16), error) && !error.empty());
        Check("truncated pixel payload rejected", !asset::DecodeExr(std::span(encoded).first(encoded.size() - 8), error) && !error.empty());
        auto deep = encoded;
        deep[5] |= 0x08; // EXR deep-data flag (0x800).
        Check("deep EXR rejected", !asset::DecodeExr(deep, error) && !error.empty());
        auto multipart = encoded;
        multipart[5] |= 0x10; // EXR multipart flag (0x1000).
        Check("multipart EXR rejected", !asset::DecodeExr(multipart, error) && !error.empty());
        const auto hugePath = workspace.root / "huge.exr";
        {
            exr::Header header(16385, 1);
            for (const char* name : {"R", "G", "B"}) header.channels().insert(name, exr::Channel(exr::FLOAT));
            exr::OutputFile output(hugePath.string().c_str(), header);
        }
        Check("oversized header rejected before pixel allocation", !asset::LoadExr(hugePath, error) && error.find("budget") != std::string::npos);
        asset::ExrImage values{1, 1, {4.0f, 0.25f, -0.5f, 0.5f}};
        const auto halfBytes = asset::ExrHalfPixels(values);
        std::uint16_t halfBits = 0;
        std::memcpy(&halfBits, halfBytes.data(), sizeof(halfBits));
        IMATH_NAMESPACE::half halfValue;
        halfValue.setBits(halfBits);
        Check("GPU half payload retains HDR intensity", halfBytes.size() == 8 && static_cast<float>(halfValue) == 4.0f);
        std::memcpy(&halfBits, halfBytes.data() + 4, sizeof(halfBits));
        halfValue.setBits(halfBits);
        Check("GPU half retains signed linear samples", static_cast<float>(halfValue) == -0.5f);
        const auto preview = asset::ExrRgba8(values, asset::ExrByteMode::Preview);
        const auto data = asset::ExrRgba8(values, asset::ExrByteMode::LinearData);
        const auto color = asset::ExrRgba8(values, asset::ExrByteMode::SrgbColor);
        Check("preview tone maps RGB while alpha stays linear", preview[0] == 231 && preview[1] == 124 && preview[2] == 0 && preview[3] == 128);
        Check("data map avoids gamma and tone mapping", data == std::vector<std::uint8_t>({255, 64, 0, 128}));
        Check("normalized color atlas encodes sRGB without exposure", color == std::vector<std::uint8_t>({255, 137, 0, 128}));
        asset::ExrImage invalid{1, 1, {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 1e20f, 1.0f}};
        Check("non-finite preview samples are safe", asset::ExrRgba8(invalid, asset::ExrByteMode::Preview)[0] == 0);
        const auto bounded = asset::ExrHalfPixels(invalid);
        std::memcpy(&halfBits, bounded.data() + 4, sizeof(halfBits));
        halfValue.setBits(halfBits);
        Check("half overflow is bounded", static_cast<float>(halfValue) == 65504.0f);
        Check("asset database recognizes EXR", detectAssetType(imagePath) == AssetType::Texture);
        const fs::path project = workspace.root / "project";
        fs::create_directories(project);
        AssetLibrary library(project, project / "Assets");
        Check("asset library initializes", library.Initialize());
        AssetLibrary::Entry entry;
        Check("EXR import succeeds", library.Import(AssetLibrary::Category::Texture, imagePath, entry, error));
        Check("import preserves original EXR bytes", Read(library.AbsolutePath(entry)) == encoded);
        Check("import stores dimensions", entry.resolutionWidth == 2 && entry.resolutionHeight == 2);
        int channels = 0;
        stbi_uc* thumbnail = stbi_load((project / "Assets" / entry.thumbnail).string().c_str(), &width, &height, &channels, 4);
        Check("import writes actual tone-mapped thumbnail", thumbnail && width == 2 && height == 2 && thumbnail[0] == 231 && thumbnail[3] == 128);
        stbi_image_free(thumbnail);
        AssetLibrary reloaded(project, project / "Assets");
        Check("EXR survives manifest reload", reloaded.InitializeReadOnly() && reloaded.FindById(entry.id).has_value());
        const auto brokenPath = workspace.root / "broken.exr";
        std::ofstream(brokenPath, std::ios::binary) << "invalid";
        Check("invalid EXR import fails with error", !library.Import(AssetLibrary::Category::Texture, brokenPath, entry, error) && !error.empty());

        // ORM packing is deliberately normalized linear data, including when its sources are EXR.
        auto& database = AssetDatabase::Instance();
        database.scan(project);
        MaterialAsset material;
        material.path = project / "Assets" / "surface.material";
        material.aoTexture = database.getOrCreateGuid(imagePath);
        material.roughnessTexture = material.aoTexture;
        auto packedGuid = MaterialAssetManager::Instance().packOcclusionRoughnessMetallic(material);
        const auto packedPath = packedGuid ? database.resolveGuid(*packedGuid) : std::nullopt;
        stbi_uc* packed = packedPath ? stbi_load(packedPath->string().c_str(), &width, &height, &channels, 4) : nullptr;
        Check("EXR ORM sources pack as linear data", packed && packed[4] == 32 && packed[5] == 32 && packed[6] == 255);
        stbi_image_free(packed);
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        ++failures;
    }
    std::cout << "EXR checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
