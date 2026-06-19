add_library(IXEngineImportExport STATIC
    AssimpExporter.cpp
    AssimpExporter.h
    AssimpImporter.cpp
    AssimpImporter.h
    FbxAssetSidecars.cpp
    FbxAssetSidecars.h
    tree/TreeGlbExporter.cpp
    tree/TreeGlbExporter.h
)

target_include_directories(IXEngineImportExport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tree
        ${CLIENT_LIBS_DIR}
)

target_link_libraries(IXEngineImportExport
    PUBLIC
        IXEngineAsset
        IXEngineDebug
        IXEngineCommon
        IXEngineTree::IXEngineTree
        assimp::assimp
        ${CLIENT_OZZ_TARGETS}
)

target_compile_definitions(IXEngineImportExport
    PUBLIC
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

target_compile_features(IXEngineImportExport PUBLIC cxx_std_20)
