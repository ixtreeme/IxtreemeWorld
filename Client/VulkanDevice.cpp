#include "VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace
{
const std::vector<const char*> kDeviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

void Log(const char* text)
{
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
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

bool VulkanDevice::Create(HWND hwnd, uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;

    if (!CreateInstance() || !CreateDebugMessenger() || !CreateSurface(hwnd) ||
        !PickPhysicalDevice() || !CreateLogicalDevice() ||
        !CreateSwapchainObjects(width, height) || !CreateCommandPool() ||
        !CreateCommandBuffers() || !CreateSyncObjects())
    {
        Destroy();
        return false;
    }

    return true;
}

void VulkanDevice::BeginFrame()
{
    m_skipFrame = true;
    m_frameStarted = false;
    m_renderPassStarted = false;

    if (!m_device || m_width == 0 || m_height == 0 || m_swapchain == VK_NULL_HANDLE)
        return;

    if (m_swapchainDirty && !RecreateSwapchain(m_width, m_height))
        return;

    VK_CHECK(vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX));
    m_safeFrameNumber = m_frameNumber;

    const VkResult acquire = vkAcquireNextImageKHR(
        m_device,
        m_swapchain,
        UINT64_MAX,
        m_imageAvailable[m_currentFrame],
        VK_NULL_HANDLE,
        &m_imageIndex);

    if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
    {
        m_swapchainDirty = true;
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
        CheckVk(acquire, "vkAcquireNextImageKHR", __FILE__, __LINE__);

    m_acquiredSuboptimal = acquire == VK_SUBOPTIMAL_KHR;

    if (m_imagesInFlight[m_imageIndex] != VK_NULL_HANDLE)
        VK_CHECK(vkWaitForFences(m_device, 1, &m_imagesInFlight[m_imageIndex], VK_TRUE, UINT64_MAX));
    m_imagesInFlight[m_imageIndex] = m_inFlightFences[m_currentFrame];

    VK_CHECK(vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]));
    VK_CHECK(vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &begin));

    m_skipFrame = false;
    m_frameStarted = true;
}

void VulkanDevice::BeginSwapchainRenderPass()
{
    if (m_skipFrame || !m_frameStarted || m_renderPassStarted)
        return;

    VkClearValue clearValues[2]{};
    clearValues[0].color.float32[0] = 0.04f;
    clearValues[0].color.float32[1] = 0.05f;
    clearValues[0].color.float32[2] = 0.09f;
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = m_renderPass;
    pass.framebuffer = m_framebuffers[m_imageIndex];
    pass.renderArea.offset = {0, 0};
    pass.renderArea.extent = m_swapchainExtent;
    pass.clearValueCount = 2;
    pass.pClearValues = clearValues;

    // The pass loadOps clear color plus combined depth/stencil; Noesis draws into this same onscreen pass.
    vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &pass, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_swapchainExtent.width);
    viewport.height = static_cast<float>(m_swapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_commandBuffers[m_currentFrame], 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_swapchainExtent;
    vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &scissor);

    m_renderPassStarted = true;
}

void VulkanDevice::EndFrame()
{
    if (m_skipFrame || !m_frameStarted)
        return;

    if (!m_renderPassStarted)
        BeginSwapchainRenderPass();

    if (m_renderPassStarted)
    {
        vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        m_renderPassStarted = false;
    }

    VK_CHECK(vkEndCommandBuffer(m_commandBuffers[m_currentFrame]));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &m_imageAvailable[m_currentFrame];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_commandBuffers[m_currentFrame];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_renderFinished[m_imageIndex];

    // The fence protects CPU reuse of this frame's command buffer and sync objects.
    VK_CHECK(vkQueueSubmit(m_graphicsQueue, 1, &submit, m_inFlightFences[m_currentFrame]));

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m_renderFinished[m_imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &m_swapchain;
    present.pImageIndices = &m_imageIndex;

    const VkResult result = vkQueuePresentKHR(m_presentQueue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_acquiredSuboptimal)
    {
        m_swapchainDirty = true;
    }
    else if (result != VK_SUCCESS)
    {
        CheckVk(result, "vkQueuePresentKHR", __FILE__, __LINE__);
    }

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    ++m_frameNumber;
    m_frameStarted = false;
}

bool VulkanDevice::Resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;

    if (!m_device || width == 0 || height == 0)
    {
        m_swapchainDirty = width != 0 && height != 0;
        return true;
    }

    return RecreateSwapchain(width, height);
}

void VulkanDevice::WaitIdle()
{
    if (m_device)
        VK_CHECK(vkDeviceWaitIdle(m_device));
}

void VulkanDevice::Destroy()
{
    WaitIdle();

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        if (m_inFlightFences[i]) vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
        if (m_imageAvailable[i]) vkDestroySemaphore(m_device, m_imageAvailable[i], nullptr);
        m_inFlightFences[i] = VK_NULL_HANDLE;
        m_imageAvailable[i] = VK_NULL_HANDLE;
    }

    if (m_commandPool)
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    m_commandPool = VK_NULL_HANDLE;
    m_commandBuffers.clear();

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

