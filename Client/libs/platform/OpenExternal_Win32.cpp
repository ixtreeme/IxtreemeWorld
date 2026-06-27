#include "platform/open_external.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace platform
{
bool OpenInDefaultApp(const std::filesystem::path& path, std::string* errorOut)
{
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    const std::wstring wpath = (ec ? path : abs).wstring();
    const HINSTANCE rc = ::ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecute returns a value > 32 on success.
    if (reinterpret_cast<INT_PTR>(rc) > 32)
        return true;
    if (errorOut)
        *errorOut = "ShellExecuteW failed: " + std::to_string(reinterpret_cast<INT_PTR>(rc));
    return false;
}
}
#endif
