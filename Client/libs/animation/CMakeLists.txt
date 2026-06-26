add_library(IXEngineAnimation STATIC
    AnimationRuntime.cpp
    AnimationRuntime.h
    AnimatorController.cpp
    AnimatorController.h
    AnimatorRuntime.cpp
    AnimatorRuntime.h
)

target_include_directories(IXEngineAnimation
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_link_libraries(IXEngineAnimation
    PUBLIC
        IXEngineDebug
        ${CLIENT_OZZ_TARGETS}
)

target_compile_definitions(IXEngineAnimation
    PUBLIC
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

target_compile_features(IXEngineAnimation PUBLIC cxx_std_20)
