#pragma once

// The color a scene view is cleared to. The sky (SkyRenderer) normally covers it; it shows only where
// no sky is drawn (the sky renderer unavailable, or outside the world). The water reflection clears
// to the same color, so a reflection without a sky still matches the visible backdrop (a black
// reflection made water dark wherever it mirrors the sky).
inline constexpr float kSceneClearColor[4] = {0.04f, 0.05f, 0.09f, 1.0f};
