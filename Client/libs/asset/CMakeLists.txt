add_library(IXEngineAsset STATIC
    AssetDatabase.cpp
    AssetDatabase.h
    AssetLibrary.cpp
    AssetLibrary.h
    AssetWatcher.cpp
    AssetWatcher.h
    IAssetReader.h
    MaterialAssetManager.cpp
    MaterialAssetManager.h
    ProjectManager.cpp
    ProjectManager.h
)

if(WIN32)
    target_sources(IXEngineAsset PRIVATE
        FileAssetReader.h
        FileAssetReader.cpp
    )
elseif(ANDROID)
    target_sources(IXEngineAsset PRIVATE
        AAssetManagerAssetReader.h
        AAssetManagerAssetReader.cpp
    )
    target_link_libraries(IXEngineAsset PUBLIC android)
endif()

target_link_libraries(IXEngineAsset
    PUBLIC
        IXEngineCommon
        IXEngineDebug
        efsw::efsw
)

target_include_directories(IXEngineAsset
    PUBLIC
        ${CLIENT_LIBS_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}/render
        ${STB_INCLUDE_DIR}
)

target_compile_features(IXEngineAsset PUBLIC cxx_std_20)
