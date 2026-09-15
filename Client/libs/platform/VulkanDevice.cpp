#include "VulkanDevice.h"

#include "IXVulkanSurface.h" // backend-owned surface creation (no link cycle: header-only)

#include <algorithm>
#include <array>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace
{
const std::vector<const char*> kDeviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

void Log(const char* text)
{
#if defined(_WIN32)
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
#elif defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "IxtreemeClient", "%s", text);
#endif
    std::fprintf(stderr, "%s\n", text);
}

void LogFormat(const char* format, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(buffer);
}

const char* VkResultName(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "UNKNOWN_VK_RESULT";
    }
}

const char* VkFormatName(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_D24_UNORM_S8_UINT: return "VK_FORMAT_D24_UNORM_S8_UINT";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "VK_FORMAT_D32_SFLOAT_S8_UINT";
    case VK_FORMAT_D16_UNORM_S8_UINT: return "VK_FORMAT_D16_UNORM_S8_UINT";
    default: return "UNKNOWN_FORMAT";
    }
}

const char* SurfaceTransformName(VkSurfaceTransformFlagBitsKHR transform)
{
    switch (transform)
    {
    case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR: return "IDENTITY";
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: return "ROTATE_90";
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return "ROTATE_180";
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return "ROTATE_270";
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR: return "MIRROR";
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR: return "MIRROR_ROTATE_90";
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR: return "MIRROR_ROTATE_180";
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR: return "MIRROR_ROTATE_270";
    case VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR: return "INHERIT";
    default: return "UNKNOWN";
    }
}

const char* PresentModeName(VkPresentModeKHR mode)
{
    switch (mode)
    {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "UNKNOWN_PRESENT_MODE";
    }
}

VkPresentModeKHR ChooseUncappedPresentMode(const std::vector<VkPresentModeKHR>& presentModes)
{
    const auto supports = [&](VkPresentModeKHR mode) {
        return std::find(presentModes.begin(), presentModes.end(), mode) != presentModes.end();
    };

    if (supports(VK_PRESENT_MODE_IMMEDIATE_KHR))
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (supports(VK_PRESENT_MODE_MAILBOX_KHR))
        return VK_PRESENT_MODE_MAILBOX_KHR;
    if (supports(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

const char* VkImageLayoutName(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
    case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_STENCIL_READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC_OPTIMAL";
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST_OPTIMAL";
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT_SRC_KHR";
    default: return "UNKNOWN_LAYOUT";
    }
}

template <typename HandleT>
unsigned long long VkHandleBits(HandleT handle)
{
#if defined(VK_USE_64_BIT_PTR_DEFINES)
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handle));
#else
    return static_cast<unsigned long long>(handle);
#endif
}

bool IsQuarterTurn(VkSurfaceTransformFlagBitsKHR transform)
{
    return transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
        transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ||
        transform == VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR ||
        transform == VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR;
}

void CheckVk(VkResult result, const char* call, const char* file, int line)
{
    if (result == VK_SUCCESS)
        return;

    LogFormat("%s:%d: Vulkan call failed: %s -> %s (%d)", file, line, call, VkResultName(result), result);
    std::abort();
}

#define VK_CHECK(call) CheckVk((call), #call, __FILE__, __LINE__)

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*)
{
    const char* prefix = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "Vulkan validation error" : "Vulkan validation";
    LogFormat("%s: %s", prefix, callbackData->pMessage);
    return VK_FALSE;
}

bool ValidationLayerAvailable()
{
    uint32_t count = 0;
    VK_CHECK(vkEnumerateInstanceLayerProperties(&count, nullptr));
    std::vector<VkLayerProperties> layers(count);
    VK_CHECK(vkEnumerateInstanceLayerProperties(&count, layers.data()));

    for (const auto& layer : layers)
    {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
            return true;
    }
    return false;
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& ext) {
        return std::strcmp(ext.extensionName, name) == 0;
    });
}
}

bool VulkanDevice::Create(NativeWindow& window, uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;

    // NOTE (Phase 3C): frame sync objects, command pool/buffers and the
    // timestamp pool were deleted with the dormant frame loop — the backend
    // owns per-frame state now (IXVulkanDevice frame authority).
    if (!CreateInstance(window) || !CreateDebugMessenger() || !CreateSurface(window) ||
        !PickPhysicalDevice() || !CreateLogicalDevice() || !CreateSwapchainObjects(width, height))
    {
        Destroy();
        return false;
    }

    Log("[FRAMES-IN-FLIGHT] frame authority = IXVulkanDevice (legacy loop dormant)");
    return true;
}


