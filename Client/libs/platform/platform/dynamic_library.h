#pragma once

// Cross-platform dynamic-library loading (Win32 LoadLibrary / POSIX dlopen), behind the platform
// abstraction so the rest of the engine never touches an OS API directly. Used to load third-party
// game-module DLLs at runtime (the native C++ scripting "SDK" path).

#include <filesystem>
#include <string>

namespace platform
{
using DynamicLibraryHandle = void*;  // opaque (HMODULE on Win32, void* from dlopen elsewhere)

// Loads a shared library. Returns nullptr on failure (and fills errorOut if provided).
DynamicLibraryHandle OpenLibrary(const std::filesystem::path& path, std::string* errorOut = nullptr);

// Resolves an exported symbol by name. Returns nullptr if not found.
void* GetLibrarySymbol(DynamicLibraryHandle handle, const char* symbolName);

// Unloads a library. No-op on a null handle.
void CloseLibrary(DynamicLibraryHandle handle);
}
