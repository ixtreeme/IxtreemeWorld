file(GLOB IXSCRIPT_LUA_SOURCES CONFIGURE_DEPENDS third_party/lua/*.c)
# User/sample native scripts (the C++ "MonoBehaviour"s). Drop new *.cpp here + IXSCRIPT_REGISTER.
file(GLOB IXSCRIPT_NATIVE_SCRIPTS CONFIGURE_DEPENDS scripts_native/*.cpp)

add_library(IXEngineScript STATIC
    ScriptSystem.cpp
    ScriptSystem.h
    NativeBackend.cpp
    NativeBackend.h
    NativeScript.h
    LuaBackend.cpp
    LuaBackend.h
    ScriptApi.h
    IScriptBackend.h
    ScriptComponent.h
    IxModuleApi.h
    ${IXSCRIPT_NATIVE_SCRIPTS}
    ${IXSCRIPT_LUA_SOURCES}
)

# Vendored Lua is C; compile it as C (separate TUs) so its linkage stays C. sol2 wraps lua.h in
# extern "C" to match.
set_source_files_properties(${IXSCRIPT_LUA_SOURCES} PROPERTIES LANGUAGE C)

target_include_directories(IXEngineScript
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua   # lua.h etc. — only this lib's .cpp needs it
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sol2   # <sol/sol.hpp>
)

target_link_libraries(IXEngineScript
    PUBLIC
        IXEngineDebug
)

target_compile_definitions(IXEngineScript
    PRIVATE
        SOL_ALL_SAFETIES_ON=1   # sol2 wraps every call — never throws into the engine
    PUBLIC
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

target_compile_features(IXEngineScript PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(IXEngineScript PRIVATE /bigobj)  # sol2's binding TU exceeds the obj limit
    # Silence vendored-Lua's own benign warnings (uninitialized-var false positives in lvm.c, etc.).
    set_source_files_properties(${IXSCRIPT_LUA_SOURCES} PROPERTIES COMPILE_OPTIONS "/wd4701;/wd4702;/wd4310")
endif()

# Keep the shipped game-module SDK headers in lock-step with the canonical ones (no drift = no ABI
# surprises). These four headers are the public native-scripting SDK surface.
set(IXSCRIPT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/include/ixtreeme")
add_custom_command(TARGET IXEngineScript POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${IXSCRIPT_SDK_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/NativeScript.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/ScriptApi.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/IxModuleApi.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/IxModuleRegistry.inl"
        "${IXSCRIPT_SDK_DIR}"
    COMMENT "Syncing native game-module SDK headers -> sdk/include/ixtreeme"
    VERBATIM)
