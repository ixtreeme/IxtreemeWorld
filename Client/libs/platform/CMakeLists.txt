add_library(client_platform STATIC
    Debug.cpp
    Debug.h
    InputEvent.h
    NativeWindow.h
    VulkanDevice.cpp
    VulkanDevice.h
)

if(WIN32)
    target_sources(client_platform PRIVATE
        NativeWindow_Win32.cpp
        NativeWindow_Win32.h
    )
    target_link_libraries(client_platform PUBLIC
        Vulkan::Vulkan
        Gdi32
        Shell32
    )
    target_compile_definitions(client_platform PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
    )
elseif(ANDROID)
    target_sources(client_platform PRIVATE
        NativeWindow_Android.cpp
        NativeWindow_Android.h
        SoftKeyboard.cpp
        SoftKeyboard.h
        "${_ANDROID_NDK_ROOT}/sources/android/native_app_glue/android_native_app_glue.c"
    )
    target_include_directories(client_platform PUBLIC
        "${_ANDROID_NDK_ROOT}/sources/android/native_app_glue"
    )
    target_link_libraries(client_platform PUBLIC
        android
        log
        vulkan
    )
endif()

target_include_directories(client_platform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_compile_features(client_platform PUBLIC cxx_std_20)