// ---------------------------------------------------------------------------
// Phase-3C deletion: the legacy frame loop (BeginFrame, BeginSwapchainRenderPass,
// EndFrame, Resize) was removed. IXVulkanDevice owns acquisition, recording,
// submission, presentation and timestamps; this object keeps device/queue/
// swapchain-handle infrastructure plus backend-synced migration shims.
// ---------------------------------------------------------------------------

void VulkanDevice::WaitIdle()
{
    if (m_device)
        VK_CHECK(vkDeviceWaitIdle(m_device));
}

void VulkanDevice::SetMigrationFrameState(VkCommandBuffer activeCmd,
                                         uint32_t frameIndex,
                                         uint32_t imageIndex,
                                         uint64_t frameNumber,
                                         uint64_t safeFrameNumber,
                                         bool frameActive)
{
    m_migrationActiveCmd = activeCmd;
    m_currentFrame = frameIndex;
    m_imageIndex = imageIndex;
    m_frameNumber = frameNumber;
    m_safeFrameNumber = safeFrameNumber;
    m_frameStarted = frameActive;
    m_skipFrame = !frameActive;
}

VkImage VulkanDevice::GetSwapchainImage(uint32_t index) const
{
    return index < m_swapchainImages.size() ? m_swapchainImages[index] : VK_NULL_HANDLE;
}

VkImageView VulkanDevice::GetSwapchainImageView(uint32_t index) const
{
    return index < m_swapchainImageViews.size() ? m_swapchainImageViews[index] : VK_NULL_HANDLE;
}

void VulkanDevice::Destroy()
{
    if (!m_device)
        return;
    WaitIdle();

    // NOTE (Phase 3C): frame sync objects, command pool/buffers and the
    // timestamp pool were deleted with the dormant frame loop — the backend
    // destroys its own (IXVulkanDevice::Shutdown runs before this).
    DestroySwapchainObjects();

    if (m_device)
        vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;

    if (m_debugMessenger)
    {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy)
            destroy(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }

    if (m_surface)
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    m_surface = VK_NULL_HANDLE;

    if (m_instance)
        vkDestroyInstance(m_instance, nullptr);
    m_instance = VK_NULL_HANDLE;
}

bool VulkanDevice::CreateInstance(NativeWindow& window)
{
#ifdef _DEBUG
    m_validationEnabled = ValidationLayerAvailable();
    if (!m_validationEnabled)
        Log("VK_LAYER_KHRONOS_validation unavailable; continuing without validation.");
#endif

    const char* surfaceExtension = window.DescribeNative().vulkanSurfaceExtension;
    if (surfaceExtension == nullptr)
    {
        Log("Window provides no Vulkan surface extension.");
        return false;
    }
    std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, surfaceExtension};
    uint32_t extensionCount = 0;
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data()));

    if (!HasExtension(availableExtensions, VK_KHR_SURFACE_EXTENSION_NAME) ||
        !HasExtension(availableExtensions, surfaceExtension))
    {
        Log("Required Vulkan surface extensions are not available.");
        return false;
    }

    if (m_validationEnabled)
    {
        if (HasExtension(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        else
            Log("VK_EXT_debug_utils is unavailable; validation remains enabled without a debug messenger.");
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Standalone Vulkan Clear";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "None";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_2;

    const char* validationLayer = "VK_LAYER_KHRONOS_validation";

    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &app;
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    create.enabledLayerCount = m_validationEnabled ? 1u : 0u;
    create.ppEnabledLayerNames = m_validationEnabled ? &validationLayer : nullptr;

    VK_CHECK(vkCreateInstance(&create, nullptr, &m_instance));
    return true;
}

bool VulkanDevice::CreateDebugMessenger()
{
    if (!m_validationEnabled)
        return true;

    VkDebugUtilsMessengerCreateInfoEXT create{};
    create.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    create.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    create.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    create.pfnUserCallback = DebugCallback;

    auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!fn)
    {
        Log("VK_EXT_debug_utils function unavailable; validation messenger not installed.");
        return true;
    }

    VK_CHECK(fn(m_instance, &create, nullptr, &m_debugMessenger));
    return true;
}

bool VulkanDevice::CreateSurface(NativeWindow& window)
{
    // Surface creation logic lives in the IXVulkan backend (Phase 3C); legacy
    // code only holds the resulting handle. See IXVulkanSurface.
    VK_CHECK(ixvulkan::CreateSurfaceForWindow(window.DescribeNative(), m_instance, &m_surface));
    return true;
}

bool VulkanDevice::PickPhysicalDevice()
{
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &count, nullptr));
    if (count == 0)
    {
        Log("No Vulkan physical devices found.");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &count, devices.data()));

    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    QueueFamilies fallbackFamilies{};

    for (VkPhysicalDevice device : devices)
    {
        QueueFamilies families{};
        if (!IsDeviceSuitable(device, &families))
            continue;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            m_physicalDevice = device;
            m_queueFamilies = families;
            LogFormat("Using Vulkan device: %s", props.deviceName);
            return true;
        }

        if (!fallback)
        {
            fallback = device;
            fallbackFamilies = families;
        }
    }

    if (!fallback)
    {
        Log("No suitable Vulkan device found.");
        return false;
    }

    m_physicalDevice = fallback;
    m_queueFamilies = fallbackFamilies;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    LogFormat("Using Vulkan device: %s", props.deviceName);
    return true;
}

