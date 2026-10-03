#include "SkyRenderer.h"

#include "Debug.h"
#include "IXRHIShader.h"
#include "asset/IAssetReader.h"
#include "math/IXMath.h"
#include "math/WorldMath.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

namespace
{
namespace xm = ixtreeme::math;

// Larger images are halved until they fit: a 4096 x 2048 HDR panorama is already 64 MB on the GPU.
constexpr int kMaxPanoramaWidth = 4096;
constexpr int kMaxCubeFaceSize = 2048;

std::vector<std::uint32_t> ReadSpirv(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0)
    {
        Tracenf("[SKY] failed to open shader: %s", path.c_str());
        return {};
    }
    const auto* words = reinterpret_cast<const std::uint32_t*>(bytes->data());
    return std::vector<std::uint32_t>(words, words + bytes->size() / sizeof(std::uint32_t));
}

std::shared_ptr<ixrhi::IXRHIShader> LoadShader(ixrhi::IXRHIDevice& rhi,
                                               client::asset::IAssetReader& assets,
                                               const std::string& path,
                                               ixrhi::IXRHIShaderStage stage,
                                               const char* entry)
{
    ixrhi::IXRHIShaderDesc desc;
    desc.stage = stage;
    desc.entryPoint = entry;
    desc.spirv = ReadSpirv(assets, path);
    if (desc.spirv.empty())
        return nullptr;
    desc.debugName = path;
    return rhi.CreateShader(desc);
}

float SrgbToLinear(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

void SrgbToLinear3(const float* srgb, float* linear)
{
    for (int i = 0; i < 3; ++i)
        linear[i] = SrgbToLinear(srgb[i]);
}

const std::array<float, 256>& SrgbByteToLinearTable()
{
    static const std::array<float, 256> table = [] {
        std::array<float, 256> values{};
        for (int i = 0; i < 256; ++i)
            values[static_cast<std::size_t>(i)] = SrgbToLinear(static_cast<float>(i) / 255.0f);
        return values;
    }();
    return table;
}

std::uint16_t FloatToHalf(float value)
{
    value = std::clamp(value, 0.0f, 65504.0f);  // sky radiance: never negative, never infinite
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t mantissa = bits & 0x007fffffu;
    const int exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
    if (exponent <= 0)
    {
        if (exponent < -10)
            return 0;
        const std::uint32_t full = mantissa | 0x00800000u;
        const int shift = 14 - exponent;
        std::uint32_t half = full >> shift;
        if ((full >> (shift - 1)) & 1u)
            ++half;
        return static_cast<std::uint16_t>(half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u)
        ++half;  // round; a carry into the exponent is still the right value
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(half, 0x7bffu));
}

// A decoded RGBA image: linear floats for .hdr, sRGB bytes otherwise.
struct SkyImage
{
    int width = 0;
    int height = 0;
    bool hdr = false;
    std::vector<float> linear;        // hdr: RGBA
    std::vector<std::uint8_t> srgb;   // ldr: RGBA
};

std::string LowerExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

std::filesystem::path ResolveImagePath(const std::string& stored, const std::filesystem::path& projectRoot)
{
    const std::filesystem::path path(stored);
    if (path.is_absolute() || projectRoot.empty())
        return path;
    return projectRoot / path;
}

std::optional<SkyImage> LoadSkyImage(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "image not found: " + path.filename().string();
        return std::nullopt;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        error = "empty image: " + path.filename().string();
        return std::nullopt;
    }

    SkyImage image;
    int channels = 0;
    const int size = static_cast<int>(bytes.size());
    if (LowerExtension(path) == ".hdr" || stbi_is_hdr_from_memory(bytes.data(), size))
    {
        float* pixels = stbi_loadf_from_memory(bytes.data(), size, &image.width, &image.height, &channels, 4);
        if (!pixels)
        {
            error = "cannot read " + path.filename().string() + " (" + stbi_failure_reason() + ")";
            return std::nullopt;
        }
        image.hdr = true;
        image.linear.assign(pixels, pixels + static_cast<std::size_t>(image.width) * image.height * 4u);
        stbi_image_free(pixels);
    }
    else
    {
        stbi_uc* pixels = stbi_load_from_memory(bytes.data(), size, &image.width, &image.height, &channels, 4);
        if (!pixels)
        {
            error = "cannot read " + path.filename().string() + " (PNG, JPG, TGA or HDR)";
            return std::nullopt;
        }
        image.srgb.assign(pixels, pixels + static_cast<std::size_t>(image.width) * image.height * 4u);
        stbi_image_free(pixels);
    }
    if (image.width <= 0 || image.height <= 0)
    {
        error = "empty image: " + path.filename().string();
        return std::nullopt;
    }
    return image;
}

