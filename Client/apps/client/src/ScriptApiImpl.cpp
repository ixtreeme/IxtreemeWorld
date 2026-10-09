#include "ScriptApiImpl.h"

#include "AudioEngine.h"
#include "Debug.h"
#include "RmlUiLayer.h"
#include "ViewportControls.h"  // MovementInputState

#include <algorithm>
#include <cmath>
#include <cstring>

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
    case K::Q: return input && input->q;
    case K::E: return input && input->e;
    case K::R: return input && input->r;
    case K::F: return input && input->f;
    case K::Num1: return input && input->num[0];
    case K::Num2: return input && input->num[1];
    case K::Num3: return input && input->num[2];
    case K::Num4: return input && input->num[3];
    case K::Num5: return input && input->num[4];
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
    if (!nextEntityId || !*nextEntityId || *nextEntityId == UINT32_MAX || meshAssetId.empty() ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return 0;
    const std::uint32_t id = (*nextEntityId)++;  // pre-allocate; the drain reuses this exact id
    deferredOps.push_back({DeferredKind::SpawnMesh, meshAssetId, {x, y, z}, id});
    return id;
}

std::uint32_t ScriptApiImpl::SpawnPrefab(const std::string& prefabAssetId, float x, float y, float z)
{
    if (!nextEntityId || !*nextEntityId || *nextEntityId == UINT32_MAX || prefabAssetId.empty() ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
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

void ScriptApiImpl::SetMaterial(std::uint32_t id, std::uint32_t slot, const std::string& materialAssetId)
{
    if (id == 0 || materialAssetId.empty() || slot >= 64)
        return;
    DeferredOp op{DeferredKind::SetMaterial, materialAssetId, {0.0f, 0.0f, 0.0f}, id};
    op.slot = slot;
    deferredOps.push_back(std::move(op));
}

bool ScriptApiImpl::GetCharacterState(std::uint32_t id, ixscript::CharacterState& out)
{
    out = {};
    return getCharacterState && getCharacterState(id, out);
}

void ScriptApiImpl::SetCharacterAbilities(std::uint32_t id, bool canRun, bool canJump)
{
    if (setCharacterAbilities)
        setCharacterAbilities(id, canRun, canJump);
}

std::uint32_t ScriptApiImpl::UiOpen(const std::string& documentPath)
{
    if (!gameUi)
        return 0;
    const std::string path = resolveUiDocument ? resolveUiDocument(documentPath) : documentPath;
    if (path.empty())
    {
        LogError("UiOpen: no such document in the asset folder: " + documentPath);
        return 0;
    }
    const std::uint32_t handle = gameUi->OpenGameDocument(path);
    if (handle == 0)
        LogError("UiOpen: the document could not be loaded: " + documentPath);
    return handle;
}

void ScriptApiImpl::UiClose(std::uint32_t document)
{
    if (gameUi)
        gameUi->CloseGameDocument(document);
}

void ScriptApiImpl::UiSetVisible(std::uint32_t document, bool visible)
{
    if (gameUi)
        gameUi->SetGameDocumentVisible(document, visible);
}

void ScriptApiImpl::UiSetText(std::uint32_t document, const std::string& elementId, const std::string& text)
{
    if (gameUi)
        gameUi->SetGameElementText(document, elementId, text);
}

void ScriptApiImpl::UiSetProperty(std::uint32_t document,
                                  const std::string& elementId,
                                  const std::string& property,
                                  const std::string& value)
{
    if (gameUi)
        gameUi->SetGameElementProperty(document, elementId, property, value);
}

void ScriptApiImpl::UiSetClass(std::uint32_t document,
                               const std::string& elementId,
                               const std::string& className,
                               bool enabled)
{
    if (gameUi)
        gameUi->SetGameElementClass(document, elementId, className, enabled);
}

bool ScriptApiImpl::UiConsumeClick(std::uint32_t document, const std::string& elementId)
{
    return gameUi != nullptr && gameUi->ConsumeGameElementClick(document, elementId);
}

void ScriptApiImpl::ParticlePlay(std::uint32_t id)
{
    if (particlePlay)
        particlePlay(id);
}

void ScriptApiImpl::ParticleStop(std::uint32_t id)
{
    if (particleStop)
        particleStop(id);
}

void ScriptApiImpl::ParticleRestart(std::uint32_t id)
{
    if (particleRestart)
        particleRestart(id);
}

void ScriptApiImpl::ParticleEmit(std::uint32_t id, std::uint32_t count)
{
    if (particleEmit)
        particleEmit(id, count);
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

ixscript::RaycastHit ScriptApiImpl::RaycastFiltered(float ox, float oy, float oz,
    float dx, float dy, float dz, float maxDistance, const ixscript::QueryFilter* filter)
{
    const ixscript::QueryFilter f = filter ? *filter : ixscript::QueryFilter{};
    if (!raycastFiltered || f.reserved || (f.flags & ~1u) || !f.layerMask ||
        !std::isfinite(ox) || !std::isfinite(oy) || !std::isfinite(oz) ||
        !std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) ||
        !std::isfinite(maxDistance) || maxDistance <= 0) return {};
    const double len = std::sqrt(double(dx)*dx + double(dy)*dy + double(dz)*dz);
    if (len < 1e-6) return {};
    const float o[3] = {ox, oy, oz};
    const float d[3] = {float(dx/len), float(dy/len), float(dz/len)};
    return raycastFiltered(o, d, maxDistance, f);
}

std::uint32_t ScriptApiImpl::OverlapSphere(float x, float y, float z, float radius,
    const ixscript::QueryFilter* filter, ixscript::SphereOverlapHit* output,
    std::uint32_t capacity, std::uint32_t* truncated)
{
    if (truncated) *truncated = 0;
    const ixscript::QueryFilter f = filter ? *filter : ixscript::QueryFilter{};
    if (!overlapSphere || f.reserved || (f.flags & ~1u) || !f.layerMask ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(radius) || radius <= 0 || (capacity && !output)) return 0;
    const float center[3] = {x,y,z};
    auto hits = overlapSphere(center, radius, f);
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {return a.entityId < b.entityId;});
    std::size_t unique = 0;
    for (const auto hit : hits) {
        if (unique && hits[unique-1].entityId == hit.entityId) hits[unique-1].flags |= hit.flags;
        else hits[unique++] = hit;
    }
    const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(unique,
        std::min(capacity, ixscript::kMaxOverlapResults)));
    if (count) std::copy_n(hits.data(), count, output);
    if (truncated) *truncated = unique > count ? 1u : 0u;
    return count;
}

