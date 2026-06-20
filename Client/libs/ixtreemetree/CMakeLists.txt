add_library(IXEngineTree STATIC
    src/tree.cpp
    src/branch_gen.cpp
    src/leaf_gen.cpp
    src/rng.cpp
    src/preset_loader.cpp
)

target_include_directories(IXEngineTree
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(IXEngineTree
    PRIVATE
        IXEngineMath
)

target_compile_features(IXEngineTree PUBLIC cxx_std_17)

add_library(IXEngineTree::IXEngineTree ALIAS IXEngineTree)
