if(ANDROID)
    add_library(VulkanClear SHARED
        src/main.cpp
    )

    set_target_properties(VulkanClear PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    target_link_options(VulkanClear PRIVATE
        "-u" "ANativeActivity_onCreate"
    )
else()
    add_executable(VulkanClear
        src/main.cpp
    )
endif()

target_link_libraries(VulkanClear
    PRIVATE
        client_render
        client_network
)

target_compile_features(VulkanClear PRIVATE cxx_std_20)

if(WIN32)
    if(TARGET ClientShaders)
        add_dependencies(VulkanClear ClientShaders)
    endif()

    set_target_properties(VulkanClear PROPERTIES
        WIN32_EXECUTABLE "$<NOT:$<CONFIG:Debug>>"
    )

    add_custom_command(TARGET VulkanClear POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NOESIS_SDK}/Bin/windows_x86_64/Noesis.dll"
            "$<TARGET_FILE_DIR:VulkanClear>/Noesis.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CLIENT_ROOT}/assets"
            "$<TARGET_FILE_DIR:VulkanClear>/assets"
    )
endif()
