add_library(client_network STATIC
    CharacterListItem.h
    IClientHandler.h
    ClientSession.h
    ClientSession.cpp
)

find_package(Boost CONFIG REQUIRED COMPONENTS system)

target_include_directories(client_network
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(client_network
    PUBLIC
        Boost::system
        protocol
)

target_compile_features(client_network PUBLIC cxx_std_20)

if(WIN32)
    target_link_libraries(client_network PUBLIC Ws2_32)
    target_compile_definitions(client_network PUBLIC _WIN32_WINNT=0x0601)
endif()

if(MSVC)
    set_target_properties(client_network PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
