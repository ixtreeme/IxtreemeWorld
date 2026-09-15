// IXVulkanDevice frame authority (Phase 3C, §4-16/20/23-25).
//
// This backend OWNS acquisition, per-frame command buffers, submission,
// presentation, frames-in-flight sync and GPU timestamps. The legacy
// VulkanDevice keeps device/queue/swapchain-handle infrastructure (§39 debt)
// plus per-frame migration shims synced below; its own loop is dormant.
//
// Sequencing mirrors the legacy implementation exactly (fence wait, acquire,
// image-fence adoption, reset/begin, submit wiring, present handling,
// capture-gated timestamps, CPU timing points).

#include "IXVulkanDevice.h"

#include "IXVulkanCommandList.h"
#include "IXVulkanConversions.h"
#include "IXVulkanRenderTarget.h"
#include "IXVulkanSwapchain.h"
#include "Debug.h"
#include "VulkanDevice.h"

#include <array>
#include <cassert>
#include <chrono>

namespace ixvulkan
{

bool IXVulkanDevice::EnsureFrameSlot(std::uint32_t slot)
{
    IXVulkanFrameSlot& context = m_slots[slot];
    if (context.pool != VK_NULL_HANDLE)
        return true;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_loop->GetGraphicsQueueFamily();
    CheckVk(vkCreateCommandPool(NativeDevice(), &poolInfo, nullptr, &context.pool),
        "vkCreateCommandPool(frame)",
        __FILE__,
        __LINE__);

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = context.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    CheckVk(vkAllocateCommandBuffers(NativeDevice(), &alloc, &context.cmd),
        "vkAllocateCommandBuffers(frame)",
        __FILE__,
        __LINE__);
    context.list = std::make_unique<IXVulkanCommandList>(*this, context.cmd);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first wait passes
    CheckVk(vkCreateFence(NativeDevice(), &fenceInfo, nullptr, &context.fence),
        "vkCreateFence(frame)",
        __FILE__,
        __LINE__);

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    CheckVk(vkCreateSemaphore(NativeDevice(), &semInfo, nullptr, &context.imageAvailable),
        "vkCreateSemaphore(acquire)",
        __FILE__,
        __LINE__);
    return true;
}

bool IXVulkanDevice::EnsureSwapchainObjects()
{
    const std::uint32_t imageCount = m_loop->GetSwapchainImageCount();
    if (imageCount == 0)
        return false;
    if (m_renderFinished.size() != imageCount)
    {
        for (VkSemaphore sem : m_renderFinished)
            vkDestroySemaphore(NativeDevice(), sem, nullptr);
        m_renderFinished.assign(imageCount, VK_NULL_HANDLE);
        for (VkSemaphore& sem : m_renderFinished)
        {
            VkSemaphoreCreateInfo semInfo{};
            semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            CheckVk(vkCreateSemaphore(NativeDevice(), &semInfo, nullptr, &sem),
                "vkCreateSemaphore(present)",
                __FILE__,
                __LINE__);
        }
        m_imagesInFlight.assign(imageCount, VK_NULL_HANDLE);
    }
    return true;
}

bool IXVulkanDevice::RecreateSwapchainNow(std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0)
        return false; // minimized: Skip without spinning, no dirty needed
    WaitIdle();
    m_swapchain->Teardown();
    if (!m_loop->RecreateSwapchain(width, height))
    {
        m_swapchainDirty = true;
        return false;
    }
    m_swapchain->Rebuild();
    if (!EnsureSwapchainObjects())
    {
        m_swapchainDirty = true;
        return false;
    }
    m_tracker.OnSwapchainRecreated();
    return true;
}

