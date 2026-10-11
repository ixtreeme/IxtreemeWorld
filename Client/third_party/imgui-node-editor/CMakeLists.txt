add_library(IXImGuiNodeEditor STATIC
    imgui_canvas.cpp
    imgui_node_editor.cpp
    imgui_node_editor_api.cpp
)
target_include_directories(IXImGuiNodeEditor SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(IXImGuiNodeEditor PUBLIC imgui::imgui IXMaterialGraph)
target_compile_features(IXImGuiNodeEditor PUBLIC cxx_std_14)
if(MSVC)
    target_compile_options(IXImGuiNodeEditor PRIVATE /W0)
endif()
