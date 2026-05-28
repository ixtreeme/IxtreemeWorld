find_package(ZLIB REQUIRED)
find_package(CapnProto CONFIG REQUIRED)

set(SCHEMA_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/schema/packet.capnp
)

capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS ${SCHEMA_FILES})

add_library(protocol STATIC
    ${CAPNP_SRCS}
    src/ProtocolError.cpp
)

target_include_directories(protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(protocol
    PUBLIC
        common
        CapnProto::capnp
)

target_compile_features(protocol PUBLIC cxx_std_20)
