find_package(unofficial-mariadb-connector-cpp CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(db STATIC
    src/DbConfig.cpp
    src/DbPool.cpp
    src/AccountRepository.cpp
    src/CharacterRepository.cpp
    src/HandoffTokenRepository.cpp
)

target_include_directories(db
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(db
    PUBLIC
        common
        Boost::asio
        Boost::system
        Boost::throw_exception
        unofficial::mariadb-connector-cpp::mariadbcpp
        unofficial-sodium::sodium
        nlohmann_json::nlohmann_json
)

target_compile_features(db PUBLIC cxx_std_20)

target_compile_definitions(db
    PUBLIC
        $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>
)
