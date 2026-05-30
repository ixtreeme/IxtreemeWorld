add_executable(loginserver
    src/main.cpp
    src/ConnectionHandler.cpp
    src/AuthHandler.cpp
    src/GameHandler.cpp
)

target_link_libraries(loginserver
    PRIVATE
        common
        network
        protocol
        db
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config/loginserver.conf.example
    ${CMAKE_BINARY_DIR}/loginserver.conf.example
    COPYONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config/database.json.example
    ${CMAKE_BINARY_DIR}/database.json.example
    COPYONLY
)
