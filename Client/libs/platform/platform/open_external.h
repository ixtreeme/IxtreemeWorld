#pragma once

// Opens a file/path in the OS default application (Win32 ShellExecute / POSIX xdg-open|open), behind
// the platform abstraction. Used by the asset browser to open a script file in the dev's editor.

#include <filesystem>
#include <string>

namespace platform
{
// Fire-and-forget. Returns false (and fills errorOut if provided) when the open couldn't be initiated.
bool OpenInDefaultApp(const std::filesystem::path& path, std::string* errorOut = nullptr);
}
