#pragma once

// IXVulkanFrameTracker — pure frame state machine + generation counter
// (Phase 3C, §51/53). Zero Vulkan: unit-tested in IXRHISmoke. The device owns
// one; it enforces Begin-once/End-once pairing (Debug asserts at the device
// boundary) and advances the frames-in-flight slot only on submitted frames.

#include <cstdint>

namespace ixvulkan
{

class IXVulkanFrameTracker
{
public:
    static constexpr std::uint32_t kSlots = 2; // == MAX_FRAMES_IN_FLIGHT

    enum class State : std::uint8_t
    {
        Idle = 0,
        Recording,
    };

    bool CanBegin() const { return m_state == State::Idle; }
    bool IsRecording() const { return m_state == State::Recording; }

    // Called when BeginFrame produces a frame. Returns the pairing token.
    std::uint64_t OnBegin(std::uint32_t slot, std::uint64_t frameNumber)
    {
        m_state = State::Recording;
        m_activeSlot = slot;
        m_activeFrameNumber = frameNumber;
        m_activeToken = ++m_tokenCounter;
        return m_activeToken;
    }

    // Called on successful EndFrame. Advances slot + frame number.
    void OnEnd()
    {
        m_state = State::Idle;
        m_activeSlot = (m_activeSlot + 1u) % kSlots;
        ++m_activeFrameNumber;
        m_activeToken = 0;
    }

    // Called on Skip/SwapchainRecreated/DeviceLost: no advance, no submit.
    void OnAbort() { m_state = State::Idle; m_activeToken = 0; }

    void OnSwapchainRecreated() { ++m_generation; }

    State GetState() const { return m_state; }
    std::uint32_t GetSlot() const { return m_activeSlot; }
    std::uint64_t GetFrameNumber() const { return m_activeFrameNumber; }
    std::uint64_t GetActiveToken() const { return m_activeToken; }
    std::uint64_t GetGeneration() const { return m_generation; }

private:
    State m_state = State::Idle;
    std::uint32_t m_activeSlot = 0;
    std::uint64_t m_activeFrameNumber = 0;
    std::uint64_t m_activeToken = 0;
    std::uint64_t m_tokenCounter = 0;
    std::uint64_t m_generation = 1; // starts at 1; 0 = invalid/unset
};

} // namespace ixvulkan
