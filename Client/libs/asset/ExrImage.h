#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace client::asset
{
// EXR samples are scene-linear. No gamma or exposure is applied when decoding.
struct ExrImage
{
    int width = 0;
    int height = 0;
    std::vector<float> rgba;
};

bool IsExr(std::span<const std::uint8_t> bytes);
bool IsExrPath(const std::filesystem::path& path);
std::optional<ExrImage> DecodeExr(std::span<const std::uint8_t> bytes, std::string& error);
std::optional<ExrImage> LoadExr(const std::filesystem::path& path, std::string& error);
bool ReadExrResolution(const std::filesystem::path& path, int& width, int& height, std::string& error);
std::vector<std::uint8_t> ExrHalfPixels(const ExrImage& image);
enum class ExrByteMode { LinearData, SrgbColor, Preview };
// Preview applies Reinhard + sRGB to RGB only. Data maps clamp to [0,1] without gamma.
std::vector<std::uint8_t> ExrRgba8(const ExrImage& image, ExrByteMode mode);
}
