#pragma once

#include <filesystem>
#include <string>

bool ProcessImportedFbxAsset(const std::filesystem::path& destination,
                             const std::filesystem::path& libraryRoot,
                             std::string& error);
