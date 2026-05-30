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
    src/SimWorld.cpp
)

target_link_libraries(gameserver
    PRIVATE
        common
        network
        protocol
        db
        ${GS_FLECS_TARGET}
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config/gameserver.conf.example
    ${CMAKE_BINARY_DIR}/gameserver.conf.example
    COPYONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config/database.json.example
    ${CMAKE_BINARY_DIR}/database.json.example
    COPYONLY
)
