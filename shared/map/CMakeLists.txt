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
    ${CMAKE_CURRENT_SOURCE_DIR}/schema/world_package_manifest.capnp
)

capnp_generate_cpp(MAP_CAPNP_SRCS MAP_CAPNP_HDRS ${MAP_SCHEMA_FILES})

add_library(IXEngineMapData STATIC
    ${MAP_CAPNP_SRCS}
    src/MapData.cpp
    src/LayeredWorld.cpp
    src/LayeredWorldGenerator.cpp
    src/LayeredWorldGeometry.cpp
    src/LayerGroundSupport.cpp
    src/LayerClearance.cpp
    src/LayerActorMovement.cpp
    src/ServerTerrain.cpp
    src/ServerWater.cpp
    src/WorldPackage.cpp
    src/WorldPackageWriter.cpp
)

add_library(mapdata ALIAS IXEngineMapData)

target_include_directories(IXEngineMapData
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(IXEngineMapData
    PUBLIC
        CapnProto::capnp
)

target_compile_features(IXEngineMapData PUBLIC cxx_std_20)

if(MSVC)
    set_target_properties(IXEngineMapData PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    add_executable(layer_clearance_test tools/layer_clearance_test.cpp)
    target_link_libraries(layer_clearance_test PRIVATE mapdata)
    target_compile_features(layer_clearance_test PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(layer_clearance_test PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    add_executable(layer_ground_support_test tools/layer_ground_support_test.cpp)
    target_link_libraries(layer_ground_support_test PRIVATE mapdata)
    target_compile_features(layer_ground_support_test PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(layer_ground_support_test PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    add_executable(terrain_surface_contract_test
        tools/terrain_surface_contract_test.cpp
    )
    target_link_libraries(terrain_surface_contract_test PRIVATE mapdata)
    target_compile_features(terrain_surface_contract_test PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(terrain_surface_contract_test PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()

    add_executable(layered_world_geometry_test
        tools/layered_world_geometry_test.cpp
    )
    target_link_libraries(layered_world_geometry_test PRIVATE mapdata)
    target_compile_features(layered_world_geometry_test PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(layered_world_geometry_test PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()

    add_executable(engine_server_map_compatibility_test
        tools/engine_server_map_compatibility_test.cpp
    )
    target_link_libraries(engine_server_map_compatibility_test PRIVATE mapdata)
    target_compile_features(engine_server_map_compatibility_test PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(engine_server_map_compatibility_test PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()

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

    add_executable(layered_world_contract_test
        tools/layered_world_contract_test.cpp
    )
    target_link_libraries(layered_world_contract_test PRIVATE mapdata)
    target_compile_features(layered_world_contract_test PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(layered_world_contract_test PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()

    add_executable(layered_world_package_test
        tools/layered_world_package_test.cpp
    )
    target_link_libraries(layered_world_package_test PRIVATE mapdata)
    target_compile_features(layered_world_package_test PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(layered_world_package_test PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()

    add_executable(layered_spatial_key_test
        tools/layered_spatial_key_test.cpp
    )
    target_link_libraries(layered_spatial_key_test PRIVATE mapdata)
    target_compile_features(layered_spatial_key_test PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(layered_spatial_key_test PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()

    add_executable(layered_world_generator_test
        tools/layered_world_generator_test.cpp
    )
    target_link_libraries(layered_world_generator_test PRIVATE mapdata)
    target_compile_features(layered_world_generator_test PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(layered_world_generator_test PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()

    add_executable(layered_world_generate
        tools/layered_world_generate.cpp
    )
    target_link_libraries(layered_world_generate PRIVATE mapdata)
    target_compile_features(layered_world_generate PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(layered_world_generate PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
endif()
