#include "ExrImage.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfTestFile.h>
#include <Imath/half.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace client::asset
{
namespace
{
class MemoryStream final : public OPENEXR_IMF_NAMESPACE::IStream
{
public:
    explicit MemoryStream(std::span<const std::uint8_t> bytes) : IStream("asset.exr"), bytes_(bytes) {}
    bool read(char* destination, int count) override
    {
        if (count < 0 || static_cast<std::size_t>(count) > bytes_.size() - position_)
            throw std::runtime_error("truncated EXR data");
        std::memcpy(destination, bytes_.data() + position_, static_cast<std::size_t>(count));
        position_ += static_cast<std::size_t>(count);
        return position_ < bytes_.size();
    }
    std::uint64_t tellg() override { return position_; }
    void seekg(std::uint64_t position) override
    {
        if (position > bytes_.size()) throw std::runtime_error("invalid EXR offset");
        position_ = static_cast<std::size_t>(position);
    }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
};
}

bool IsExr(std::span<const std::uint8_t> bytes)
{
    return bytes.size() >= 4 && bytes[0] == 0x76 && bytes[1] == 0x2f && bytes[2] == 0x31 && bytes[3] == 0x01;
}

bool IsExrPath(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".exr";
}

static std::optional<ExrImage> DecodeExrImpl(std::span<const std::uint8_t> bytes, std::string& error, bool headerOnly)
{
    error.clear();
    try
    {
        if (!IsExr(bytes)) throw std::runtime_error("not an EXR image");
        MemoryStream stream(bytes);
        bool tiled = false, deep = false, multipart = false;
        if (!OPENEXR_IMF_NAMESPACE::isOpenExrFile(stream, tiled, deep, multipart))
            throw std::runtime_error("invalid EXR header");
        if (deep || multipart) throw std::runtime_error("deep and multipart EXR images are not supported; export a flat RGB/RGBA image");
        stream.seekg(0);
        OPENEXR_IMF_NAMESPACE::InputFile file(stream, 0);
        const auto& window = file.header().dataWindow();
        const std::int64_t width = static_cast<std::int64_t>(window.max.x) - window.min.x + 1;
        const std::int64_t height = static_cast<std::int64_t>(window.max.y) - window.min.y + 1;
        // Bound the decoded allocation to 512 MiB, independently of compressed file size.
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || width * height > 33554432)
            throw std::runtime_error("EXR dimensions exceed the texture decode budget");
        const auto& channels = file.header().channels();
        const bool rgb = channels.findChannel("R") && channels.findChannel("G") && channels.findChannel("B");
        const bool luminance = !rgb && channels.findChannel("Y");
        if (!rgb && !luminance) throw std::runtime_error("EXR needs unlayered R/G/B or Y channels");
        ExrImage image;
        image.width = static_cast<int>(width);
        image.height = static_cast<int>(height);
        if (headerOnly) return image;
        image.rgba.resize(static_cast<std::size_t>(width * height) * 4u);
        OPENEXR_IMF_NAMESPACE::FrameBuffer buffer;
        const char* names[] = {"R", "G", "B", "A"};
        for (int c = 0; c < 4; ++c)
        {
            if (luminance && (c == 1 || c == 2)) continue;
            const char* name = luminance && c == 0 ? "Y" : names[c];
            if (const auto* channel = channels.findChannel(name); channel && (channel->xSampling != 1 || channel->ySampling != 1))
                throw std::runtime_error("subsampled EXR channels are not supported");
            buffer.insert(name, OPENEXR_IMF_NAMESPACE::Slice::Make(OPENEXR_IMF_NAMESPACE::FLOAT,
                image.rgba.data() + c, window, sizeof(float) * 4u, sizeof(float) * 4u * static_cast<std::size_t>(width),
                1, 1, c == 3 ? 1.0 : 0.0));
        }
        file.setFrameBuffer(buffer);
        file.readPixels(window.min.y, window.max.y);
        for (std::size_t i = 0; i < image.rgba.size(); i += 4)
        {
            if (luminance) image.rgba[i + 1] = image.rgba[i + 2] = image.rgba[i];
            for (int c = 0; c < 4; ++c)
                if (!std::isfinite(image.rgba[i + c])) image.rgba[i + c] = 0.0f;
        }
        return image;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return std::nullopt;
    }
}

std::optional<ExrImage> DecodeExr(std::span<const std::uint8_t> bytes, std::string& error)
{
    return DecodeExrImpl(bytes, error, false);
}

bool ReadExrResolution(const std::filesystem::path& path, int& width, int& height, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot open EXR: " + path.generic_string(); return false; }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto image = DecodeExrImpl(bytes, error, true);
    if (!image) return false;
    width = image->width;
    height = image->height;
    return true;
}

std::optional<ExrImage> LoadExr(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot open EXR: " + path.generic_string(); return std::nullopt; }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return DecodeExr(bytes, error);
}

std::vector<std::uint8_t> ExrHalfPixels(const ExrImage& image)
{
    std::vector<std::uint8_t> pixels(image.rgba.size() * sizeof(std::uint16_t));
    for (std::size_t i = 0; i < image.rgba.size(); ++i)
    {
        const float value = std::isfinite(image.rgba[i]) ? std::clamp(image.rgba[i], -65504.0f, 65504.0f) : 0.0f;
        const std::uint16_t bits = IMATH_NAMESPACE::half(value).bits();
        std::memcpy(pixels.data() + i * sizeof(bits), &bits, sizeof(bits));
    }
    return pixels;
}

std::vector<std::uint8_t> ExrRgba8(const ExrImage& image, ExrByteMode mode)
{
    std::vector<std::uint8_t> pixels(image.rgba.size());
    for (std::size_t i = 0; i < pixels.size(); ++i)
    {
        float value = std::isfinite(image.rgba[i]) ? std::max(0.0f, image.rgba[i]) : 0.0f;
        if (mode != ExrByteMode::LinearData && i % 4u != 3u)
        {
            value = mode == ExrByteMode::Preview ? value / (1.0f + value) : std::min(value, 1.0f);
            value = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
        }
        pixels[i] = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }
    return pixels;
}
}
