add_library(IXEngineTree STATIC
    src/tree.cpp
    src/leaf_trim.cpp
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

if(BUILD_TESTING AND NOT ANDROID)
    add_executable(TreeGeneratorTest tests/TreeGeneratorTest.cpp)
    target_link_libraries(TreeGeneratorTest PRIVATE IXEngineTree)
    if(MSVC)
        set_target_properties(TreeGeneratorTest PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME TreeGeneratorTest COMMAND TreeGeneratorTest)
endif()
