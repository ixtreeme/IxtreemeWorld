add_library(IXEngineCommon STATIC
    Common.cpp
    Common.h
)

target_include_directories(IXEngineCommon
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_compile_definitions(IXEngineCommon
    PUBLIC
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

target_compile_features(IXEngineCommon PUBLIC cxx_std_20)