void ToLinearFloat(SkyImage& image)
{
    if (image.hdr)
        return;
    const auto& table = SrgbByteToLinearTable();
    image.linear.resize(image.srgb.size());
    for (std::size_t i = 0; i < image.srgb.size(); ++i)
        image.linear[i] = (i % 4u == 3u) ? image.srgb[i] / 255.0f : table[image.srgb[i]];
    image.srgb.clear();
    image.hdr = true;
}

// Box-filters the image down to half size (odd edges drop their last row / column).
void HalveImage(SkyImage& image)
{
    const int width = std::max(1, image.width / 2);
    const int height = std::max(1, image.height / 2);
    const auto sample = [&](int x, int y, int c) -> float {
        x = std::min(x, image.width - 1);
        y = std::min(y, image.height - 1);
        const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 4u + c;
        return image.hdr ? image.linear[index] : static_cast<float>(image.srgb[index]);
    };
    std::vector<float> linear;
    std::vector<std::uint8_t> srgb;
    if (image.hdr)
        linear.resize(static_cast<std::size_t>(width) * height * 4u);
    else
        srgb.resize(static_cast<std::size_t>(width) * height * 4u);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            for (int c = 0; c < 4; ++c)
            {
                const float value = 0.25f * (sample(2 * x, 2 * y, c) + sample(2 * x + 1, 2 * y, c) +
                    sample(2 * x, 2 * y + 1, c) + sample(2 * x + 1, 2 * y + 1, c));
                const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4u + c;
                if (image.hdr)
                    linear[index] = value;
                else
                    srgb[index] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            }
        }
    }
    image.width = width;
    image.height = height;
    image.linear = std::move(linear);
    image.srgb = std::move(srgb);
}

// Appends the pixels in the upload format: half floats for HDR, sRGB bytes otherwise.
void AppendPixels(const SkyImage& image, std::vector<std::uint8_t>& out)
{
    if (!image.hdr)
    {
        out.insert(out.end(), image.srgb.begin(), image.srgb.end());
        return;
    }
    const std::size_t start = out.size();
    out.resize(start + image.linear.size() * sizeof(std::uint16_t));
    for (std::size_t i = 0; i < image.linear.size(); ++i)
    {
        const std::uint16_t half = FloatToHalf(image.linear[i]);
        std::memcpy(out.data() + start + i * sizeof(std::uint16_t), &half, sizeof(half));
    }
}

// Average linear colour. rowWeight(y) weighs rows (solid angle of a panorama row); null = even.
std::array<float, 3> AverageLinear(const SkyImage& image, float (*rowWeight)(int y, int height))
{
    const auto& table = SrgbByteToLinearTable();
    double sum[3] = {0.0, 0.0, 0.0};
    double total = 0.0;
    for (int y = 0; y < image.height; ++y)
    {
        const double weight = rowWeight ? rowWeight(y, image.height) : 1.0;
        for (int x = 0; x < image.width; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 4u;
            for (int c = 0; c < 3; ++c)
                sum[c] += weight * (image.hdr ? image.linear[index + c] : table[image.srgb[index + c]]);
            total += weight;
        }
    }
    if (total <= 0.0)
        return {};
    return {static_cast<float>(sum[0] / total), static_cast<float>(sum[1] / total), static_cast<float>(sum[2] / total)};
}

float PanoramaRowWeight(int y, int height)
{
    // Rows near the poles cover less of the sphere: weight by sin(polar angle).
    return std::sin((static_cast<float>(y) + 0.5f) / static_cast<float>(height) * xm::Pi);
}

// General 4x4 inverse (cofactor expansion) of a flat 16-float matrix; layout-agnostic.
bool InvertMatrix(const float* m, float* out)
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-30f)
        return false;
    const float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] * invDet;
    return true;
}

std::shared_ptr<ixrhi::IXRHITexture> CreateFallbackTexture(ixrhi::IXRHIDevice& rhi, bool cube)
{
    const std::uint32_t layers = cube ? 6u : 1u;
    const std::vector<std::uint8_t> black(4u * layers, 0u);
    ixrhi::IXRHITextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.arrayLayers = layers;
    desc.cubeMap = cube;
    desc.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    desc.usage = ixrhi::IXRHITextureUsage::Sampled;
    desc.debugName = cube ? "Sky:FallbackCube" : "Sky:FallbackPanorama";
    return rhi.CreateTexture(desc, black.data(), black.size());
}
} // namespace

