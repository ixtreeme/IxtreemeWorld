add_executable(IwSelfTest
    src/main.cpp
)

target_link_libraries(IwSelfTest
    PRIVATE
        IXEngineRender
)

target_compile_features(IwSelfTest PRIVATE cxx_std_20)

target_compile_definitions(IwSelfTest
    PRIVATE
        IW_CLIENT_SOURCE_ROOT="${CLIENT_ROOT}"
        $<$<BOOL:${IXTREEME_WITH_EDITOR}>:IXTREEME_WITH_EDITOR=1>
)

if(WIN32)
    set_target_properties(IwSelfTest PROPERTIES
        WIN32_EXECUTABLE FALSE
    )

    if(TARGET IXEngineShaders)
        add_dependencies(IwSelfTest IXEngineShaders)
    endif()
endif()

if(BUILD_TESTING)
    add_test(NAME IwSelfTest_Offline
        COMMAND IwSelfTest --asset --render --client-root "${CLIENT_ROOT}"
    )
endif()
