#include "platform/dynamic_library.h"

#if !defined(_WIN32)
#include <dlfcn.h>

namespace platform
{
DynamicLibraryHandle OpenLibrary(const std::filesystem::path& path, std::string* errorOut)
{
    // RTLD_LOCAL keeps the module's symbols out of the global namespace (modules don't clash);
    // RTLD_NOW resolves everything up front so a bad module fails at load, not mid-frame.
    void* handle = ::dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle && errorOut)
    {
        const char* err = ::dlerror();
        *errorOut = err ? err : "dlopen failed";
    }
    return handle;
}

void* GetLibrarySymbol(DynamicLibraryHandle handle, const char* symbolName)
{
    if (!handle || !symbolName)
        return nullptr;
    return ::dlsym(handle, symbolName);
}

void CloseLibrary(DynamicLibraryHandle handle)
{
    if (handle)
        ::dlclose(handle);
}
}
#endif
