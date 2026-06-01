find_package(ZLIB REQUIRED)
find_package(CapnProto CONFIG REQUIRED)

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(_capnp_host_dir "C:/vcpkg/installed/x64-windows-static/tools/capnproto")
    if(NOT EXISTS "${_capnp_host_dir}/capnp.exe")
        message(FATAL_ERROR
            "Cap'n Proto host-tool nem talalhato: ${_capnp_host_dir}/capnp.exe\n"
            "Telepitsd: vcpkg install capnproto:x64-windows-static"
        )
    endif()
    set(CAPNP_EXECUTABLE "${_capnp_host_dir}/capnp.exe"
        CACHE FILEPATH "Cap'n Proto compiler (host build for cross-compile)" FORCE)
    set(CAPNPC_CXX_EXECUTABLE "${_capnp_host_dir}/capnpc-c++.exe"
        CACHE FILEPATH "Cap'n Proto C++ codegen plugin (host build)" FORCE)
endif()

set(MAP_SCHEMA_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/schema/map_manifest.capnp
)

capnp_generate_cpp(MAP_CAPNP_SRCS MAP_CAPNP_HDRS ${MAP_SCHEMA_FILES})

add_library(mapdata STATIC
    ${MAP_CAPNP_SRCS}
    src/MapData.cpp
)

target_include_directories(mapdata
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(mapdata
    PUBLIC
        CapnProto::capnp
)

target_compile_features(mapdata PUBLIC cxx_std_20)

if(MSVC)
    set_target_properties(mapdata PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    add_executable(mapgen_test_zone
        tools/mapgen_test_zone.cpp
    )
    target_link_libraries(mapgen_test_zone PRIVATE mapdata CapnProto::capnp)
    target_compile_features(mapgen_test_zone PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(mapgen_test_zone PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
endif()