bool VulkanDevice::CreateLogicalDevice()
{
    std::set<uint32_t> uniqueFamilies = {m_queueFamilies.graphics, m_queueFamilies.present};
    std::vector<VkDeviceQueueCreateInfo> queueCreates;
    const float priority = 1.0f;

    for (uint32_t family : uniqueFamilies)
    {
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        queueCreates.push_back(queue);
    }

    VkDeviceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.queueCreateInfoCount = static_cast<uint32_t>(queueCreates.size());
    create.pQueueCreateInfos = queueCreates.data();
    create.enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size());
    create.ppEnabledExtensionNames = kDeviceExtensions.data();

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &supported);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);

    VkPhysicalDeviceFeatures enabled{};
    if (supported.fillModeNonSolid)
    {
        enabled.fillModeNonSolid = VK_TRUE;
    }
    else
    {
        Log("Vulkan device does not support fillModeNonSolid; wireframe/line polygon modes are disabled.");
    }
    if (supported.samplerAnisotropy)
    {
        enabled.samplerAnisotropy = VK_TRUE;
        m_samplerAnisotropySupported = true;
        m_maxSamplerAnisotropy = std::max(1.0f, properties.limits.maxSamplerAnisotropy);
        LogFormat("[VULKAN] samplerAnisotropy enabled max=%.1f", m_maxSamplerAnisotropy);
    }
    else
    {
        m_samplerAnisotropySupported = false;
        m_maxSamplerAnisotropy = 1.0f;
        Log("[VULKAN] samplerAnisotropy unsupported; using linear filtering fallback.");
    }
    create.pEnabledFeatures = &enabled;

    VK_CHECK(vkCreateDevice(m_physicalDevice, &create, nullptr, &m_device));
    vkGetDeviceQueue(m_device, m_queueFamilies.graphics, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_queueFamilies.present, 0, &m_presentQueue);
    return true;
}

bool VulkanDevice::CreateSwapchainObjects(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return true;

    // NOTE (Phase 3C): the main render pass + framebuffers are backend-owned
    // (IXVulkanSwapchain). Legacy CreateRenderPass/CreateFramebuffers are
    // dormant; m_renderPass is a backend-synced mirror, m_framebuffers unused.
    return CreateSwapchain(width, height) && CreateImageViews() && CreateDepthStencilImages();
}

