add_library(IXEnginePlatform STATIC
    InputEvent.h
    NativeWindow.h
    platform/trash.h
    platform/dynamic_library.h
    platform/process.h
    platform/open_external.h
    Trash_Stub.cpp
    DynamicLibrary_Posix.cpp
    Process_Posix.cpp
    OpenExternal_Posix.cpp
    VulkanDevice.cpp
    VulkanDevice.h
)

if(WIN32)
    target_sources(IXEnginePlatform PRIVATE
        NativeWindow_Win32.cpp
        NativeWindow_Win32.h
        Trash_Win32.cpp
        DynamicLibrary_Win32.cpp
        Process_Win32.cpp
        OpenExternal_Win32.cpp
    )
    target_link_libraries(IXEnginePlatform PUBLIC
        Vulkan::Vulkan
        Gdi32
        Shell32
    )
    target_compile_definitions(IXEnginePlatform PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
    )
elseif(ANDROID)
    target_sources(IXEnginePlatform PRIVATE
        NativeWindow_Android.cpp
        NativeWindow_Android.h
        SoftKeyboard.cpp
        SoftKeyboard.h
        "${_ANDROID_NDK_ROOT}/sources/android/native_app_glue/android_native_app_glue.c"
    )
    target_include_directories(IXEnginePlatform PUBLIC
        "${_ANDROID_NDK_ROOT}/sources/android/native_app_glue"
    )
    target_link_libraries(IXEnginePlatform PUBLIC
        android
        log
        vulkan
    )
endif()

target_link_libraries(IXEnginePlatform
    PUBLIC
        IXEngineDebug
        ${CMAKE_DL_LIBS}   # dlopen/dlsym for DynamicLibrary on POSIX (empty on Windows)
)

target_include_directories(IXEnginePlatform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_compile_features(IXEnginePlatform PUBLIC cxx_std_20)
