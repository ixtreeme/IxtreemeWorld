// Provides the game-module DLL's registration plumbing and its exported entry point. Compiled ONLY into
// the module (never the engine), and only meaningful when IXTREEME_GAME_MODULE is defined.
//
// Include it in EVERY script .cpp (right after NativeScript.h) so each file is self-contained — the
// entry point + DLL-local list are `inline`, so the linker merges them across all your module's .cpp
// into a single export. There is no special "first file": adding or deleting any one script is safe.
//
// Usage (in each .cpp of your module):
//     #include "ixtreeme/NativeScript.h"
//     #include "ixtreeme/IxModuleRegistry.inl"
//     // ... your class(es) + IXSCRIPT_REGISTER(MyScript) ...
//
// (IXTREEME_GAME_MODULE must be on for module mode: the editor-generated CMakeLists sets it for the
//  whole target; building standalone with your own CMakeLists, #define it atop each .cpp — see the SDK example.)

#pragma once

#include "IxModuleApi.h"
#include "NativeScript.h"

#include <vector>

namespace ixscript
{
namespace detail
{

struct ModuleEntry
{
    const char* name;             // points at the #Class string literal (static storage — safe as char*)
    IxNativeScriptFactory factory;
};

// The DLL-local registry. A function-local static so IXSCRIPT_REGISTER initializers (which run before
// the entry point is called) populate it regardless of cross-TU static-init order.
inline std::vector<ModuleEntry>& ModuleList()
{
    static std::vector<ModuleEntry> entries;
    return entries;
}

// Called by IXSCRIPT_REGISTER in a module build. Records the class for the entry point to drain.
inline bool ModuleRegisterScript(const char* className, IxNativeScriptFactory factory)
{
    ModuleList().push_back(ModuleEntry{className, factory});
    return true;
}

} // namespace detail
} // namespace ixscript

// The single export the engine resolves and calls. Drains the DLL-local list into the engine-owned
// registrar (which copies each name and stores each factory), then returns this module's API version.
// `inline` so accidentally including this .inl in two of the module's TUs is harmless (no duplicate
// symbol); dllexport/default-visibility still exports the one merged definition.
IX_MODULE_EXPORT inline std::uint32_t IxtreemeGameModule_v1(ixscript::IModuleRegistrar* reg)
{
    if (reg)
        for (const ixscript::detail::ModuleEntry& entry : ixscript::detail::ModuleList())
            reg->RegisterScript(entry.name, entry.factory);
    return IXTREEME_MODULE_API_VERSION;
}