bool VulkanDevice::CreateSwapchain(uint32_t width, uint32_t height)
{
    SwapchainSupport support = QuerySwapchainSupport(m_physicalDevice);
    if (support.formats.empty() || support.presentModes.empty())
    {
        Log("[VULKAN] Swap-chain create skipped: surface has no formats or present modes available.");
        m_swapchainDirty = true;
        return false;
    }

    VkSurfaceFormatKHR format = ChooseSurfaceFormat(support.formats);
    VkExtent2D extent = ChooseExtent(support.capabilities, width, height);
    VkSurfaceTransformFlagBitsKHR preTransform = support.capabilities.currentTransform;

    Log("[VULKAN] Surface capabilities:");
    LogFormat("[VULKAN]   currentExtent = %u x %u",
        support.capabilities.currentExtent.width,
        support.capabilities.currentExtent.height);
    LogFormat("[VULKAN]   currentTransform = %s (0x%x)",
        SurfaceTransformName(support.capabilities.currentTransform),
        static_cast<unsigned int>(support.capabilities.currentTransform));
    LogFormat("[VULKAN]   supportedTransforms = 0x%x",
        static_cast<unsigned int>(support.capabilities.supportedTransforms));
    LogFormat("[VULKAN]   minImageCount = %u, maxImageCount = %u",
        support.capabilities.minImageCount,
        support.capabilities.maxImageCount);

    if (IsQuarterTurn(support.capabilities.currentTransform))
    {
        if (support.capabilities.currentExtent.width == UINT32_MAX)
        {
            std::swap(extent.width, extent.height);
            LogFormat("[VULKAN] Quarter-turn surface transform: swapped render extent to %u x %u",
                extent.width,
                extent.height);
        }
        else if (support.capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        {
            preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            Log("[VULKAN] Quarter-turn fixed currentExtent: using IDENTITY preTransform so Android compositor handles rotation.");
        }
        else
        {
            Log("[VULKAN] Quarter-turn fixed currentExtent without IDENTITY support: shader pre-rotation will be needed if output is rotated.");
        }
    }

    // min+1 avoids stalling on the exact minimum while respecting platform maxImageCount.
    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0)
        imageCount = std::min(imageCount, support.capabilities.maxImageCount);

    VkSwapchainCreateInfoKHR create{};
    create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create.surface = m_surface;
    create.minImageCount = imageCount;
    create.imageFormat = format.format;
    create.imageColorSpace = format.colorSpace;
    create.imageExtent = extent;
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t families[] = {m_queueFamilies.graphics, m_queueFamilies.present};
    if (m_queueFamilies.graphics != m_queueFamilies.present)
    {
        create.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create.queueFamilyIndexCount = 2;
        create.pQueueFamilyIndices = families;
    }
    else
    {
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    create.preTransform = preTransform;
    create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create.presentMode = ChooseUncappedPresentMode(support.presentModes);
    m_swapchainPresentMode = create.presentMode;
    create.clipped = VK_TRUE;

    LogFormat("[VULKAN] Swap-chain create: preTransform=%s presentMode=%s uncapped=%s imageExtent=%u x %u requestedWindow=%u x %u",
        SurfaceTransformName(create.preTransform),
        PresentModeName(create.presentMode),
        create.presentMode == VK_PRESENT_MODE_FIFO_KHR ? "no" : "yes",
        create.imageExtent.width,
        create.imageExtent.height,
        width,
        height);

    const VkResult createResult = vkCreateSwapchainKHR(m_device, &create, nullptr, &m_swapchain);
    if (createResult == VK_ERROR_SURFACE_LOST_KHR ||
        createResult == VK_ERROR_OUT_OF_DATE_KHR ||
        createResult == VK_ERROR_INITIALIZATION_FAILED)
    {
        LogFormat("[VULKAN] Swap-chain create deferred: %s", VkResultName(createResult));
        m_swapchainDirty = true;
        m_swapchain = VK_NULL_HANDLE;
        return false;
    }
    CheckVk(createResult, "vkCreateSwapchainKHR", __FILE__, __LINE__);

    VK_CHECK(vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr));
    m_swapchainImages.resize(imageCount);
    VK_CHECK(vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data()));

    m_swapchainFormat = format.format;
    m_swapchainExtent = extent;
    m_currentTransform = support.capabilities.currentTransform;

    LogFormat("[VULKAN] Swap-chain created with preTransform=%s presentMode=%s imageExtent=%u x %u imageCount=%u",
        SurfaceTransformName(preTransform),
        PresentModeName(create.presentMode),
        m_swapchainExtent.width,
        m_swapchainExtent.height,
        imageCount);
    std::string imageList;
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        char item[96]{};
        std::snprintf(item, sizeof(item), "%sidx%u=0x%llx", i == 0 ? "" : ", ", i, VkHandleBits(m_swapchainImages[i]));
        imageList += item;
    }
    LogFormat("[SWP-DIAG] swapchain created handle=0x%llx imageCount=%u extent=%ux%u images=[%s]",
        VkHandleBits(m_swapchain),
        imageCount,
        m_swapchainExtent.width,
        m_swapchainExtent.height,
        imageList.c_str());
    LogFormat("[SWP-DIAG] INIT transition loop over swapchain images count=0 beforeAnyAcquire=yes swapchain=0x%llx",
        VkHandleBits(m_swapchain));

    // NOTE (Phase 3C): per-image present semaphores are backend-owned now
    // (IXVulkanDevice frame authority); legacy m_renderFinished is deleted.
    return true;
}

