#include "platform/file_dialog.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <iterator>

namespace platform
{
namespace
{
std::wstring Wide(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    if (length > 0)
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}
}

std::optional<std::filesystem::path> ShowFileDialog(const FileDialogOptions& options)
{
    wchar_t file[32768]{};
    const std::wstring initial = options.initialPath.wstring();
    if (initial.size() < std::size(file))
        std::copy(initial.begin(), initial.end(), file);

    // "label\0patterns\0...\0\0", as the common dialog expects.
    std::wstring filter;
    for (const FileDialogFilter& entry : options.filters)
    {
        filter += Wide(entry.label);
        filter.push_back(L'\0');
        filter += Wide(entry.patterns);
        filter.push_back(L'\0');
    }
    filter.push_back(L'\0');
    const std::wstring title = Wide(options.title);
    const std::wstring extension = Wide(options.defaultExtension);

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(options.ownerWindow);
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.lpstrFilter = options.filters.empty() ? nullptr : filter.c_str();
    dialog.lpstrDefExt = extension.empty() ? nullptr : extension.c_str();
    dialog.lpstrTitle = title.empty() ? nullptr : title.c_str();
    // The process keeps its working directory; relative asset paths depend on it.
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (options.save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (options.save ? ::GetSaveFileNameW(&dialog) : ::GetOpenFileNameW(&dialog))
        return std::filesystem::path(file);
    return std::nullopt;
}
}
#endif
