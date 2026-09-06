find_package(flecs CONFIG REQUIRED)

if(TARGET flecs::flecs_static)
    set(GS_FLECS_TARGET flecs::flecs_static)
elseif(TARGET flecs::flecs)
    set(GS_FLECS_TARGET flecs::flecs)
else()
    message(FATAL_ERROR "flecs CMake target not found")
endif()

add_executable(gameserver
    src/main.cpp
    src/GameConnectionHandler.cpp
    src/world/WorldRuntime.cpp
    src/world/zone/Zone.cpp
    src/world/zone/ZoneGraph.cpp
    src/world/zone/ZoneManager.cpp
    src/world/zone/ZoneOwnership.cpp
    src/world/zone/ZoneScheduler.cpp
    src/world/zone/ZoneWorkerPool.cpp
    src/world/systems/AiSystem.cpp
    src/world/systems/CombatSystem.cpp
    src/world/systems/CooldownSystem.cpp
    src/world/systems/MovementSystem.cpp
    src/world/spatial/AoiSystem.cpp
    src/world/spatial/SpatialGrid.cpp
    src/world/spatial/SpatialValidator.cpp
    src/world/debug/WorldValidator.cpp
    src/world/visibility/BorderPublisher.cpp
    src/world/visibility/GhostSystem.cpp
    src/world/visibility/VisibilitySystem.cpp
    src/world/migration/EntityTransfer.cpp
    src/world/migration/MigrationCoordinator.cpp
    src/world/migration/MigrationSystem.cpp
    src/world/input/InputRouter.cpp
    src/world/terrain/TerrainService.cpp
    src/world/spawn/MobPrototypeRegistry.cpp
    src/world/spawn/RespawnSystem.cpp
    src/world/spawn/SpawnCoordinator.cpp
    src/world/spawn/SpawnLoader.cpp
    src/world/spawn/SpawnSystem.cpp
    src/world/replication/ProtocolEncoder.cpp
    src/world/replication/ReplicationSystem.cpp
    src/world/replication/SnapshotBuilder.cpp
)

target_link_libraries(gameserver
    PRIVATE
        common
        network
        protocol
        mapdata
        db
        ${GS_FLECS_TARGET}
)


target_compile_definitions(gameserver
    PRIVATE
        IXTREEME_DEFAULT_MAP_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../../Client/assets/Maps/test_zone"
        IXTREEME_DEFAULT_MOB_TYPES_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/config/mob_types.conf"
        $<$<CONFIG:Debug>:MMO_DEBUG_SPAWN_OVERRIDE=1>
        $<$<CONFIG:RelWithDebInfo>:MMO_DEBUG_SPAWN_OVERRIDE=1>
        $<$<CONFIG:Release>:MMO_DEBUG_SPAWN_OVERRIDE=0>
        $<$<CONFIG:MinSizeRel>:MMO_DEBUG_SPAWN_OVERRIDE=0>
)


configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config/gameserver.conf.example
    ${CMAKE_BINARY_DIR}/gameserver.conf.example
    COPYONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config/mob_types.conf
    ${CMAKE_BINARY_DIR}/mob_types.conf
    COPYONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config/database.json.example
    ${CMAKE_BINARY_DIR}/database.json.example
    COPYONLY
)

# Load/benchmark harness. Dev-only behind ENABLE_WORLDBENCH (default OFF);
# never linked into production. Drives WorldRuntime through its public API.
if(ENABLE_WORLDBENCH)
    add_executable(worldbench
        src/bench/WorldBench.cpp
        src/world/WorldRuntime.cpp
        src/world/zone/Zone.cpp
        src/world/zone/ZoneGraph.cpp
        src/world/zone/ZoneManager.cpp
        src/world/zone/ZoneOwnership.cpp
        src/world/zone/ZoneScheduler.cpp
        src/world/zone/ZoneWorkerPool.cpp
        src/world/systems/AiSystem.cpp
        src/world/systems/CombatSystem.cpp
        src/world/systems/CooldownSystem.cpp
        src/world/systems/MovementSystem.cpp
        src/world/spatial/AoiSystem.cpp
        src/world/spatial/SpatialGrid.cpp
        src/world/spatial/SpatialValidator.cpp
        src/world/debug/WorldValidator.cpp
        src/world/visibility/BorderPublisher.cpp
        src/world/visibility/GhostSystem.cpp
        src/world/visibility/VisibilitySystem.cpp
        src/world/migration/EntityTransfer.cpp
        src/world/migration/MigrationCoordinator.cpp
        src/world/migration/MigrationSystem.cpp
        src/world/input/InputRouter.cpp
        src/world/terrain/TerrainService.cpp
        src/world/spawn/MobPrototypeRegistry.cpp
        src/world/spawn/RespawnSystem.cpp
        src/world/spawn/SpawnCoordinator.cpp
        src/world/spawn/SpawnLoader.cpp
        src/world/spawn/SpawnSystem.cpp
        src/world/replication/ProtocolEncoder.cpp
        src/world/replication/ReplicationSystem.cpp
        src/world/replication/SnapshotBuilder.cpp
    )

    target_link_libraries(worldbench
        PRIVATE
            common
            network
            protocol
            mapdata
            db
            ${GS_FLECS_TARGET}
    )

    target_compile_definitions(worldbench
        PRIVATE
            IXTREEME_DEFAULT_MAP_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../../Client/assets/Maps/test_zone"
            IXTREEME_DEFAULT_MOB_TYPES_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/config/mob_types.conf"
            MMO_DEBUG_SPAWN_OVERRIDE=1
    )
endif()
