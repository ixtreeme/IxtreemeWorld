# IXRHISmoke — current headless contract + conversion checks (CTest).
# GPU device/buffer/pipeline tests need a window + swapchain; they run through
# the migrated renderers in-editor instead of being faked here.

add_executable(IXRHISmoke
    IXRHISmoke.cpp
    IXRHIConversionsTest.cpp
)

target_link_libraries(IXRHISmoke
    PRIVATE
        IXEngineIXRHI
        IXEngineVulkan
)

target_compile_definitions(IXRHISmoke PRIVATE IXRHI_SMOKE_WITH_VULKAN=1)
target_compile_features(IXRHISmoke PRIVATE cxx_std_20)

add_test(NAME IXRHISmoke COMMAND IXRHISmoke)