void ScriptApiImpl::SetEntityEnabled(std::uint32_t id, bool enabled)
{
    if (!id) return;
    DeferredOp op{DeferredKind::SetEnabled, {}, {0,0,0}, id};
    op.enabled = enabled;
    deferredOps.push_back(std::move(op));
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

// --- generic TCP transport: a byte stream only; the game's protocol lives in its scripts ---

namespace
{
constexpr std::size_t kMaxScriptStreams = 16;
constexpr std::size_t kMaxScriptPrompts = 8;
constexpr std::size_t kMaxPromptText = 256;

void WipeText(std::string& text)
{
    // Prompt answers may be passwords: overwrite before releasing the buffer.
    std::fill(text.begin(), text.end(), '\0');
    text.clear();
    text.shrink_to_fit();
}
} // namespace

std::uint32_t ScriptApiImpl::NetConnect(const std::string& host, std::uint32_t port)
{
    if (host.empty() || port == 0 || port > 65535)
    {
        TraceError("[SCRIPT] NetConnect: invalid endpoint '%s:%u'", host.c_str(), port);
        return 0;
    }
    if (m_streams.size() >= kMaxScriptStreams)
    {
        TraceError("[SCRIPT] NetConnect: stream limit (%zu) reached", kMaxScriptStreams);
        return 0;
    }
    std::string error;
    std::unique_ptr<platform::TcpStream> stream =
        platform::TcpStream::Connect(host, static_cast<std::uint16_t>(port), &error);
    if (!stream)
    {
        TraceError("[SCRIPT] NetConnect %s:%u failed: %s", host.c_str(), port, error.c_str());
        return 0;
    }
    const std::uint32_t handle = m_nextStream++;
    m_streams.emplace(handle, std::move(stream));
    return handle;
}

int ScriptApiImpl::NetState(std::uint32_t handle)
{
    const auto it = m_streams.find(handle);
    if (it == m_streams.end())
        return 3;
    switch (it->second->Poll())
    {
    case platform::TcpStream::State::Connecting: return 0;
    case platform::TcpStream::State::Connected: return 1;
    case platform::TcpStream::State::Closed: return 2;
    case platform::TcpStream::State::Failed: return 3;
    }
    return 3;
}

bool ScriptApiImpl::NetSend(std::uint32_t handle, const std::uint8_t* data, std::uint32_t size)
{
    const auto it = m_streams.find(handle);
    if (it == m_streams.end() || (size > 0 && data == nullptr))
        return false;
    return it->second->Send(data, size);
}

std::uint32_t ScriptApiImpl::NetReceive(std::uint32_t handle, std::uint8_t* out, std::uint32_t capacity)
{
    const auto it = m_streams.find(handle);
    if (it == m_streams.end() || out == nullptr || capacity == 0)
        return 0;
    it->second->Poll();
    return static_cast<std::uint32_t>(it->second->Receive(out, capacity));
}

void ScriptApiImpl::NetClose(std::uint32_t handle)
{
    const auto it = m_streams.find(handle);
    if (it == m_streams.end())
        return;
    it->second->Close();
    m_streams.erase(it);
}

// --- text prompts: the script asks, the host UI (editor) draws, the script collects the answer ---

std::uint32_t ScriptApiImpl::PromptText(const std::string& title, const std::string& label, bool secret)
{
    if (!promptsAvailable || m_prompts.size() >= kMaxScriptPrompts)
        return 0;
    const std::uint32_t id = m_nextPrompt++;
    Prompt prompt;
    prompt.title = title.substr(0, 128);
    prompt.label = label.substr(0, 128);
    prompt.secret = secret;
    m_prompts.emplace(id, std::move(prompt));
    return id;
}

int ScriptApiImpl::PromptResult(std::uint32_t promptId, char* out, std::uint32_t capacity)
{
    const auto it = m_prompts.find(promptId);
    if (it == m_prompts.end())
        return -1;
    Prompt& prompt = it->second;
    if (prompt.status == 0)
        return 0;
    const int status = prompt.status;
    if (status == 1 && out != nullptr && capacity > 0)
    {
        const std::size_t n = std::min<std::size_t>(prompt.text.size(), capacity - 1);
        std::memcpy(out, prompt.text.data(), n);
        out[n] = '\0';
    }
    WipeText(prompt.text);
    m_prompts.erase(it);  // the engine forgets the answer once the script has collected it
    return status == 1 ? 1 : -1;
}

void ScriptApiImpl::SubmitPrompt(std::uint32_t id, const std::string& text)
{
    const auto it = m_prompts.find(id);
    if (it == m_prompts.end() || it->second.status != 0)
        return;
    it->second.text.assign(text, 0, kMaxPromptText);  // the caller wipes its own copy
    it->second.status = 1;
}

void ScriptApiImpl::CancelPrompt(std::uint32_t id)
{
    const auto it = m_prompts.find(id);
    if (it != m_prompts.end() && it->second.status == 0)
        it->second.status = -1;
}

void ScriptApiImpl::ResetTransportAndPrompts()
{
    for (auto& [handle, stream] : m_streams)
        stream->Close();
    m_streams.clear();
    for (auto& [id, prompt] : m_prompts)
        WipeText(prompt.text);
    m_prompts.clear();
}
