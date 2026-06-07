#pragma once

#include "MapEditorTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace client::render {

constexpr const char* kWaterBodiesFilename = "water_bodies.mxwater";
constexpr std::uint32_t kWaterBodiesFormatVersion = 2;

bool LoadWaterBodiesBinary(const std::vector<std::uint8_t>& bytes,
                           std::vector<WaterBody>& outBodies,
                           std::string* error = nullptr);

bool SaveWaterBodiesBinary(const std::filesystem::path& path,
                           const std::vector<WaterBody>& bodies,
                           std::string* error = nullptr);

std::vector<WaterBody> CreateWaterBodyTestSet();

} // namespace client::render
