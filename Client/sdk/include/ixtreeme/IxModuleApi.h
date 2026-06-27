#pragma once

// The STABLE C-ABI contract between the engine binary and a third-party native game-module DLL.
// A module is a DLL the prebuilt engine loads at runtime; it registers its C++ NativeScript classes
// through this seam without ever linking the engine. Designed for the engine's STATIC CRT (/MT): only
// scalars, raw pointers, and vtable calls cross — never an STL object that one side allocates and the
// other frees. Game modules must be built with the SAME MSVC toolset + /MT + C++20 as the engine.
//
// This header is part of the SDK shipped to game developers.

#include <cstdint>

// Bump on ANY break to: the IModuleRegistrar vtable, the NativeScript vtable / hook set, the IScriptApi
// vtable, the FieldBinder vtable, or the entry-point signature. The engine rejects a module whose
// returned version differs. v2 added NativeScript::DeclareFields (a new vtable slot) + FieldBinder.
// v3 added IScriptApi spawn/destroy/raycast + animator-param vtable slots (rebuild v2 modules).
#define IXTREEME_MODULE_API_VERSION 3u

// The exact exported symbol name the engine resolves in each module DLL.
#define IXTREEME_MODULE_ENTRY_SYMBOL "IxtreemeGameModule_v1"

// Cross-platform export marker for the module's entry point (Win32 dllexport / ELF default visibility).
#if defined(_WIN32)
#define IX_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define IX_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace ixscript
{

class NativeScript;

// RAW factory: news a NativeScript inside the DLL (NOT std::unique_ptr — no STL ownership on the seam).
// The engine wraps the returned pointer in its own unique_ptr; the object is freed via NativeScript's
// VIRTUAL destructor, which routes operator delete back to the DLL's heap. Heap-safe under /MT.
using IxNativeScriptFactory = NativeScript* (*)();

// Engine-owned registrar handed to the module's entry point. The module CALLS in; the engine OWNS every
// allocation. Only C-ABI types cross this vtable.
class IModuleRegistrar
{
public:
    // className: NUL-terminated UTF-8, owned by the DLL and valid only for the duration of this call —
    // the engine COPIES it. factory: a DLL-local raw factory the engine stores and calls later.
    virtual void RegisterScript(const char* className, IxNativeScriptFactory factory) = 0;
    virtual std::uint32_t ApiVersion() const = 0;

protected:
    ~IModuleRegistrar() = default;  // engine owns its lifetime; a module must never delete it
};

// The reflected-field seam (Unity-[SerializeField] style). The ENGINE implements this; a script's
// DeclareFields hands each inspector-visible member to the binder, once per member. Only scalars,
// const char*, and float* cross — never an STL object — so it is /MT-safe exactly like IModuleRegistrar.
// The engine subclass runs in one of two modes, opaque to the module: REFLECT (read *ref to capture the
// in-class default + record name/type for the schema) or APPLY (overwrite *ref with the stored value).
// The module's DeclareFields body is mode-agnostic — it just binds each member.
class FieldBinder
{
public:
    // name: NUL-terminated UTF-8 in the DLL's static storage (the #field literal), valid for the call;
    // the engine copies it if it keeps it. ref/array: address of a POD member INSIDE the DLL-allocated
    // instance — the engine reads or writes *ref but never allocates/frees it.
    virtual void Float(const char* name, float& ref) = 0;
    virtual void Int(const char* name, int& ref) = 0;
    virtual void Bool(const char* name, bool& ref) = 0;
    virtual void Vec3(const char* name, float ref[3]) = 0;  // decays to float* — a raw pointer, no STL
    virtual void Color(const char* name, float ref[4]) = 0; // rgba 0..1

    // Header-only inline overload set: lets IX_REFLECT deduce the member type and dispatch to the right
    // virtual. No ABI surface of their own (they just call a virtual), so editing them never bumps the
    // version. Fixed-extent array refs disambiguate vec3 (3) from color (4).
    void Auto(const char* name, float& ref) { Float(name, ref); }
    void Auto(const char* name, int& ref) { Int(name, ref); }
    void Auto(const char* name, bool& ref) { Bool(name, ref); }
    void Auto(const char* name, float (&ref)[3]) { Vec3(name, ref); }
    void Auto(const char* name, float (&ref)[4]) { Color(name, ref); }

protected:
    ~FieldBinder() = default;  // engine owns its lifetime; a module must never delete it
};

// Signature of the module entry point. The engine resolves IXTREEME_MODULE_ENTRY_SYMBOL, casts to this,
// calls it with an engine-owned registrar, and treats the RETURN as the module's API version (which it
// version-checks before trusting any registration). A module defines this via IxModuleRegistry.inl.
using IxModuleEntryFn = std::uint32_t (*)(IModuleRegistrar*);

} // namespace ixscript
