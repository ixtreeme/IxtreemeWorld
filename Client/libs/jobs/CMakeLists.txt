add_library(IXEngineJobs STATIC
    JobSystem.cpp
    JobSystem.h
)

target_include_directories(IXEngineJobs
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_compile_definitions(IXEngineJobs
    PUBLIC
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

target_compile_features(IXEngineJobs PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(IXEngineJobs PUBLIC Threads::Threads)

if(BUILD_TESTING AND NOT ANDROID)
    add_executable(JobSystemTest tests/JobSystemTest.cpp)
    target_link_libraries(JobSystemTest PRIVATE IXEngineJobs)
    target_compile_features(JobSystemTest PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(JobSystemTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME JobSystemTest COMMAND JobSystemTest)
endif()
