add_library(network STATIC
    src/Server.cpp
    src/Session.cpp
    src/Framing.cpp
)

target_include_directories(network
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(network
    PUBLIC
        common
        Boost::system
        Boost::headers
        $<$<PLATFORM_ID:Windows>:ws2_32>
)

target_compile_features(network PUBLIC cxx_std_20)

target_compile_definitions(network
    PUBLIC
        BOOST_ASIO_NO_DEPRECATED
        BOOST_ASIO_HAS_CO_AWAIT
        $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>
)
