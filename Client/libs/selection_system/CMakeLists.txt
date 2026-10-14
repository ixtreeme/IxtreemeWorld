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

if(BUILD_TESTING AND NOT ANDROID)
    add_executable(MeshSelectionTest tests/MeshSelectionTest.cpp)
    target_link_libraries(MeshSelectionTest PRIVATE IXEngineSelectionSystem)
    if(MSVC)
        set_target_properties(MeshSelectionTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME MeshSelectionTest COMMAND MeshSelectionTest)
endif()
