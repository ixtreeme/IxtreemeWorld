add_library(IXEnginePrefab STATIC
    PrefabDocument.cpp
    PrefabDocument.h
)

target_include_directories(IXEnginePrefab
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_link_libraries(IXEnginePrefab
    PUBLIC
        IXEngineRender
)

target_compile_features(IXEnginePrefab PUBLIC cxx_std_20)

if(BUILD_TESTING AND NOT ANDROID)
    add_executable(PrefabRuntimeTest tests/PrefabRuntimeTest.cpp)
    target_link_libraries(PrefabRuntimeTest PRIVATE IXEnginePrefab)
    target_compile_features(PrefabRuntimeTest PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(PrefabRuntimeTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME PrefabRuntimeTest COMMAND PrefabRuntimeTest)
endif()
