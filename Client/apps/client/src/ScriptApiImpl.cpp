#include "ScriptApiImpl.h"

#include "AudioEngine.h"
#include "Debug.h"
#include "ViewportControls.h"  // MovementInputState

#include <cmath>

namespace
{
constexpr float kRadToDeg = 57.29577951f;
constexpr float kDegToRad = 0.01745329252f;
}

MeshSceneEntity* ScriptApiImpl::Find(std::uint32_t id)
{
    if (!meshes || id == 0)
        return nullptr;
    for (MeshSceneEntity& m : *meshes)
        if (m.id == id)
            return &m;
    return nullptr;
}

std::uint32_t ScriptApiImpl::FindEntityByName(const std::string& name)
{
    if (meshes)
        for (const MeshSceneEntity& m : *meshes)
            if (m.name == name)
                return m.id;
    return 0;
}

bool ScriptApiImpl::EntityExists(std::uint32_t id) { return Find(id) != nullptr; }

std::string ScriptApiImpl::GetEntityName(std::uint32_t id)
{
    const MeshSceneEntity* m = Find(id);
    return m ? m->name : std::string();
}

void ScriptApiImpl::GetPosition(std::uint32_t id, float out[3])
{
    const MeshSceneEntity* m = Find(id);
    out[0] = m ? m->position[0] : 0.0f;
    out[1] = m ? m->position[1] : 0.0f;
    out[2] = m ? m->position[2] : 0.0f;
}

void ScriptApiImpl::SetPosition(std::uint32_t id, const float p[3])
{
    MeshSceneEntity* m = Find(id);
    if (!m)
        return;
    m->position[0] = p[0];
    m->position[1] = p[1];
    m->position[2] = p[2];
    if (syncMesh)
        syncMesh(*m);
    if (markDirty)
        markDirty();
}

void ScriptApiImpl::GetRotation(std::uint32_t id, float out[3])
{
    const MeshSceneEntity* m = Find(id);
    out[0] = m ? m->rotation[0] * kRadToDeg : 0.0f;
    out[1] = m ? m->rotation[1] * kRadToDeg : 0.0f;
    out[2] = m ? m->rotation[2] * kRadToDeg : 0.0f;
}

void ScriptApiImpl::SetRotation(std::uint32_t id, const float r[3])
{
    MeshSceneEntity* m = Find(id);
    if (!m)
        return;
    m->rotation[0] = r[0] * kDegToRad;
    m->rotation[1] = r[1] * kDegToRad;
    m->rotation[2] = r[2] * kDegToRad;
    if (syncMesh)
        syncMesh(*m);
    if (markDirty)
        markDirty();
}

void ScriptApiImpl::GetScale(std::uint32_t id, float out[3])
{
    const MeshSceneEntity* m = Find(id);
    out[0] = m ? m->scale[0] : 1.0f;
    out[1] = m ? m->scale[1] : 1.0f;
    out[2] = m ? m->scale[2] : 1.0f;
}

void ScriptApiImpl::SetScale(std::uint32_t id, const float s[3])
{
    MeshSceneEntity* m = Find(id);
    if (!m)
        return;
    m->scale[0] = s[0];
    m->scale[1] = s[1];
    m->scale[2] = s[2];
    if (syncMesh)
        syncMesh(*m);
    if (markDirty)
        markDirty();
}

bool ScriptApiImpl::IsKeyDown(ixscript::ScriptKey key)
{
    if (!movement)
        return false;
    using K = ixscript::ScriptKey;
    switch (key)
    {
    case K::W: return movement->w;
    case K::A: return movement->a;
    case K::S: return movement->s;
    case K::D: return movement->d;
    case K::Space: return movement->space;
    case K::Shift: return movement->shift;
    case K::Ctrl: return movement->control;
    case K::Up: return input && input->up;
    case K::Down: return input && input->down;
    case K::Left: return input && input->left;
    case K::Right: return input && input->right;
    case K::MouseLeft: return input && input->mouseLeft;
    case K::MouseRight: return input && input->mouseRight;
    default: return false;
    }
}

void ScriptApiImpl::GetMouseDelta(float& dx, float& dy)
{
    dx = mouseDx;
    dy = mouseDy;
}

double ScriptApiImpl::GetDeltaTime() { return deltaSeconds; }
double ScriptApiImpl::GetElapsedTime() { return elapsedSeconds; }

void ScriptApiImpl::PlayOneShot(const std::string& clipAssetId)
{
    if (!audio || !resolveAudioClip)
        return;
    const std::string path = resolveAudioClip(clipAssetId);
    if (!path.empty())
        audio->PlayOneShot(path);
}

std::string ScriptApiImpl::LoadScriptSource(const std::string& assetId)
{
    if (!resolveScriptSource)
        return {};
    return resolveScriptSource(assetId);
}

void ScriptApiImpl::Log(const std::string& msg) { Tracenf("[SCRIPT] %s", msg.c_str()); }
void ScriptApiImpl::LogError(const std::string& msg) { TraceError("[SCRIPT] %s", msg.c_str()); }

std::uint32_t ScriptApiImpl::SpawnMesh(const std::string& meshAssetId, float x, float y, float z)
{
    if (!nextEntityId || meshAssetId.empty())
        return 0;
    const std::uint32_t id = (*nextEntityId)++;  // pre-allocate; the drain reuses this exact id
    deferredOps.push_back({DeferredKind::SpawnMesh, meshAssetId, {x, y, z}, id});
    return id;
}

std::uint32_t ScriptApiImpl::SpawnPrefab(const std::string& prefabAssetId, float x, float y, float z)
{
    if (!nextEntityId || prefabAssetId.empty())
        return 0;
    const std::uint32_t id = (*nextEntityId)++;
    deferredOps.push_back({DeferredKind::SpawnPrefab, prefabAssetId, {x, y, z}, id});
    return id;
}

void ScriptApiImpl::DestroyEntity(std::uint32_t id)
{
    if (id == 0)
        return;
    deferredOps.push_back({DeferredKind::Destroy, std::string(), {0.0f, 0.0f, 0.0f}, id});
}

ixscript::RaycastHit ScriptApiImpl::Raycast(float ox, float oy, float oz,
                                            float dx, float dy, float dz, float maxDist)
{
    ixscript::RaycastHit out;
    if (!raycast)
        return out;
    float dir[3] = {dx, dy, dz};
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1e-6f)
        return out;  // degenerate direction
    dir[0] /= len; dir[1] /= len; dir[2] /= len;
    const float origin[3] = {ox, oy, oz};
    out.hit = raycast(origin, dir, maxDist, out.entityId, out.point, out.normal, out.distance);
    return out;
}

void ScriptApiImpl::SetAnimatorFloat(std::uint32_t id, const std::string& name, float value)
{
    if (setAnimatorParam)
        setAnimatorParam(id, name, AnimatorParamType::Float, value, false);
}
void ScriptApiImpl::SetAnimatorBool(std::uint32_t id, const std::string& name, bool value)
{
    if (setAnimatorParam)
        setAnimatorParam(id, name, AnimatorParamType::Bool, 0.0f, value);
}
void ScriptApiImpl::SetAnimatorTrigger(std::uint32_t id, const std::string& name)
{
    if (setAnimatorParam)
        setAnimatorParam(id, name, AnimatorParamType::Trigger, 0.0f, false);
}
