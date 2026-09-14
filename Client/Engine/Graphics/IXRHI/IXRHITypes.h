#pragma once

// IXRHI core value types. Models ENGINE needs (not a Vulkan-enum clone): only the
// states today's renderers actually use, plus format/depth variants the swapchain
// and offscreen targets require. Anything exotic stays behind IXRHICapabilities
// queries in Phase 3+.

#include <cstdint>

namespace ixrhi
{

// Minimal format set covering: vertex attributes (pos3/color4/normal/uv),
// font-atlas + sampled color (RGBA8), swapchain (BGRA8 + sRGB), offscreen depth.
enum class IXRHIFormat : std::uint32_t
{
    Undefined = 0,
    R8G8B8A8Unorm,
    B8G8R8A8Unorm,
    B8G8R8A8Srgb,
    R32G32Float,
    R32G32B32Float,
    R32G32B32A32Float,
    D32Float,
    D24UnormS8Uint,
};

inline std::uint32_t IXRHIFormatByteSize(IXRHIFormat format)
{
    switch (format)
    {
    case IXRHIFormat::R8G8B8A8Unorm:
    case IXRHIFormat::B8G8R8A8Unorm:
    case IXRHIFormat::B8G8R8A8Srgb:
    case IXRHIFormat::D24UnormS8Uint: return 4;
    case IXRHIFormat::R32G32Float: return 8;
    case IXRHIFormat::R32G32B32Float: return 12;
    case IXRHIFormat::R32G32B32A32Float:
    case IXRHIFormat::D32Float: return 16;
    case IXRHIFormat::Undefined: break;
    }
    return 0;
}

enum class IXRHIBufferUsage : std::uint32_t
{
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    TransferSrc = 1u << 4,
    TransferDst = 1u << 5,
    Indirect = 1u << 6,
};

inline IXRHIBufferUsage operator|(IXRHIBufferUsage a, IXRHIBufferUsage b)
{
    return static_cast<IXRHIBufferUsage>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline IXRHIBufferUsage operator&(IXRHIBufferUsage a, IXRHIBufferUsage b)
{
    return static_cast<IXRHIBufferUsage>(
        static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

inline bool HasUsage(IXRHIBufferUsage mask, IXRHIBufferUsage bit)
{
    return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(bit)) != 0;
}// CPU visibility. Write = host-visible + coherent (today's staging/uniform path).
// Device-only resources use None. VMA can back either without API change.
enum class IXRHICpuAccess : std::uint8_t
{
    None = 0,
    Write,
};

enum class IXRHITextureUsage : std::uint32_t
{
    None = 0,
    Sampled = 1u << 0,
    ColorAttachment = 1u << 1,
    DepthStencilAttachment = 1u << 2,
    TransferDst = 1u << 3,
    TransferSrc = 1u << 4,
};

inline IXRHITextureUsage operator|(IXRHITextureUsage a, IXRHITextureUsage b)
{
    return static_cast<IXRHITextureUsage>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline IXRHITextureUsage operator&(IXRHITextureUsage a, IXRHITextureUsage b)
{
    return static_cast<IXRHITextureUsage>(
        static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

inline bool HasUsage(IXRHITextureUsage mask, IXRHITextureUsage bit)
{
    return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(bit)) != 0;
}

// Shader-stage bitmask for bindings and shaders.
enum class IXRHIShaderStage : std::uint32_t
{
    None = 0,
    Vertex = 1u << 0,
    Fragment = 1u << 1,
    Compute = 1u << 2,
};

inline IXRHIShaderStage operator|(IXRHIShaderStage a, IXRHIShaderStage b)
{
    return static_cast<IXRHIShaderStage>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

enum class IXRHIPrimitiveTopology : std::uint8_t
{
    PointList = 0,
    LineList,
    TriangleList,
};

enum class IXRHICullMode : std::uint8_t
{
    None = 0,
    Front,
    Back,
};

enum class IXRHIFrontFace : std::uint8_t
{
    CounterClockwise = 0,
    Clockwise,
};

enum class IXRHIPolygonMode : std::uint8_t
{
    Fill = 0,
};

enum class IXRHICompareOp : std::uint8_t
{
    Never = 0,
    Less,
    LessOrEqual,
    Always,
};

enum class IXRHIBlendFactor : std::uint8_t
{
    Zero = 0,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
};

enum class IXRHIBlendOp : std::uint8_t
{
    Add = 0,
};

enum class IXRHILoadOp : std::uint8_t
{
    Load = 0,
    Clear,
    DontCare,
};

enum class IXRHIStoreOp : std::uint8_t
{
    Store = 0,
    DontCare,
};

// Engine-level resource state (backend translates to VkImageLayout / barriers).
enum class IXRHIImageLayout : std::uint8_t
{
    Undefined = 0,
    TransferDst,
    ShaderReadOnly,
    ColorAttachment,
    DepthStencilAttachment,
    Present,
};

enum class IXRHIBindingType : std::uint8_t
{
    UniformBuffer = 0,
    StorageBuffer,
    SampledTexture,
};

enum class IXRHISamplerFilter : std::uint8_t
{
    Nearest = 0,
    Linear,
};

enum class IXRHISamplerAddress : std::uint8_t
{
    ClampToEdge = 0,
    Repeat,
};

} // namespace ixrhi