bool VulkanDevice::CreateImageViews()
{
    m_swapchainImageViews.resize(m_swapchainImages.size());
    for (size_t i = 0; i < m_swapchainImages.size(); ++i)
    {
        VkImageViewCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        create.image = m_swapchainImages[i];
        create.viewType = VK_IMAGE_VIEW_TYPE_2D;
        create.format = m_swapchainFormat;
        create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        create.subresourceRange.baseMipLevel = 0;
        create.subresourceRange.levelCount = 1;
        create.subresourceRange.baseArrayLayer = 0;
        create.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(m_device, &create, nullptr, &m_swapchainImageViews[i]));
    }
    return true;
}

bool VulkanDevice::CreateDepthStencilImages()
{
    m_depthStencilFormat = FindDepthStencilFormat();
    LogFormat("Using combined Vulkan depth/stencil format: %s", VkFormatName(m_depthStencilFormat));
    m_depthStencilImages.resize(m_swapchainImages.size());
    m_depthStencilMemory.resize(m_swapchainImages.size());
    m_depthStencilImageViews.resize(m_swapchainImages.size());

    for (size_t i = 0; i < m_swapchainImages.size(); ++i)
    {
        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.extent = {m_swapchainExtent.width, m_swapchainExtent.height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.format = m_depthStencilFormat;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateImage(m_device, &image, nullptr, &m_depthStencilImages[i]));

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, m_depthStencilImages[i], &req);

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(m_device, &alloc, nullptr, &m_depthStencilMemory[i]));
        VK_CHECK(vkBindImageMemory(m_device, m_depthStencilImages[i], m_depthStencilMemory[i], 0));

        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = m_depthStencilImages[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = m_depthStencilFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &m_depthStencilImageViews[i]));
    }
    return true;
}

// NOTE (Phase 3C): legacy CreateRenderPass/CreateFramebuffers were deleted with the
// dormant frame loop. The backend builds compatible passes/framebuffers itself
// (IXVulkanDevice::CreateCompatRenderPass/CreateFramebufferFor).

// ---------------------------------------------------------------------------
// Phase-3C deletion: dormant frame-loop, sync-object, timestamp and image-index
// helpers were removed (BeginFrame/EndFrame/BeginSwapchainRenderPass/Resize,
// CreateCommandPool/Buffers/SyncObjects/TimestampQueryPool, capture flow,
// FindSwapchainImageIndex/LogSwapchainImageTransition). IXVulkanDevice owns
// acquisition, recording, submission, presentation, sync and timestamps.
// ---------------------------------------------------------------------------


VulkanDevice::QueueFamilies VulkanDevice::FindQueueFamilies(VkPhysicalDevice device) const
{
    QueueFamilies families{};
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props.data());

    for (uint32_t i = 0; i < count; ++i)
    {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            families.graphics = i;

        VkBool32 present = VK_FALSE;
        VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present));
        if (present)
            families.present = i;

        if (families.Complete())
            break;
    }
    return families;
}

VulkanDevice::SwapchainSupport VulkanDevice::QuerySwapchainSupport(VkPhysicalDevice device) const
{
    SwapchainSupport support{};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface, &support.capabilities);
    if (result != VK_SUCCESS)
    {
        LogFormat("[VULKAN] Surface capabilities unavailable: %s", VkResultName(result));
        return support;
    }

    uint32_t count = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &count, nullptr);
    if (result != VK_SUCCESS)
    {
        LogFormat("[VULKAN] Surface formats unavailable: %s", VkResultName(result));
        return support;
    }
    support.formats.resize(count);
    if (count)
    {
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &count, support.formats.data());
        if (result != VK_SUCCESS)
        {
            LogFormat("[VULKAN] Surface formats fetch failed: %s", VkResultName(result));
            support.formats.clear();
            return support;
        }
    }

    result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &count, nullptr);
    if (result != VK_SUCCESS)
    {
        LogFormat("[VULKAN] Surface present modes unavailable: %s", VkResultName(result));
        support.formats.clear();
        return support;
    }
    support.presentModes.resize(count);
    if (count)
    {
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &count, support.presentModes.data());
        if (result != VK_SUCCESS)
        {
            LogFormat("[VULKAN] Surface present modes fetch failed: %s", VkResultName(result));
            support.presentModes.clear();
            return support;
        }
    }
    return support;
}

