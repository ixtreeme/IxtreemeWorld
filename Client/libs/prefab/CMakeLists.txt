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
