#pragma once

// Native "open file" / "save file" dialogs behind the platform abstraction (Win32 common dialogs;
// not implemented on other platforms yet). Editor tools use these instead of calling an OS API.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace platform
{
struct FileDialogFilter
{
    std::string label;     // Shown in the type list, e.g. "Textures".
    std::string patterns;  // Semicolon-separated, e.g. "*.png;*.jpg".
};

struct FileDialogOptions
{
    bool save = false;                     // Save dialog (asks before overwriting) instead of open.
    std::string title;                     // Empty: the platform's default title.
    std::vector<FileDialogFilter> filters; // Empty: every file.
    std::string defaultExtension;          // Without the dot; added when the user types none.
    std::filesystem::path initialPath;     // Preselected file, or empty.
    void* ownerWindow = nullptr;           // Native window handle the dialog belongs to, or null.
};

// Blocks until the user answers. Returns the chosen file, or nothing when the dialog was cancelled
// or the platform has no file dialog.
std::optional<std::filesystem::path> ShowFileDialog(const FileDialogOptions& options);
}