void VulkanDevice::DestroySwapchainObjects()
{
    if (m_swapchain)
    {
        LogFormat("[SWP-DIAG] destroy swapchain handle=0x%llx imageCount=%zu frame=%llu",
            VkHandleBits(m_swapchain),
            m_swapchainImages.size(),
            static_cast<unsigned long long>(m_frameNumber));
    }

    // NOTE (Phase 3C): framebuffers + render pass are backend-owned now (see
    // IXVulkanSwapchain); legacy must not destroy them. m_renderPass is a
    // backend-synced mirror cleared by the backend.

    for (VkImageView view : m_depthStencilImageViews)
        vkDestroyImageView(m_device, view, nullptr);
    m_depthStencilImageViews.clear();

    for (VkImage image : m_depthStencilImages)
        vkDestroyImage(m_device, image, nullptr);
    m_depthStencilImages.clear();

    for (VkDeviceMemory memory : m_depthStencilMemory)
        vkFreeMemory(m_device, memory, nullptr);
    m_depthStencilMemory.clear();
    m_depthStencilFormat = VK_FORMAT_UNDEFINED;

    for (VkImageView view : m_swapchainImageViews)
        vkDestroyImageView(m_device, view, nullptr);
    m_swapchainImageViews.clear();

    if (m_swapchain)
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
    m_swapchainImages.clear();
}

bool VulkanDevice::RecreateSwapchain(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return true;

    LogFormat("[SWP-DIAG] recreate begin frame=%llu oldSwapchain=0x%llx extent=%ux%u requested=%ux%u",
        static_cast<unsigned long long>(m_frameNumber),
        VkHandleBits(m_swapchain),
        m_swapchainExtent.width,
        m_swapchainExtent.height,
        width,
        height);
    VK_CHECK(vkDeviceWaitIdle(m_device));
    DestroySwapchainObjects();
    m_swapchainDirty = false;
    return CreateSwapchainObjects(width, height);
}

bool VulkanDevice::IsDeviceSuitable(VkPhysicalDevice device, QueueFamilies* outFamilies) const
{
    QueueFamilies families = FindQueueFamilies(device);
    if (!families.Complete() || !CheckDeviceExtensionSupport(device))
        return false;

    SwapchainSupport support = QuerySwapchainSupport(device);
    if (support.formats.empty() || support.presentModes.empty())
        return false;

    *outFamilies = families;
    return true;
}

bool VulkanDevice::CheckDeviceExtensionSupport(VkPhysicalDevice device) const
{
    uint32_t count = 0;
    VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> extensions(count);
    VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()));

    for (const char* required : kDeviceExtensions)
        if (!HasExtension(extensions, required))
            return false;
    return true;
}

VkSurfaceFormatKHR VulkanDevice::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const
{
    for (const auto& format : formats)
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;
    return formats[0];
}

VkExtent2D VulkanDevice::ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, uint32_t width, uint32_t height) const
{
    if (caps.currentExtent.width != UINT32_MAX)
        return caps.currentExtent;

    VkExtent2D extent{width, height};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return extent;
}

VkFormat VulkanDevice::FindDepthStencilFormat() const
{
    const VkFormat candidates[] = {
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D16_UNORM_S8_UINT,
    };

    for (VkFormat format : candidates)
    {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return format;
    }

    Log("No supported Vulkan depth/stencil format with 8-bit stencil was found.");
    std::abort();
}

uint32_t VulkanDevice::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &mem);

    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((typeFilter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties)
            return i;

    Log("No compatible Vulkan memory type was found.");
    std::abort();
}

bool VulkanDevice::IsSwapchainFormatSrgb() const
{
    switch (m_swapchainFormat)
    {
    case VK_FORMAT_R8_SRGB:
    case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R8G8B8_SRGB:
    case VK_FORMAT_B8G8R8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}
