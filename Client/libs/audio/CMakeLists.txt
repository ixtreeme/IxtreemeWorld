add_library(IXEngineAudio STATIC
    AudioEngine.cpp
    AudioEngine.h
    AudioComponents.h
    miniaudio_impl.cpp
)

target_include_directories(IXEngineAudio
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party  # vendored miniaudio.h — only this lib's .cpp needs it
)

target_link_libraries(IXEngineAudio
    PUBLIC
        IXEngineDebug
)

target_compile_definitions(IXEngineAudio
    PUBLIC
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

target_compile_features(IXEngineAudio PUBLIC cxx_std_20)
