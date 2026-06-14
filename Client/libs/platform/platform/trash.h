#pragma once

#include <filesystem>
#include <string>

namespace platform
{
bool move_to_trash(const std::filesystem::path& path, std::string* errorOut = nullptr);
}
