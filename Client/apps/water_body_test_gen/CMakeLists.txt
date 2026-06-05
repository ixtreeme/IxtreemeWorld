add_executable(IwWaterBodyTestGen
    src/main.cpp
)

target_link_libraries(IwWaterBodyTestGen
    PRIVATE
        client_render
)

target_compile_features(IwWaterBodyTestGen PRIVATE cxx_std_20)

if(WIN32)
    set_target_properties(IwWaterBodyTestGen PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
endif()
