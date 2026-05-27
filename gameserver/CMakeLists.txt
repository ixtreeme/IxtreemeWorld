cmake_minimum_required(VERSION 3.21)
project(gameserver
    VERSION 0.1.0
    DESCRIPTION "GameServer for the new MMORPG project"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Boost REQUIRED COMPONENTS system)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

add_subdirectory(libs/common)
add_subdirectory(libs/network)
add_subdirectory(apps/gameserver)
