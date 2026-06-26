#include "AudioEngine.h"

#include "Debug.h"

#include <algorithm>
#include <array>

#include <miniaudio.h>

namespace ixaudio
{

// Master is the engine endpoint (ma_engine volume); Music + SFX are groups parented to it. Sources
// (and one-shots) route into their bus's group, so per-bus volume scales independently under master.
struct AudioEngine::Impl
{
    ma_engine engine{};
    std::array<ma_sound_group, 2> groups{};  // [0]=Music, [1]=SFX
    bool engineOk = false;
    bool groupsOk = false;
};

namespace
{
ma_sound_group* GroupFor(std::array<ma_sound_group, 2>& groups, bool groupsOk, AudioBus bus)
{
    if (!groupsOk)
        return nullptr;
    switch (bus)
    {
    case AudioBus::Music: return &groups[0];
    case AudioBus::SFX: return &groups[1];
    case AudioBus::Master: return nullptr;  // straight to the engine endpoint
    }
    return nullptr;
}
}  // namespace

AudioEngine::AudioEngine() : m_impl(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { Shutdown(); }

bool AudioEngine::Initialize()
{
    if (m_impl->engineOk)
        return true;

    ma_engine_config config = ma_engine_config_init();
    if (ma_engine_init(&config, &m_impl->engine) != MA_SUCCESS)
    {
        TraceError("[AUDIO] device init failed — audio disabled (engine runs silently)");
        m_impl->engineOk = false;
        return false;
    }
    m_impl->engineOk = true;

    bool ok = true;
    for (ma_sound_group& group : m_impl->groups)
        if (ma_sound_group_init(&m_impl->engine, 0, nullptr, &group) != MA_SUCCESS)
            ok = false;
    m_impl->groupsOk = ok;
    if (!ok)
        TraceError("[AUDIO] bus group init failed — sounds will play on the master bus");

    Tracenf("[AUDIO] initialized (miniaudio %s)", MA_VERSION_STRING);
    return true;
}

void AudioEngine::Shutdown()
{
    if (!m_impl->engineOk)
        return;
    if (m_impl->groupsOk)
        for (ma_sound_group& group : m_impl->groups)
            ma_sound_group_uninit(&group);
    m_impl->groupsOk = false;
    ma_engine_uninit(&m_impl->engine);
    m_impl->engineOk = false;
}

bool AudioEngine::IsInitialized() const { return m_impl->engineOk; }

void AudioEngine::SetMasterVolume(float volume01)
{
    if (!m_impl->engineOk)
        return;
    ma_engine_set_volume(&m_impl->engine, std::max(0.0f, volume01));
}

void AudioEngine::SetBusVolume(AudioBus bus, float volume01)
{
    if (!m_impl->engineOk)
        return;
    const float v = std::max(0.0f, volume01);
    if (bus == AudioBus::Master)
    {
        ma_engine_set_volume(&m_impl->engine, v);
        return;
    }
    if (ma_sound_group* group = GroupFor(m_impl->groups, m_impl->groupsOk, bus))
        ma_sound_group_set_volume(group, v);
}

void AudioEngine::PlayOneShot(const std::string& absFilePath, AudioBus bus)
{
    if (!m_impl->engineOk || absFilePath.empty())
        return;
    if (ma_engine_play_sound(&m_impl->engine, absFilePath.c_str(), GroupFor(m_impl->groups, m_impl->groupsOk, bus)) != MA_SUCCESS)
        TraceError("[AUDIO] play one-shot failed: %s", absFilePath.c_str());
}

// --- AudioSourceRuntime (defined here, where ma_sound is a complete type) ---
AudioSourceRuntime::AudioSourceRuntime(AudioSourceRuntime&& other) noexcept
    : sound(other.sound), ok(other.ok)
{
    other.sound = nullptr;
    other.ok = false;
}

AudioSourceRuntime& AudioSourceRuntime::operator=(AudioSourceRuntime&& other) noexcept
{
    if (this != &other)
    {
        if (sound)
        {
            ma_sound_uninit(sound);
            delete sound;
        }
        sound = other.sound;
        ok = other.ok;
        other.sound = nullptr;
        other.ok = false;
    }
    return *this;
}

AudioSourceRuntime::~AudioSourceRuntime()
{
    if (sound)
    {
        ma_sound_uninit(sound);  // stops playback + releases the data source
        delete sound;
        sound = nullptr;
    }
}

bool AudioEngine::CreateSource(AudioSourceRuntime& rt, const AudioSourceComponent& comp,
                               const std::string& absFilePath)
{
    rt = AudioSourceRuntime{};  // release any prior sound first
    if (!m_impl->engineOk || absFilePath.empty())
        return false;

    // Looping clips (music) stream; one-shots decode upfront for low latency.
    ma_uint32 flags = comp.loop ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
    if (!comp.is3d)
        flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

    ma_sound* sound = new ma_sound{};
    ma_sound_group* group = GroupFor(m_impl->groups, m_impl->groupsOk, comp.bus);
    if (ma_sound_init_from_file(&m_impl->engine, absFilePath.c_str(), flags, group, nullptr, sound) != MA_SUCCESS)
    {
        delete sound;
        TraceError("[AUDIO] source init failed: %s", absFilePath.c_str());
        return false;
    }
    rt.sound = sound;
    rt.ok = true;
    ma_sound_set_looping(sound, comp.loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume(sound, comp.volume);
    ma_sound_set_pitch(sound, comp.pitch);
    if (comp.is3d)
    {
        ma_sound_set_min_distance(sound, comp.minDistance);
        ma_sound_set_max_distance(sound, comp.maxDistance);
    }
    return true;
}

void AudioEngine::StartSource(AudioSourceRuntime& rt)
{
    if (rt.ok && rt.sound)
        ma_sound_start(rt.sound);
}

void AudioEngine::UpdateSource(AudioSourceRuntime& rt, const AudioSourceComponent& comp, const float pos[3])
{
    if (!rt.ok || !rt.sound)
        return;
    ma_sound_set_volume(rt.sound, comp.volume);
    ma_sound_set_pitch(rt.sound, comp.pitch);
    ma_sound_set_looping(rt.sound, comp.loop ? MA_TRUE : MA_FALSE);
    if (comp.is3d && pos)
    {
        ma_sound_set_position(rt.sound, pos[0], pos[1], pos[2]);
        ma_sound_set_min_distance(rt.sound, comp.minDistance);
        ma_sound_set_max_distance(rt.sound, comp.maxDistance);
    }
}

void AudioEngine::SetListener(const float pos[3], const float forward[3], const float up[3])
{
    if (!m_impl->engineOk)
        return;
    if (pos)
        ma_engine_listener_set_position(&m_impl->engine, 0, pos[0], pos[1], pos[2]);
    if (forward)
        ma_engine_listener_set_direction(&m_impl->engine, 0, forward[0], forward[1], forward[2]);
    if (up)
        ma_engine_listener_set_world_up(&m_impl->engine, 0, up[0], up[1], up[2]);
}

} // namespace ixaudio
