add_library(IXEngineMeshSystem STATIC
    LODSystem.cpp
    LODSystem.h
    MeshSystem.cpp
    MeshSystem.h
)

target_include_directories(IXEngineMeshSystem
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_link_libraries(IXEngineMeshSystem
    PUBLIC
        IXEngineRender
)

target_compile_features(IXEngineMeshSystem PUBLIC cxx_std_20)
