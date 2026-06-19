add_library(IXEngineSelectionSystem STATIC
    SelectionSystem.cpp
    SelectionSystem.h
)

target_include_directories(IXEngineSelectionSystem
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_link_libraries(IXEngineSelectionSystem
    PUBLIC
        IXEngineMeshSystem
        IXEngineTerrainEditorSystem
)

target_compile_features(IXEngineSelectionSystem PUBLIC cxx_std_20)
