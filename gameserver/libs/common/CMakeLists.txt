add_library(common STATIC
    src/Logging.cpp
    src/Config.cpp
)

target_include_directories(common
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(common
    PUBLIC
        spdlog::spdlog
        fmt::fmt
)

target_compile_features(common PUBLIC cxx_std_20)

target_compile_definitions(common
    PUBLIC
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
)