bool SkyRenderer::Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets)
{
    Destroy();
    m_rhi = &rhi;
    m_assets = &assets;

    m_vertexShader = LoadShader(rhi, assets, "assets/shaders/sky_vs.spv", ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    m_fragmentShader = LoadShader(rhi, assets, "assets/shaders/sky_ps.spv", ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    const bool shaders = m_vertexShader && m_fragmentShader;
    const bool resources = shaders && CreateResources(rhi);
    if (resources)
        m_pipeline = BuildPipeline(rhi, m_targetPass);
    Tracenf("[SKY] Create: shaders=%d resources=%d pipeline=%d",
        shaders ? 1 : 0, resources ? 1 : 0, m_pipeline ? 1 : 0);
    if (resources && m_pipeline)
        return true;
    Destroy();
    return false;
}

bool SkyRenderer::RecreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_rhi)
        return true;
    m_rhi = &rhi;
    m_pipeline.reset();
    m_extraPipeline.reset();
    m_extraPass = nullptr;
    // Null while the target pass is torn down: retried on the next RecreatePipeline.
    m_pipeline = BuildPipeline(rhi, m_targetPass);
    return true;
}

bool SkyRenderer::CreateResources(ixrhi::IXRHIDevice& rhi)
{
    for (auto& buffer : m_uniformBuffers)
    {
        ixrhi::IXRHIBufferDesc desc;
        desc.sizeBytes = sizeof(UniformBlock);
        desc.usage = ixrhi::IXRHIBufferUsage::Uniform;
        desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        desc.debugName = "Sky UBO";
        buffer = rhi.CreateBuffer(desc, nullptr, 0);
        if (!buffer)
            return false;
    }

    ixrhi::IXRHISamplerDesc cubeSampler;
    cubeSampler.debugName = "Sky:CubeSampler";
    m_cubeSampler = rhi.CreateSampler(cubeSampler);
    ixrhi::IXRHISamplerDesc panoramaSampler;
    panoramaSampler.addressU = ixrhi::IXRHISamplerAddress::Repeat;  // longitude wraps around
    panoramaSampler.debugName = "Sky:PanoramaSampler";
    m_panoramaSampler = rhi.CreateSampler(panoramaSampler);
    m_fallbackCube = CreateFallbackTexture(rhi, true);
    m_fallbackPanorama = CreateFallbackTexture(rhi, false);
    if (!m_cubeSampler || !m_panoramaSampler || !m_fallbackCube || !m_fallbackPanorama)
        return false;

    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::UniformBuffer, ixrhi::IXRHIShaderStage::Fragment},
        {1, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
        {2, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
    };
    m_bindLayout = rhi.CreateBindGroupLayout(bindings);
    if (!m_bindLayout)
        return false;
    m_bindGroup = rhi.CreateBindGroup(*m_bindLayout, kFramesInFlight);
    if (!m_bindGroup)
        return false;
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
        m_bindGroup->UpdateBuffer(i, 0, m_uniformBuffers[i], 0, sizeof(UniformBlock));
    BindTextures();
    return true;
}

std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> SkyRenderer::BuildPipeline(ixrhi::IXRHIDevice& rhi,
    const ixrhi::IXRHIRenderPass* pass)
{
    if (!m_vertexShader || !m_fragmentShader || !m_bindLayout)
        return nullptr;
    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = m_vertexShader;
    desc.fragmentShader = m_fragmentShader;
    desc.bindGroupLayouts = {m_bindLayout.get()};
    desc.pushRanges = {{ixrhi::IXRHIShaderStage::Fragment, 0, sizeof(float) * 16}};
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.depthTestEnable = true;
    desc.depthWriteEnable = false;
    desc.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    desc.blendAttachments = {ixrhi::IXRHIBlendAttachment{}};
    desc.sampleCount = 1;
    desc.targetRenderPass = pass;
    desc.debugName = "Sky";
    return rhi.CreateGraphicsPipeline(desc);
}

void SkyRenderer::BindTextures()
{
    if (!m_bindGroup)
        return;
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        m_bindGroup->UpdateTexture(i, 1, m_cube ? m_cube : m_fallbackCube, m_cubeSampler);
        m_bindGroup->UpdateTexture(i, 2, m_panorama ? m_panorama : m_fallbackPanorama, m_panoramaSampler);
    }
}

