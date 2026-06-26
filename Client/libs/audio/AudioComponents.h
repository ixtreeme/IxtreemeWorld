#pragma once

// Audio component data (plain old data — NO miniaudio dependency). These structs live on a
// MeshSceneEntity (mirroring the physics CharacterController component) and are serialized into the
// scene/prefab. The runtime side (AudioEngine, ma_sound) lives in AudioEngine.h/.cpp and is driven
// in Play. Keeping this header miniaudio-free lets MapEditorTypes/SceneManager/PrefabDocument include
// it without dragging the ~90k-line miniaudio implementation around.

#include <algorithm>
#include <cstdint>
#include <string>

namespace ixaudio
{

// Volume buses: Master is the engine endpoint; Music/SFX are groups parented to it.
enum class AudioBus : std::uint8_t
{
    Master = 0,
    Music = 1,
    SFX = 2
};

inline const char* BusName(AudioBus bus)
{
    switch (bus)
    {
    case AudioBus::Master: return "Master";
    case AudioBus::Music: return "Music";
    case AudioBus::SFX: return "SFX";
    }
    return "SFX";
}

inline AudioBus ParseBus(const std::string& s)
{
    if (s == "Master") return AudioBus::Master;
    if (s == "Music") return AudioBus::Music;
    return AudioBus::SFX;
}

struct AudioSourceComponent
{
    std::string clipAssetId;     // AssetLibrary id of an Audio clip ("" = none)
    bool enabled = true;
    bool loop = false;
    bool is3d = true;            // 3D spatialized (false = 2D, plays at constant volume)
    bool playOnStart = true;     // start when Play begins
    float volume = 1.0f;         // 0..1
    float pitch = 1.0f;          // 0.5..2
    float minDistance = 1.0f;    // 3D rolloff: full volume within this radius
    float maxDistance = 100.0f;  // 3D rolloff: silent beyond this radius
    AudioBus bus = AudioBus::SFX;
};

inline void Sanitize(AudioSourceComponent& a)
{
    a.volume = std::clamp(a.volume, 0.0f, 1.0f);
    a.pitch = std::clamp(a.pitch, 0.5f, 2.0f);
    a.minDistance = std::max(0.01f, a.minDistance);
    a.maxDistance = std::max(a.minDistance + 0.01f, a.maxDistance);
}

struct AudioListenerComponent
{
    bool enabled = true;
};

} // namespace ixaudio
