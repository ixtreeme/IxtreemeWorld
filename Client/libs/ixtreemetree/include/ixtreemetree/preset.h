#pragma once

#include "tree_options.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ixtreemetree
{
struct Preset
{
    std::string name;
    TreeOptions options;
};

std::optional<Preset> loadPresetFile(const std::filesystem::path& path, std::string* errorOut = nullptr);
std::vector<Preset> loadAllPresets(const std::filesystem::path& presetDir, std::vector<std::string>* errorsOut = nullptr);

std::optional<Preset> loadPreset(const std::filesystem::path& path, std::string* error = nullptr);
}
