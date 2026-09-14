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
// - Known Phase 1 violation under review: libs/platform/VulkanDevice.* lives in the
//   Platform lib but is really Graphics/Vulkan. It stays put in Phase 1 (moving it
//   would touch every renderer include); Phase 2 relocates it behind IXVulkanDevice.

namespace ixengine::core
{
} // namespace ixengine::core
