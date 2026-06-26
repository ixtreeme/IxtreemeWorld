#pragma once

// The audio runtime: a thin wrapper over miniaudio's high-level engine (ma_engine). PIMPL hides all
// ma_* types so consumers (apps/client, the editor) never transitively include miniaudio. Device-init
// failure is a SILENT no-op (headless CI / no audio device) — every method early-returns when not
// initialized, so the engine never crashes for lack of sound hardware.
//
// Cross-platform: miniaudio auto-selects the OS backend (WASAPI / AAudio / CoreAudio / ALSA); this
// module contains zero platform-specific code.

#include "AudioComponents.h"

#include <memory>
#include <string>

struct ma_sound;  // miniaudio (global scope) — kept opaque here so the impl never leaks into consumers

namespace ixaudio
{

// A playing instance of one entity's AudioSource. Owns its ma_sound (heap-allocated; the dtor
// stops + uninits it). Move-only — stored by value in a per-entity map.
struct AudioSourceRuntime
{
    ::ma_sound* sound = nullptr;
    bool ok = false;
    AudioSourceRuntime() = default;
    AudioSourceRuntime(AudioSourceRuntime&& other) noexcept;
    AudioSourceRuntime& operator=(AudioSourceRuntime&& other) noexcept;
    AudioSourceRuntime(const AudioSourceRuntime&) = delete;
    AudioSourceRuntime& operator=(const AudioSourceRuntime&) = delete;
    ~AudioSourceRuntime();
};

class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Brings up the audio device + the Music/SFX bus groups. Returns false (and stays silent-safe) if
    // no device is available. Idempotent.
    bool Initialize();
    void Shutdown();
    bool IsInitialized() const;

    void SetMasterVolume(float volume01);          // engine endpoint
    void SetBusVolume(AudioBus bus, float volume01);

    // Fire-and-forget one-shot for editor preview / UI sounds — miniaudio frees it when it finishes.
    void PlayOneShot(const std::string& absFilePath, AudioBus bus = AudioBus::SFX);

    // Per-entity AudioSource lifecycle (driven by the Play loop).
    bool CreateSource(AudioSourceRuntime& rt, const AudioSourceComponent& comp, const std::string& absFilePath);
    void StartSource(AudioSourceRuntime& rt);
    // Pushes live state (volume/pitch/loop) every frame; pos is the world position for 3D sources.
    void UpdateSource(AudioSourceRuntime& rt, const AudioSourceComponent& comp, const float pos[3]);
    // Listener (the "ears") — Chunk 4 drives this from the camera / a listener entity each frame.
    void SetListener(const float pos[3], const float forward[3], const float up[3]);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ixaudio