void IXVulkanDevice::TeardownFrameObjects()
{
    for (IXVulkanFrameSlot& context : m_slots)
    {
        context.list.reset();
        if (context.pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(NativeDevice(), context.pool, nullptr);
        context.pool = VK_NULL_HANDLE;
        context.cmd = VK_NULL_HANDLE;
        if (context.fence != VK_NULL_HANDLE)
            vkDestroyFence(NativeDevice(), context.fence, nullptr);
        context.fence = VK_NULL_HANDLE;
        if (context.imageAvailable != VK_NULL_HANDLE)
            vkDestroySemaphore(NativeDevice(), context.imageAvailable, nullptr);
        context.imageAvailable = VK_NULL_HANDLE;
    }
    for (VkSemaphore sem : m_renderFinished)
        vkDestroySemaphore(NativeDevice(), sem, nullptr);
    m_renderFinished.clear();
    m_imagesInFlight.clear();
    if (m_queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(NativeDevice(), m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }
}

void IXVulkanDevice::SyncLegacyFrameState() const
{
    // Keeps still-native renderers (terrain/skinned/RmlUi) working unchanged:
    // they read the active command buffer, indices and numbers from the
    // legacy object, which mirrors backend authority (deleted with them).
    // safeFrameNumber mirrors legacy: current frame number once its slot
    // fence has been waited (all older frames complete).
    m_loop->SetMigrationFrameState(m_frameActive ? m_slots[m_activeSlot].cmd : VK_NULL_HANDLE,
        m_tracker.GetSlot(),
        m_activeImage,
        m_tracker.GetFrameNumber(),
        m_tracker.GetFrameNumber(),
        m_frameActive);
}

ixrhi::IXRHIFrame IXVulkanDevice::BeginFrame()
{
    ixrhi::IXRHIFrame frame;
    if (m_shutDown || !m_tracker.CanBegin())
    {
        assert(!m_tracker.IsRecording() && "BeginFrame while a frame is active");
        return frame; // Skip
    }
    m_cpuFrameStart = std::chrono::steady_clock::now();
    const bool captureThisFrame = m_captureRequested;
    m_captureRequested = false;
    m_activeCpuTiming = ixrhi::IXRHICpuFrameTiming{};
    m_activeCpuTiming.valid = captureThisFrame;
    m_activeCpuTiming.frameNumber = m_tracker.GetFrameNumber();

    if (!m_haveRequestedSize)
    {
        // Seed from legacy startup state once; RequestResize owns it after.
        const VkExtent2D initial = m_loop->GetSwapchainExtent();
        m_requestedWidth = initial.width;
        m_requestedHeight = initial.height;
        m_haveRequestedSize = true;
    }
    if (m_pendingResize)
    {
        m_requestedWidth = m_pendingWidth;
        m_requestedHeight = m_pendingHeight;
        m_pendingResize = false;
    }

    if (NativeDevice() == VK_NULL_HANDLE)
    {
        m_tracker.OnAbort();
        SyncLegacyFrameState();
        return frame; // Skip
    }

    // Deferred creation (surface lost at startup), proactive resize and the
    // reactive dirty flag converge here (single authoritative flow, §73).
    // A successful (re)build reports SwapchainRecreated once so the app runs
    // resize orchestration before the first frame on the new swapchain.
    const VkExtent2D currentExtent = m_loop->GetSwapchainExtent();
    const bool swapchainMissing = m_loop->GetSwapchain() == VK_NULL_HANDLE;
    const bool sizeChanged = !swapchainMissing &&
        (currentExtent.width != m_requestedWidth || currentExtent.height != m_requestedHeight);
    if (swapchainMissing || sizeChanged || m_swapchainDirty)
    {
        m_swapchainDirty = false;
        if (!RecreateSwapchainNow(m_requestedWidth, m_requestedHeight))
        {
            m_tracker.OnAbort();
            SyncLegacyFrameState();
            return frame; // Skip (zero size or legacy failure); retry next frame
        }
        m_tracker.OnAbort();
        SyncLegacyFrameState();
        frame.result = ixrhi::IXRHIFrameResult::SwapchainRecreated;
        frame.info.swapchainGeneration = m_swapchain->Generation();
        return frame;
    }

    const VkExtent2D extent = m_loop->GetSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
    {
        m_tracker.OnAbort();
        SyncLegacyFrameState();
        return frame; // Skip
    }
    if (m_swapchain->Generation() == 0)
    {
        m_swapchain->Rebuild();
        EnsureSwapchainObjects();
    }
    if (m_swapchain->Generation() == 0 || m_loop->GetSwapchain() == VK_NULL_HANDLE)
    {
        m_tracker.OnAbort();
        SyncLegacyFrameState();
        return frame; // Skip: no usable swapchain yet
    }

    const std::uint32_t slot = m_tracker.GetSlot();
    if (!EnsureFrameSlot(slot) || !EnsureSwapchainObjects())
    {
        m_tracker.OnAbort();
        SyncLegacyFrameState();
        return frame; // Skip
    }
    IXVulkanFrameSlot& context = m_slots[slot];

    auto waitStart = std::chrono::steady_clock::now();
    CheckVk(vkWaitForFences(NativeDevice(), 1, &context.fence, VK_TRUE, UINT64_MAX),
        "vkWaitForFences(frame)",
        __FILE__,
        __LINE__);
    auto waitEnd = std::chrono::steady_clock::now();
    if (m_activeCpuTiming.valid)
        m_activeCpuTiming.waitForFencesMs +=
            std::chrono::duration<double, std::milli>(waitEnd - waitStart).count();

    auto acquireStart = std::chrono::steady_clock::now();
    std::uint32_t imageIndex = 0;
    const VkResult acquire = vkAcquireNextImageKHR(NativeDevice(),
        m_loop->GetSwapchain(),
        UINT64_MAX,
        context.imageAvailable,
        VK_NULL_HANDLE,
        &imageIndex);
    auto acquireEnd = std::chrono::steady_clock::now();
    if (m_activeCpuTiming.valid)
        m_activeCpuTiming.acquireImageMs =
            std::chrono::duration<double, std::milli>(acquireEnd - acquireStart).count();

    const ixrhi::IXRHIFrameResult acquireResult = TranslateFrameResult(acquire);
    if (acquireResult != ixrhi::IXRHIFrameResult::Success)
    {
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
            m_swapchainDirty = true;
        m_tracker.OnAbort();
        SyncLegacyFrameState();
        frame.result = acquireResult; // Skip or DeviceLost; SwapchainRecreated handled next Begin
        if (acquireResult == ixrhi::IXRHIFrameResult::SwapchainRecreated)
            frame.info.swapchainGeneration = m_swapchain->Generation();
        return frame;
    }
    static bool warnedAcquireSuboptimal = false;
    if (acquire == VK_SUBOPTIMAL_KHR && !warnedAcquireSuboptimal)
    {
        warnedAcquireSuboptimal = true;
        Tracen("[VULKAN] acquireNextImage: VK_SUBOPTIMAL_KHR (continuing)");
    }
    m_activeImage = imageIndex;

    if (m_imagesInFlight[imageIndex] != VK_NULL_HANDLE)
    {
        waitStart = std::chrono::steady_clock::now();
        CheckVk(vkWaitForFences(NativeDevice(), 1, &m_imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX),
            "vkWaitForFences(image)",
            __FILE__,
            __LINE__);
        waitEnd = std::chrono::steady_clock::now();
        if (m_activeCpuTiming.valid)
            m_activeCpuTiming.waitForFencesMs +=
                std::chrono::duration<double, std::milli>(waitEnd - waitStart).count();
    }
    m_imagesInFlight[imageIndex] = context.fence;

    CheckVk(vkResetFences(NativeDevice(), 1, &context.fence), "vkResetFences(frame)", __FILE__, __LINE__);
    CheckVk(vkResetCommandBuffer(context.cmd, 0), "vkResetCommandBuffer(frame)", __FILE__, __LINE__);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    CheckVk(vkBeginCommandBuffer(context.cmd, &begin), "vkBeginCommandBuffer(frame)", __FILE__, __LINE__);

    m_frameActive = true;
    m_activeSlot = slot;
    m_cpuWorkStart = std::chrono::steady_clock::now();
    m_activeToken = m_tracker.OnBegin(slot, m_tracker.GetFrameNumber());
    if (captureThisFrame)
        BeginCaptureForFrame();

    SyncLegacyFrameState();

    frame.result = ixrhi::IXRHIFrameResult::Success;
    frame.frameToken = m_activeToken;
    frame.info.result = ixrhi::IXRHIFrameResult::Success;
    frame.info.frameIndex = slot;
    frame.info.imageIndex = imageIndex;
    frame.info.targetWidth = extent.width;
    frame.info.targetHeight = extent.height;
    frame.info.frameActive = true;
    frame.info.frameNumber = m_tracker.GetFrameNumber();
    frame.info.commandList = context.list.get();
    frame.info.backBuffer = m_swapchain->Backbuffer(imageIndex);
    frame.info.swapchainGeneration = m_swapchain->Generation();
    return frame;
}

void IXVulkanDevice::EndFrame(const ixrhi::IXRHIFrame& frame)
{
    // Debug pairing: only the live frame may close, exactly once (§51/89).
    assert(m_tracker.IsRecording() && "EndFrame without an active BeginFrame");
    assert(frame.result == ixrhi::IXRHIFrameResult::Success && "EndFrame requires a successful frame");
    assert(frame.frameToken != 0 && frame.frameToken == m_activeToken && "EndFrame token mismatch");
    if (!m_tracker.IsRecording() || frame.frameToken != m_activeToken)
        return;

    const auto endFrameStart = std::chrono::steady_clock::now();
    if (m_activeCpuTiming.valid)
        m_activeCpuTiming.renderLoopCpuWorkMs =
            std::chrono::duration<double, std::milli>(endFrameStart - m_cpuWorkStart).count();

    IXVulkanFrameSlot& context = m_slots[m_activeSlot];
    if (m_captureActive)
        WriteTimestamp(static_cast<std::uint32_t>(ixrhi::IXRHITimestampPoint::FrameEnd));

    CheckVk(vkEndCommandBuffer(context.cmd), "vkEndCommandBuffer(frame)", __FILE__, __LINE__);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &context.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &context.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_renderFinished[m_activeImage];

    auto submitStart = std::chrono::steady_clock::now();
    CheckVk(vkQueueSubmit(m_loop->GetGraphicsQueue(), 1, &submit, context.fence),
        "vkQueueSubmit(frame)",
        __FILE__,
        __LINE__);
    auto submitEnd = std::chrono::steady_clock::now();
    if (m_activeCpuTiming.valid)
        m_activeCpuTiming.submitMs =
            std::chrono::duration<double, std::milli>(submitEnd - submitStart).count();

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m_renderFinished[m_activeImage];
    present.swapchainCount = 1;
    VkSwapchainKHR swapchain = m_loop->GetSwapchain();
    present.pSwapchains = &swapchain;
    present.pImageIndices = &m_activeImage;

    auto presentStart = std::chrono::steady_clock::now();
    const VkResult presentResult = vkQueuePresentKHR(m_loop->GetPresentQueue(), &present);
    auto presentEnd = std::chrono::steady_clock::now();
    if (m_activeCpuTiming.valid)
        m_activeCpuTiming.presentMs =
            std::chrono::duration<double, std::milli>(presentEnd - presentStart).count();
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        m_swapchainDirty = true;
    }
    else if (presentResult == VK_SUBOPTIMAL_KHR)
    {
        static bool warnedPresentSuboptimal = false;
        if (!warnedPresentSuboptimal)
        {
            warnedPresentSuboptimal = true;
            Tracen("[VULKAN] present: VK_SUBOPTIMAL_KHR (continuing)");
        }
    }
    else if (presentResult != VK_SUCCESS)
    {
        CheckVk(presentResult, "vkQueuePresentKHR", __FILE__, __LINE__);
    }

    if (m_activeCpuTiming.valid)
    {
        m_activeCpuTiming.totalCpuFrameMs =
            std::chrono::duration<double, std::milli>(presentEnd - m_cpuFrameStart).count();
        m_lastCpuTiming = m_activeCpuTiming;
    }
    FinishCaptureAfterSubmit();

    m_tracker.OnEnd();
    m_frameActive = false;
    SyncLegacyFrameState();
}

bool IXVulkanDevice::RequestResize(std::uint32_t width, std::uint32_t height)
{
    if (m_shutDown)
        return false;
    const VkExtent2D current = m_loop->GetSwapchainExtent();
    const bool sizeDiffers = current.width != width || current.height != height;
    const bool pendingDiffers = !m_pendingResize || m_pendingWidth != width || m_pendingHeight != height;
    if (!sizeDiffers && !pendingDiffers)
        return false;
    // Queued: the actual recreation happens synchronously inside the next
    // BeginFrame (single authoritative flow, §73), which then reports
    // SwapchainRecreated so the app runs resize orchestration first.
    m_pendingResize = true;
    m_pendingWidth = width;
    m_pendingHeight = height;
    return true;
}

std::uint32_t IXVulkanDevice::GetFramesInFlight() const
{
    return IXVulkanFrameTracker::kSlots;
}

std::uint64_t IXVulkanDevice::GetSwapchainGeneration() const
{
    return m_swapchain ? m_swapchain->Generation() : 0;
}

ixrhi::IXRHISwapchain& IXVulkanDevice::GetMainSwapchain()
{
    return *m_swapchain;
}

ixrhi::IXRHIRenderTarget* IXVulkanDevice::GetMainRenderTarget()
{
    if (!m_frameActive || !m_swapchain)
        return nullptr;
    return m_swapchain->MainTarget(m_activeImage);
}

const ixrhi::IXRHIRenderPass* IXVulkanDevice::GetMainPass() const
{
    if (!m_swapchain)
        return nullptr;
    return m_swapchain->MainPass();
}

void IXVulkanDevice::Shutdown()
{
    if (m_shutDown)
        return;
    m_shutDown = true;
    if (NativeDevice() == VK_NULL_HANDLE)
    {
        // Legacy device already gone (explicit Shutdown was skipped): release
        // host objects bluntly without Vulkan calls. No crash, documented leak.
        m_tracker.OnAbort();
        m_frameActive = false;
        m_slots = {};
        m_renderFinished.clear();
        m_imagesInFlight.clear();
        m_swapchain.reset();
        m_queryPool = VK_NULL_HANDLE;
        m_uploadPool = VK_NULL_HANDLE;
        return;
    }
    // Documented order (§70): idle first, then frame objects, swapchain
    // object (pass/targets/backbuffers), query pool, upload pool. Legacy
    // shims cleared so late native calls observe inactive state.
    vkDeviceWaitIdle(NativeDevice());
    m_tracker.OnAbort();
    m_frameActive = false;
    SyncLegacyFrameState();
    m_loop->SetMigrationMainPass(VK_NULL_HANDLE);
    TeardownFrameObjects();
    m_swapchain.reset();
    if (m_uploadPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(NativeDevice(), m_uploadPool, nullptr);
        m_uploadPool = VK_NULL_HANDLE;
    }
}

void IXVulkanDevice::CreateTimestampPool()
{
#if defined(IXTREEME_DEBUG_LOGS)
    if (m_queryPool != VK_NULL_HANDLE || !m_capabilities.supportsTimestampQueries)
        return;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_loop->GetPhysicalDevice(), &props);
    m_queryPeriodNs = props.limits.timestampPeriod;
    VkQueryPoolCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    create.queryType = VK_QUERY_TYPE_TIMESTAMP;
    create.queryCount = ixrhi::IXRHI_MAX_TIMESTAMP_POINTS;
    CheckVk(vkCreateQueryPool(NativeDevice(), &create, nullptr, &m_queryPool),
        "vkCreateQueryPool(timestamps)",
        __FILE__,
        __LINE__);
#endif
}

