// Spinner: rotates the entity around its Y axis.
// Parameter (Script component -> Parameters):  speed  = degrees/second (default 90).
// Drag this .as into the editor's asset browser, then on an entity add a Script component,
// set Backend = AngelScript, pick this script, and press Play.
//
// Every AngelScript asset defines ONE `class Script`; the engine creates one object per entity.
// Same-named members are auto-filled before OnStart (uint id/entityId = the entity; the Script
// component's parameters by name). The engine API is bound as global functions (GetRotation,
// SetRotation, IsKeyDown, Raycast, SpawnMesh, UiOpen, NetConnect, ...), the same surface as Lua's.

class Script
{
    uint entityId;        // filled by the engine
    float speed = 90.0f;  // "speed" parameter (auto-filled before OnStart)

    void OnStart()
    {
        Log("spinner start");
    }

    void OnUpdate(float dt)
    {
        float3 r = GetRotation(entityId);  // Euler degrees
        SetRotation(entityId, r.x, r.y + speed * dt, r.z);
    }

    void OnDestroy()
    {
        Log("spinner stop");
    }
}
