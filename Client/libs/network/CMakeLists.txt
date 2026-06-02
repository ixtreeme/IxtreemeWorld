add_library(client_network STATIC
    CharacterListItem.h
    IClientHandler.h
    ClientSession.h
    ClientSession.cpp
)

target_include_directories(client_network
    PUBLIC
        ${CLIENT_LIBS_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(client_network
    PUBLIC
        Boost::system
        protocol
)

target_compile_features(client_network PUBLIC cxx_std_20)

target_compile_definitions(client_network
    PUBLIC
        $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0601>
)

target_link_libraries(client_network
    PUBLIC
        $<$<PLATFORM_ID:Windows>:Ws2_32>
)