void IXVulkanDevice::BeginCaptureForFrame()
{
#if defined(IXTREEME_DEBUG_LOGS)
    if (m_queryPool == VK_NULL_HANDLE || !m_frameActive)
        return;
    IXVulkanFrameSlot& context = m_slots[m_activeSlot];
    vkCmdResetQueryPool(context.cmd, m_queryPool, 0, ixrhi::IXRHI_MAX_TIMESTAMP_POINTS);
    m_captureActive = true;
    m_captureResultsReady = false;
    m_captureWritten.fill(false);
    WriteTimestamp(static_cast<std::uint32_t>(ixrhi::IXRHITimestampPoint::FrameBegin));
#endif
}

void IXVulkanDevice::WriteTimestamp(ixrhi::IXRHITimestampPoint point)
{
    WriteTimestamp(static_cast<std::uint32_t>(point));
}

void IXVulkanDevice::WriteTimestamp(std::uint32_t pointIndex)
{
#if defined(IXTREEME_DEBUG_LOGS)
    if (!m_captureActive || m_queryPool == VK_NULL_HANDLE || !m_frameActive)
        return;
    if (pointIndex >= ixrhi::IXRHI_MAX_TIMESTAMP_POINTS)
        return;
    vkCmdWriteTimestamp(m_slots[m_activeSlot].cmd,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        m_queryPool,
        pointIndex);
    m_captureWritten[pointIndex] = true;
#else
    (void)pointIndex;
#endif
}

