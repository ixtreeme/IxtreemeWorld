#pragma once

// The color a scene view is cleared to. There is no sky pass, so this is the backdrop above the
// horizon; the water reflection clears to the same color so that the reflected "sky" matches the
// visible one (a black reflection made water dark wherever it mirrors the sky).
inline constexpr float kSceneClearColor[4] = {0.04f, 0.05f, 0.09f, 1.0f};
