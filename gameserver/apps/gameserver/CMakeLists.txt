add_executable(gameserver
    src/main.cpp
    src/ConnectionHandler.cpp
    src/AuthHandler.cpp
    src/GameHandler.cpp
)

target_link_libraries(gameserver
    PRIVATE
        common
        network
        protocol
        db
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
