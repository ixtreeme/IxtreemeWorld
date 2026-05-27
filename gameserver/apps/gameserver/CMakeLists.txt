add_executable(gameserver
    src/main.cpp
)

target_link_libraries(gameserver
    PRIVATE
        common
        network
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config/gameserver.conf.example
    ${CMAKE_BINARY_DIR}/gameserver.conf.example
    COPYONLY
)
