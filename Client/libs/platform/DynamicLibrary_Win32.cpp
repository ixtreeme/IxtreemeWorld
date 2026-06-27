#include "platform/dynamic_library.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace platform
{
DynamicLibraryHandle OpenLibrary(const std::filesystem::path& path, std::string* errorOut)
{
    const HMODULE module = ::LoadLibraryW(path.wstring().c_str());
    if (!module && errorOut)
        *errorOut = "LoadLibraryW failed: " + std::to_string(::GetLastError());
    return reinterpret_cast<DynamicLibraryHandle>(module);
}

void* GetLibrarySymbol(DynamicLibraryHandle handle, const char* symbolName)
{
    if (!handle || !symbolName)
        return nullptr;
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle), symbolName));
}

void CloseLibrary(DynamicLibraryHandle handle)
{
    if (handle)
        ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
}
}
#endif