void SkyRenderer::LoadImages(const SkySettings& sky, const std::filesystem::path& projectRoot)
{
    // Only the image the current mode shows is kept; switching modes reloads (it is quick to do and
    // keeps a set-but-unused 64 MB panorama off the GPU).
    const bool wantCube = sky.mode == SkySettings::Mode::Cubemap;
    const bool wantPanorama = sky.mode == SkySettings::Mode::Panorama;
    const bool cubeChanged = wantCube != m_cubeRequested ||
        (wantCube && (sky.cubeFacePaths != m_loadedCubePaths || projectRoot != m_loadedRoot));
    const bool panoramaChanged = wantPanorama != m_panoramaRequested ||
        (wantPanorama && (sky.panoramaPath != m_loadedPanoramaPath || projectRoot != m_loadedRoot));
    if (!cubeChanged && !panoramaChanged)
        return;

    std::shared_ptr<ixrhi::IXRHITexture> cube = cubeChanged ? nullptr : m_cube;
    std::shared_ptr<ixrhi::IXRHITexture> panorama = panoramaChanged ? nullptr : m_panorama;
    if (cubeChanged)
    {
        m_cubeStatus.clear();
        m_cubeAverage = {};
        if (wantCube)
        {
            const std::ptrdiff_t setFaces = std::count_if(sky.cubeFacePaths.begin(), sky.cubeFacePaths.end(),
                [](const std::string& path) { return !path.empty(); });
            std::array<SkyImage, SkySettings::kCubeFaces> faces;
            bool ok = setFaces == static_cast<std::ptrdiff_t>(SkySettings::kCubeFaces);
            if (!ok)
                m_cubeStatus = "Set all six faces (" + std::to_string(setFaces) + " of 6 set)";
            bool anyHdr = false;
            for (std::size_t face = 0; ok && face < faces.size(); ++face)
            {
                std::string error;
                auto image = LoadSkyImage(ResolveImagePath(sky.cubeFacePaths[face], projectRoot), error);
                if (!image)
                {
                    m_cubeStatus = error;
                    ok = false;
                    break;
                }
                faces[face] = std::move(*image);
                anyHdr |= faces[face].hdr;
                if (faces[face].width != faces[face].height || faces[face].width != faces[0].width)
                {
                    m_cubeStatus = "The six faces must be square images of the same size";
                    ok = false;
                }
            }
            if (ok)
            {
                std::vector<std::uint8_t> pixels;
                double sum[3] = {0.0, 0.0, 0.0};
                for (SkyImage& face : faces)
                {
                    if (anyHdr)
                        ToLinearFloat(face);  // one format for all six layers
                    while (face.width > kMaxCubeFaceSize)
                        HalveImage(face);
                    const std::array<float, 3> average = AverageLinear(face, nullptr);
                    for (int c = 0; c < 3; ++c)
                        sum[c] += average[c] / static_cast<double>(faces.size());
                    AppendPixels(face, pixels);
                }
                ixrhi::IXRHITextureDesc desc;
                desc.width = static_cast<std::uint32_t>(faces[0].width);
                desc.height = desc.width;
                desc.arrayLayers = 6;
                desc.cubeMap = true;
                desc.format = anyHdr ? ixrhi::IXRHIFormat::R16G16B16A16Float : ixrhi::IXRHIFormat::R8G8B8A8Srgb;
                desc.usage = ixrhi::IXRHITextureUsage::Sampled;
                desc.debugName = "Sky:Cube";
                cube = m_rhi->CreateTexture(desc, pixels.data(), pixels.size());
                if (cube)
                {
                    m_cubeAverage = {static_cast<float>(sum[0]), static_cast<float>(sum[1]), static_cast<float>(sum[2])};
                    Tracenf("[SKY] cube map loaded: %ux%u per face, %s", desc.width, desc.width, anyHdr ? "HDR" : "sRGB");
                }
                else
                {
                    m_cubeStatus = "The cube map could not be created on the GPU";
                }
            }
        }
        m_loadedCubePaths = sky.cubeFacePaths;
        m_cubeRequested = wantCube;
    }
    if (panoramaChanged)
    {
        m_panoramaStatus.clear();
        m_panoramaAverage = {};
        if (wantPanorama)
        {
            std::string error;
            std::optional<SkyImage> image;
            if (sky.panoramaPath.empty())
                m_panoramaStatus = "Drop a panorama image (PNG, JPG or HDR) on the Panorama slot";
            else if (!(image = LoadSkyImage(ResolveImagePath(sky.panoramaPath, projectRoot), error)))
                m_panoramaStatus = error;
            if (image)
            {
                while (image->width > kMaxPanoramaWidth)
                    HalveImage(*image);
                m_panoramaAverage = AverageLinear(*image, &PanoramaRowWeight);
                std::vector<std::uint8_t> pixels;
                AppendPixels(*image, pixels);
                ixrhi::IXRHITextureDesc desc;
                desc.width = static_cast<std::uint32_t>(image->width);
                desc.height = static_cast<std::uint32_t>(image->height);
                desc.format = image->hdr ? ixrhi::IXRHIFormat::R16G16B16A16Float : ixrhi::IXRHIFormat::R8G8B8A8Srgb;
                desc.usage = ixrhi::IXRHITextureUsage::Sampled;
                desc.debugName = "Sky:Panorama";
                panorama = m_rhi->CreateTexture(desc, pixels.data(), pixels.size());
                if (panorama)
                    Tracenf("[SKY] panorama loaded: %ux%u %s", desc.width, desc.height, image->hdr ? "HDR" : "sRGB");
                else
                    m_panoramaStatus = "The panorama could not be created on the GPU";
            }
        }
        m_loadedPanoramaPath = sky.panoramaPath;
        m_panoramaRequested = wantPanorama;
    }
    m_loadedRoot = projectRoot;
    if (!m_cubeStatus.empty())
        Tracenf("[SKY] cube map: %s", m_cubeStatus.c_str());
    if (!m_panoramaStatus.empty())
        Tracenf("[SKY] panorama: %s", m_panoramaStatus.c_str());

    // The descriptor sets of frames still in flight reference the old images: let them finish
    // before the sets are rewritten and the old images are released.
    m_rhi->WaitIdle();
    m_cube = std::move(cube);
    m_panorama = std::move(panorama);
    BindTextures();
}

