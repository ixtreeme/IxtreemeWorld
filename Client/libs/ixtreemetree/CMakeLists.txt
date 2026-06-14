add_library(ixtreemetree STATIC
    src/tree.cpp
    src/branch_gen.cpp
    src/leaf_gen.cpp
    src/rng.cpp
    src/preset_loader.cpp
)

target_include_directories(ixtreemetree
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(ixtreemetree PUBLIC cxx_std_17)

add_library(ixtreemetree::ixtreemetree ALIAS ixtreemetree)