void IXVulkanDevice::RequestGpuFrameCapture()
{
#if defined(IXTREEME_DEBUG_LOGS)
    m_captureRequested = true;
#endif
}

void IXVulkanDevice::FinishCaptureAfterSubmit()
{
#if defined(IXTREEME_DEBUG_LOGS)
    if (!m_captureActive || m_queryPool == VK_NULL_HANDLE)
        return;

    CheckVk(vkQueueWaitIdle(m_loop->GetGraphicsQueue()), "vkQueueWaitIdle(capture)", __FILE__, __LINE__);

    struct TimestampWithAvailability
    {
        std::uint64_t value = 0;
        std::uint64_t available = 0;
    };
    std::array<TimestampWithAvailability, ixrhi::IXRHI_MAX_TIMESTAMP_POINTS> raw{};
    const VkResult result = vkGetQueryPoolResults(NativeDevice(),
        m_queryPool,
        0,
        ixrhi::IXRHI_MAX_TIMESTAMP_POINTS,
        sizeof(raw),
        raw.data(),
        sizeof(TimestampWithAvailability),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY)
    {
        m_captureActive = false;
        return;
    }

    ixrhi::IXRHITimestampResults results{};
    results.valid = true;
    results.frameNumber = m_tracker.GetFrameNumber();
    const std::uint64_t firstValue = raw[0].value;
    for (std::uint32_t i = 0; i < ixrhi::IXRHI_MAX_TIMESTAMP_POINTS; ++i)
    {
        results.pointValid[i] = m_captureWritten[i] && raw[i].available != 0;
        if (results.pointValid[i])
            results.pointNanoseconds[i] = static_cast<std::uint64_t>(
                static_cast<double>(raw[i].value - firstValue) * m_queryPeriodNs);
    }
    m_lastResults = results;
    m_captureResultsReady = true;
    m_captureActive = false;
#endif
}

bool IXVulkanDevice::TryReadTimestamps(ixrhi::IXRHITimestampResults& gpu,
                                       ixrhi::IXRHICpuFrameTiming& cpu)
{
#if defined(IXTREEME_DEBUG_LOGS)
    if (!m_captureResultsReady)
        return false;
    gpu = m_lastResults;
    cpu = m_lastCpuTiming;
    m_captureResultsReady = false;
    return true;
#else
    (void)gpu;
    (void)cpu;
    return false;
#endif
}

} // namespace ixvulkan
