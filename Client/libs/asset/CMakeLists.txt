add_library(client_asset STATIC
    IAssetReader.h
)

if(WIN32)
    target_sources(client_asset PRIVATE
        FileAssetReader.h
        FileAssetReader.cpp
    )
elseif(ANDROID)
    target_sources(client_asset PRIVATE
        AAssetManagerAssetReader.h
        AAssetManagerAssetReader.cpp
    )
    target_link_libraries(client_asset PUBLIC android)
endif()

target_include_directories(client_asset
    PUBLIC
        ${CLIENT_LIBS_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(client_asset PUBLIC cxx_std_20)