void SkyRenderer::Update(const ixrhi::IXRHIFrameInfo& frame,
                         const SkySettings& sky,
                         const LightingState& lighting,
                         const std::filesystem::path& projectRoot)
{
    if (!m_rhi || !frame.frameActive)
        return;
    LoadImages(sky, projectRoot);

    // An image sky whose image is not there yet shows the procedural sky instead of black.
    SkySettings::Mode mode = sky.mode;
    m_status.clear();
    if (mode == SkySettings::Mode::Cubemap && !m_cube)
    {
        m_status = m_cubeStatus;
        mode = SkySettings::Mode::Procedural;
    }
    else if (mode == SkySettings::Mode::Panorama && !m_panorama)
    {
        m_status = m_panoramaStatus;
        mode = SkySettings::Mode::Procedural;
    }

    UniformBlock uniform{};
    SrgbToLinear3(sky.zenithColor, uniform.zenith);
    SrgbToLinear3(sky.horizonColor, uniform.horizon);
    SrgbToLinear3(sky.groundColor, uniform.ground);
    SrgbToLinear3(sky.color, uniform.color);
    uniform.zenith[3] = static_cast<float>(static_cast<std::int32_t>(mode));
    const float exposure = std::max(0.0f, sky.exposure);
    uniform.horizon[3] = exposure;
    uniform.ground[3] = xm::DegreesToRadians(sky.rotationDegrees);

    const DirectionalLight& sun = lighting.directional;
    const WorldVec3 sunDir = WorldDirectionFromAzimuthElevation(
        xm::DegreesToRadians(sun.azimuthDegrees),
        xm::DegreesToRadians(std::clamp(sun.elevationDegrees, -90.0f, 90.0f)));
    uniform.sunDir[0] = sunDir.x;
    uniform.sunDir[1] = sunDir.y;
    uniform.sunDir[2] = sunDir.z;
    uniform.sunDir[3] = sky.sunSizeDegrees > 0.0f
        ? std::cos(xm::DegreesToRadians(std::clamp(sky.sunSizeDegrees, 0.0f, 20.0f) * 0.5f))
        : 2.0f;  // > 1: no disc
    // The sun's colour, dimmed when the light is weak but never brighter than the colour itself:
    // scenes run their Sun at intensities up to 5, which would wash the whole sky out.
    const float sunIntensity = std::clamp(sun.intensity, 0.0f, 1.0f);
    uniform.sunColor[0] = std::max(0.0f, sun.r) * sunIntensity;
    uniform.sunColor[1] = std::max(0.0f, sun.g) * sunIntensity;
    uniform.sunColor[2] = std::max(0.0f, sun.b) * sunIntensity;
    uniform.sunColor[3] = std::max(0.0f, sky.sunGlow);
    uniform.tint[0] = std::max(0.0f, sky.tint[0]);
    uniform.tint[1] = std::max(0.0f, sky.tint[1]);
    uniform.tint[2] = std::max(0.0f, sky.tint[2]);
    uniform.tint[3] = sun.enabled ? 1.0f : 0.0f;

    const std::uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    if (m_uniformBuffers[frameIndex])
        m_uniformBuffers[frameIndex]->Write(0, &uniform, sizeof(uniform));

    // The average colour, matching what the shader draws (mode after the fallback above).
    std::array<float, 3> average{};
    switch (mode)
    {
    case SkySettings::Mode::Color:
        average = {uniform.color[0], uniform.color[1], uniform.color[2]};
        break;
    case SkySettings::Mode::Cubemap:
        average = m_cubeAverage;
        break;
    case SkySettings::Mode::Panorama:
        average = m_panoramaAverage;
        break;
    case SkySettings::Mode::Procedural:
    {
        const float day = std::clamp((sunDir.y + 0.18f) / 0.36f, 0.0f, 1.0f);
        const float night[3] = {0.02f, 0.025f, 0.05f};
        for (int c = 0; c < 3; ++c)
        {
            const float lit = 0.5f * uniform.zenith[c] + 0.35f * uniform.horizon[c] + 0.15f * uniform.ground[c];
            average[static_cast<std::size_t>(c)] = uniform.zenith[c] * night[c] + (lit - uniform.zenith[c] * night[c]) * day;
        }
        break;
    }
    }
    if (mode != SkySettings::Mode::Color)
    {
        for (std::size_t c = 0; c < 3; ++c)
            average[c] *= exposure * uniform.tint[c];
    }
    m_averageColor = average;
}