bool VulkanDevice::CreateInstance()
{
#ifdef _DEBUG
    m_validationEnabled = ValidationLayerAvailable();
    if (!m_validationEnabled)
        Log("VK_LAYER_KHRONOS_validation unavailable; continuing without validation.");
#endif

    std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    uint32_t extensionCount = 0;
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data()));

    if (!HasExtension(availableExtensions, VK_KHR_SURFACE_EXTENSION_NAME) ||
        !HasExtension(availableExtensions, VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
    {
        Log("Required Win32 Vulkan surface extensions are not available.");
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

bool VulkanDevice::CreateSurface(HWND hwnd)
{
    VkWin32SurfaceCreateInfoKHR create{};
    create.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create.hinstance = GetModuleHandleA(nullptr);
    create.hwnd = hwnd;
    VK_CHECK(vkCreateWin32SurfaceKHR(m_instance, &create, nullptr, &m_surface));
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
    if (!supported.fillModeNonSolid)
    {
        Log("Selected Vulkan device does not support fillModeNonSolid required by Noesis VKRenderDevice.");
        return false;
    }

    VkPhysicalDeviceFeatures enabled{};
    enabled.fillModeNonSolid = VK_TRUE;
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

    return CreateSwapchain(width, height) && CreateImageViews() && CreateDepthStencilImages() &&
        CreateRenderPass() && CreateFramebuffers();
}

bool VulkanDevice::CreateSwapchain(uint32_t width, uint32_t height)
{
    SwapchainSupport support = QuerySwapchainSupport(m_physicalDevice);
    VkSurfaceFormatKHR format = ChooseSurfaceFormat(support.formats);
    VkExtent2D extent = ChooseExtent(support.capabilities, width, height);

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

    create.preTransform = support.capabilities.currentTransform;
    create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR; // FIFO is guaranteed and avoids tearing.
    create.clipped = VK_TRUE;

    VK_CHECK(vkCreateSwapchainKHR(m_device, &create, nullptr, &m_swapchain));
    VK_CHECK(vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr));
    m_swapchainImages.resize(imageCount);
    VK_CHECK(vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data()));

    m_swapchainFormat = format.format;
    m_swapchainExtent = extent;
    m_imagesInFlight.assign(imageCount, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo sem{};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    m_renderFinished.assign(imageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imageCount; ++i)
        VK_CHECK(vkCreateSemaphore(m_device, &sem, nullptr, &m_renderFinished[i]));

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

bool VulkanDevice::CreateRenderPass()
{
    VkAttachmentDescription color{};
    color.format = m_swapchainFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; // Presentation engine consumes this layout.

    VkAttachmentDescription depthStencil{};
    depthStencil.format = m_depthStencilFormat;
    depthStencil.samples = VK_SAMPLE_COUNT_1_BIT;
    depthStencil.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthStencil.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthStencil.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthStencil.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthStencil.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthStencil.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthStencilRef{};
    depthStencilRef.attachment = 1;
    depthStencilRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthStencilRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription attachments[] = {color, depthStencil};

    VkRenderPassCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create.attachmentCount = 2;
    create.pAttachments = attachments;
    create.subpassCount = 1;
    create.pSubpasses = &subpass;
    create.dependencyCount = 1;
    create.pDependencies = &dependency;

    VK_CHECK(vkCreateRenderPass(m_device, &create, nullptr, &m_renderPass));
    return true;
}

bool VulkanDevice::CreateFramebuffers()
{
    m_framebuffers.resize(m_swapchainImageViews.size());
    for (size_t i = 0; i < m_swapchainImageViews.size(); ++i)
    {
        VkImageView attachments[] = {m_swapchainImageViews[i], m_depthStencilImageViews[i]};
        VkFramebufferCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        create.renderPass = m_renderPass;
        create.attachmentCount = 2;
        create.pAttachments = attachments;
        create.width = m_swapchainExtent.width;
        create.height = m_swapchainExtent.height;
        create.layers = 1;
        VK_CHECK(vkCreateFramebuffer(m_device, &create, nullptr, &m_framebuffers[i]));
    }
    return true;
}

bool VulkanDevice::CreateCommandPool()
{
    VkCommandPoolCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    create.queueFamilyIndex = m_queueFamilies.graphics;
    VK_CHECK(vkCreateCommandPool(m_device, &create, nullptr, &m_commandPool));
    return true;
}

bool VulkanDevice::CreateCommandBuffers()
{
    m_commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = m_commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());
    VK_CHECK(vkAllocateCommandBuffers(m_device, &alloc, m_commandBuffers.data()));
    return true;
}

bool VulkanDevice::CreateSyncObjects()
{
    VkSemaphoreCreateInfo sem{};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        VK_CHECK(vkCreateSemaphore(m_device, &sem, nullptr, &m_imageAvailable[i]));
        VK_CHECK(vkCreateFence(m_device, &fence, nullptr, &m_inFlightFences[i]));
    }
    return true;
}

void VulkanDevice::DestroySwapchainObjects()
{
    for (VkSemaphore semaphore : m_renderFinished)
        vkDestroySemaphore(m_device, semaphore, nullptr);
    m_renderFinished.clear();

    for (VkFramebuffer framebuffer : m_framebuffers)
        vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    m_framebuffers.clear();

    if (m_renderPass)
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    m_renderPass = VK_NULL_HANDLE;

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
    m_imagesInFlight.clear();
}

bool VulkanDevice::RecreateSwapchain(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return true;

    VK_CHECK(vkDeviceWaitIdle(m_device));
    DestroySwapchainObjects();
    m_swapchainDirty = false;
    return CreateSwapchainObjects(width, height);
}

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
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface, &support.capabilities));

    uint32_t count = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &count, nullptr));
    support.formats.resize(count);
    if (count) VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &count, support.formats.data()));

    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &count, nullptr));
    support.presentModes.resize(count);
    if (count) VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &count, support.presentModes.data()));
    return support;
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
