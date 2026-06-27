// HelloGame — a minimal native C++ game module for the IxtreemeWorld engine.
//
// This file is compiled into HelloGame.dll using ONLY the SDK headers (no engine source, no engine
// library). The prebuilt engine loads the DLL from <ProjectRoot>/Binaries at project open, and these
// IXSCRIPT_REGISTER'd classes show up in the Script component's Native class picker and run in Play.
//
// Build: see CMakeLists.txt in this folder. The one requirement is the SAME MSVC toolset + /MT + C++20
// the engine binary was built with (the plugin boundary is C++ ABI; mismatched toolchains corrupt).

#define IXTREEME_GAME_MODULE 1           // switch IXSCRIPT_REGISTER to the module (DLL-local) path
#include "ixtreeme/NativeScript.h"
#include "ixtreeme/IxModuleRegistry.inl" // provides the registry plumbing + the exported entry point

#include <cmath>
#include <cstdint>
#include <string>

// Bobs the entity up and down on Y. Params: amplitude (units, default 1), speed (Hz, default 1).
class HelloBobber : public ixscript::NativeScript
{
public:
    void OnStart() override
    {
        m_amplitude = ParamFloat("amplitude", 1.0f);
        m_speed = ParamFloat("speed", 1.0f);
        float p[3];
        GetPosition(p);
        m_baseY = p[1];
        Log("HelloBobber: started");
    }

    void OnUpdate(float dt) override
    {
        m_phase += dt * m_speed;
        float p[3];
        GetPosition(p);
        p[1] = m_baseY + std::sin(m_phase * 6.2831853f) * m_amplitude;
        SetPosition(p);
    }

    void OnDestroy() override { Log("HelloBobber: stopped"); }

private:
    float m_amplitude = 1.0f;
    float m_speed = 1.0f;
    float m_baseY = 0.0f;
    float m_phase = 0.0f;
};
IXSCRIPT_REGISTER(HelloBobber)

// Continuously yaws the entity around Y at `speed` deg/sec. Demonstrates IX_REFLECT: `speed` shows up
// in the engine inspector as a typed, serialized field (default 120 from the in-class initializer) — no
// manual Parameters key, no ParamFloat. The saved value is written into `speed` before OnStart/OnUpdate,
// so you just use the member. Auto-spins on Play (no input needed).
class HelloSpinner : public ixscript::NativeScript
{
public:
    float speed = 120.0f;  // deg/sec — editable in the inspector

    void OnUpdate(float dt) override
    {
        float r[3];
        GetRotation(r);  // Euler degrees
        r[1] += speed * dt;
        SetRotation(r);
    }

    IX_REFLECT(HelloSpinner, speed)  // expose `speed` to the inspector + serialization
};
IXSCRIPT_REGISTER(HelloSpinner)

// Showcases every IX_REFLECT field type — float, bool, and a vec3 (float[3]). Oscillates the entity
// along `axis` by amplitude*sin(frequency). Demonstrates a multi-field reflected script.
class HelloOscillator : public ixscript::NativeScript
{
public:
    float amplitude = 1.0f;
    float frequency = 1.0f;          // Hz
    bool  enabled = true;
    float axis[3] = {0.0f, 1.0f, 0.0f};  // float[3] -> Vec3 widget

    void OnStart() override { GetPosition(m_base); }

    void OnUpdate(float dt) override
    {
        if (!enabled)
            return;
        m_phase += dt * frequency;
        const float s = std::sin(m_phase * 6.2831853f) * amplitude;
        float p[3] = {m_base[0] + axis[0] * s, m_base[1] + axis[1] * s, m_base[2] + axis[2] * s};
        SetPosition(p);
    }

    IX_REFLECT(HelloOscillator, amplitude, frequency, enabled, axis)

private:
    float m_base[3] = {0.0f, 0.0f, 0.0f};
    float m_phase = 0.0f;
};
IXSCRIPT_REGISTER(HelloOscillator)

// Demonstrates the rich API (v3): arrow-key movement, a downward ground-check raycast on left-click,
// and collision logging. Param: speed (units/sec).
class HelloController : public ixscript::NativeScript
{
public:
    float speed = 5.0f;

    void OnUpdate(float dt) override
    {
        float p[3];
        GetPosition(p);
        const float v = speed * dt;
        if (IsKeyDown(ixscript::ScriptKey::Up))    p[2] += v;
        if (IsKeyDown(ixscript::ScriptKey::Down))  p[2] -= v;
        if (IsKeyDown(ixscript::ScriptKey::Left))  p[0] -= v;
        if (IsKeyDown(ixscript::ScriptKey::Right)) p[0] += v;
        SetPosition(p);

        if (IsKeyDown(ixscript::ScriptKey::MouseLeft))
        {
            const ixscript::RaycastHit hit = Raycast(p[0], p[1], p[2], 0.0f, -1.0f, 0.0f, 50.0f);
            if (hit.hit)
                Log("ground under me: entity=" + std::to_string(hit.entityId) +
                    " dist=" + std::to_string(hit.distance));
        }
    }

    void OnCollision(std::uint32_t other) override
    {
        Log("HelloController collided with entity " + std::to_string(other));
    }

    IX_REFLECT(HelloController, speed)
};
IXSCRIPT_REGISTER(HelloController)
