#pragma once

#include <filesystem>
#include <string>

namespace material_tool
{
// Shared by editor and runtime asset loaders. A failed replacement preserves the old file.
bool SaveMaterialTextAtomic(const std::filesystem::path& path, const std::string& text, std::string& error);
bool LoadMaterialText(const std::filesystem::path& path, std::string& text, std::string& error);
}
