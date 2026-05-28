add_executable(db_test
    src/main.cpp
)

target_link_libraries(db_test
    PRIVATE
        common
        db
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config/database.json.example
    ${CMAKE_BINARY_DIR}/database.json.example
    COPYONLY
)
