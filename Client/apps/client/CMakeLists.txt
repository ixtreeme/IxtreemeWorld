if(ANDROID)
    add_library(IxtreemeEngine SHARED
        src/EngineApplication.cpp
        src/EngineApplication.h
        src/EditorSceneRuntime.cpp
        src/EditorSceneRuntime.h
        src/ViewportControls.cpp
        src/ViewportControls.h
        src/main.cpp
    )

    set_target_properties(IxtreemeEngine PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    target_link_options(IxtreemeEngine PRIVATE
        "-u" "ANativeActivity_onCreate"
    )
else()
    add_executable(IxtreemeEngine
        src/EngineApplication.cpp
        src/EngineApplication.h
        src/EditorSceneRuntime.cpp
        src/EditorSceneRuntime.h
        src/ViewportControls.cpp
        src/ViewportControls.h
        src/main.cpp
    )
endif()

target_link_libraries(IxtreemeEngine
    PRIVATE
        IXEngineSelectionSystem
        IXEngineTerrainEditorSystem
        IXEngineMeshSystem
        IXEngineRender
)

target_compile_features(IxtreemeEngine PRIVATE cxx_std_20)

target_compile_definitions(IxtreemeEngine
    PRIVATE
        $<$<BOOL:${IXTREEME_WITH_EDITOR}>:IXTREEME_WITH_EDITOR=1>
        $<$<BOOL:${IXTREEME_VERBOSE_DIAGNOSTICS}>:IXTREEME_DEBUG_LOGS=1>
)

if(WIN32)
    if(TARGET IXEngineShaders)
        add_dependencies(IxtreemeEngine IXEngineShaders)
    endif()

    set_target_properties(IxtreemeEngine PROPERTIES
        WIN32_EXECUTABLE "$<NOT:$<CONFIG:Debug>>"
    )

    add_custom_command(TARGET IxtreemeEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CLIENT_ROOT}/assets"
            "$<TARGET_FILE_DIR:IxtreemeEngine>/assets"
    )
endif()
