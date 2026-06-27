// Sample native scripts — the C++ "MonoBehaviour" pattern. Derive from ixscript::NativeScript,
// override the hooks, reach the engine via the convenience methods, and IXSCRIPT_REGISTER the class.
// These show up in the Script component's class picker. Add your own .cpp files here the same way.

#include "NativeScript.h"

namespace
{

// Spins the entity around its Y axis. Param "speed" = degrees/second (default 90).
class SpinScript : public ixscript::NativeScript
{
    float m_speed = 90.0f;

    void OnStart() override
    {
        m_speed = ParamFloat("speed", 90.0f);
        Log("SpinScript started");
    }

    void OnUpdate(float dt) override
    {
        float r[3];
        GetRotation(r);          // Euler degrees
        r[1] += m_speed * dt;    // yaw
        SetRotation(r);
    }
};

// Moves the entity in the XZ plane with WASD. Param "speed" = units/second (default 5).
class PlayerMover : public ixscript::NativeScript
{
    float m_speed = 5.0f;

    void OnStart() override { m_speed = ParamFloat("speed", 5.0f); }

    void OnUpdate(float dt) override
    {
        float p[3];
        GetPosition(p);
        const float step = m_speed * dt;
        if (IsKeyDown(ixscript::ScriptKey::W)) p[2] += step;
        if (IsKeyDown(ixscript::ScriptKey::S)) p[2] -= step;
        if (IsKeyDown(ixscript::ScriptKey::D)) p[0] += step;
        if (IsKeyDown(ixscript::ScriptKey::A)) p[0] -= step;
        SetPosition(p);
    }
};

} // namespace

IXSCRIPT_REGISTER(SpinScript)
IXSCRIPT_REGISTER(PlayerMover)
