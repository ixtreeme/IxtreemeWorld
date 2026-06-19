add_library(IXEngineTerrainEditorSystem STATIC
    TerrainEditorSystem.cpp
    TerrainEditorSystem.h
)

target_include_directories(IXEngineTerrainEditorSystem
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_link_libraries(IXEngineTerrainEditorSystem
    PUBLIC
        IXEngineRender
)

target_compile_features(IXEngineTerrainEditorSystem PUBLIC cxx_std_20)