void SkyRenderer::Render(ixrhi::IXRHICommandList& cmd,
                         const ixrhi::IXRHIFrameInfo& frame,
                         const WorldCamera& camera,
                         std::uint32_t targetWidth,
                         std::uint32_t targetHeight,
                         const ixrhi::IXRHIRenderPass* renderPass)
{
    if (!m_rhi || !m_bindGroup || !frame.frameActive)
        return;
    const std::uint32_t width = targetWidth > 0 ? targetWidth : frame.targetWidth;
    const std::uint32_t height = targetHeight > 0 ? targetHeight : frame.targetHeight;
    if (width == 0 || height == 0)
        return;

    const ixrhi::IXRHIGraphicsPipeline* pipeline = m_pipeline.get();
    if (renderPass != nullptr && renderPass != m_targetPass)
    {
        if (!m_extraPipeline || m_extraPass != renderPass)
        {
            m_extraPipeline = BuildPipeline(*m_rhi, renderPass);
            m_extraPass = m_extraPipeline ? renderPass : nullptr;
            Tracenf("[SKY] pipeline for an extra pass (water reflection): %s", m_extraPipeline ? "ok" : "FAILED");
        }
        pipeline = m_extraPipeline.get();
    }
    if (!pipeline)
        return;

    float invViewProjection[16];
    if (!InvertMatrix(camera.viewProjection.m, invViewProjection))
        return;

    const std::uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    cmd.SetScissor(0, 0, width, height);
    cmd.SetGraphicsPipeline(*pipeline);
    cmd.BindGroup(0, *m_bindGroup, frameIndex);
    cmd.PushConstants(invViewProjection, sizeof(invViewProjection));
    cmd.Draw(3, 1, 0, 0);
}

void SkyRenderer::Destroy()
{
    m_pipeline.reset();
    m_extraPipeline.reset();
    m_extraPass = nullptr;
    m_bindGroup.reset();
    m_bindLayout.reset();
    for (auto& buffer : m_uniformBuffers)
        buffer.reset();
    m_cube.reset();
    m_panorama.reset();
    m_fallbackCube.reset();
    m_fallbackPanorama.reset();
    m_cubeSampler.reset();
    m_panoramaSampler.reset();
    m_vertexShader.reset();
    m_fragmentShader.reset();
    m_loadedCubePaths = {};
    m_loadedPanoramaPath.clear();
    m_loadedRoot.clear();
    m_cubeRequested = false;
    m_panoramaRequested = false;
    m_rhi = nullptr;
    m_assets = nullptr;
}
