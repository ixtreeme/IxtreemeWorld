add_library(IXEngineDebug STATIC
    Debug.cpp
    Debug.h
    LogConfig.h
)

target_include_directories(IXEngineDebug
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_compile_definitions(IXEngineDebug
    PUBLIC
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

if(ANDROID)
    target_link_libraries(IXEngineDebug PUBLIC log)
endif()

target_compile_features(IXEngineDebug PUBLIC cxx_std_20)
