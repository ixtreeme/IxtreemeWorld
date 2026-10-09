#pragma once

// Engine/Core boundary (Phase 1: documentation + dependency rule).
//
// Generic low-level functionality that belongs under Engine/Core:
//   Application lifecycle helpers, Logging (libs/debug), Math (libs/math),
//   Filesystem/paths, Time, Jobs (future), Platform (libs/platform OS/window/process).
//
// Rules:
// - Core must NOT depend on: Graphics renderer, Editor, game project, Auriga/world
//   gameplay. Higher-level modules may depend on Core.
// - Current mapping (no moves in Phase 1 for build safety):
//     Core/Logging    -> libs/debug (Debug.h/LogConfig.h)
//     Core/Math       -> libs/math (IXMath.h/Types.h/Vector.h/Matrix.h/...)
//     Core/Platform   -> libs/platform (NativeWindow/VulkanDevice/Process/...)
//     Core/Common     -> libs/common
// - Known placement debt (Phase 1 → 3H, unchanged): libs/platform/VulkanDevice.*
//   lives in the Platform lib but is really backend bootstrap for Graphics/Vulkan.
//   It stays put (moving it would touch every consumer include for zero behavior
//   gain); the IXVulkan backend owns all frame/device authority behind
//   IXVulkanDevice, and no generic renderer calls VulkanDevice frame APIs
//   anymore (deleted in Phase 3F).

namespace ixengine::core
{
} // namespace ixengine::core
