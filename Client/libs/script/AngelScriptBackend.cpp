#include "AngelScriptBackend.h"

#include "ScriptApi.h"
#include "Debug.h"

#include <angelscript.h>
#include <angelscript/scriptarray/scriptarray.h>
#include <angelscript/scriptstdstring/scriptstdstring.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace ixscript
{
namespace
{

// --- value types exposed to scripts (floats/ints only: POD, safe to pass and return by value) ------

struct AsFloat2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct AsFloat3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct AsRaycastHit
{
    std::uint32_t entityId = 0;
    float distance = 0.0f;
    bool hit = false;
    AsFloat3 point;
    AsFloat3 normal;
};

struct AsCharacterState
{
    bool grounded = false;
    bool moving = false;
    bool running = false;
    bool jumped = false;
    float planarSpeed = 0.0f;
};

// --- backend state the registered global functions reach ----------------------------------------

class AngelScriptScriptInstance;

struct BackendState
{
    ScriptApi* api = nullptr;                  // valid for the Play session (outlives the backend)
    AngelScriptScriptInstance* current = nullptr;  // the instance whose hook is executing
    std::string compilingAsset;                // asset id while a module compiles (message callback)
};

// ScriptSystem owns exactly one AngelScriptBackend per Play session; the registered C functions need
// a way back to it (AngelScript global functions carry no user pointer). Single-threaded.
BackendState* g_state = nullptr;

ScriptApi& Api()
{
    return *g_state->api;
}

// --- the engine API bound as global script functions (mirrors the Lua surface 1:1) ----------------

std::uint32_t AsFind(const std::string& name) { return Api().FindEntityByName(name); }
bool AsEntityExists(std::uint32_t id) { return Api().EntityExists(id); }
std::string AsGetName(std::uint32_t id) { return Api().GetEntityName(id); }

AsFloat3 AsGetPosition(std::uint32_t id)
{
    AsFloat3 out;
    Api().GetPosition(id, &out.x);
    return out;
}
void AsSetPosition(std::uint32_t id, float x, float y, float z)
{
    const float p[3] = {x, y, z};
    Api().SetPosition(id, p);
}
AsFloat3 AsGetRotation(std::uint32_t id)
{
    AsFloat3 out;
    Api().GetRotation(id, &out.x);
    return out;
}
void AsSetRotation(std::uint32_t id, float x, float y, float z)
{
    const float r[3] = {x, y, z};
    Api().SetRotation(id, r);
}
AsFloat3 AsGetScale(std::uint32_t id)
{
    AsFloat3 out;
    Api().GetScale(id, &out.x);
    return out;
}
void AsSetScale(std::uint32_t id, float x, float y, float z)
{
    const float s[3] = {x, y, z};
    Api().SetScale(id, s);
}

bool AsIsKeyDown(const std::string& key)
{
    ScriptKey parsed;
    return ParseKeyName(key, parsed) && Api().IsKeyDown(parsed);
}
void AsGetMouseDelta(AsFloat2* out)
{
    if (!out)
        return;
    Api().GetMouseDelta(out->x, out->y);
}
double AsDeltaTime() { return Api().GetDeltaTime(); }
double AsElapsedTime() { return Api().GetElapsedTime(); }

void AsPlayOneShot(const std::string& clipAssetId) { Api().PlayOneShot(clipAssetId); }
void AsLog(const std::string& message) { Api().Log(message); }
void AsLogError(const std::string& message) { Api().LogError(message); }

std::uint32_t AsSpawnMesh(const std::string& meshAssetId, float x, float y, float z)
{
    return Api().SpawnMesh(meshAssetId, x, y, z);
}
std::uint32_t AsSpawnPrefab(const std::string& prefabAssetId, float x, float y, float z)
{
    return Api().SpawnPrefab(prefabAssetId, x, y, z);
}
void AsDestroyEntity(std::uint32_t id) { Api().DestroyEntity(id); }

bool AsRaycast(float ox, float oy, float oz, float dx, float dy, float dz, float maxDist, AsRaycastHit* out)
{
    if (!out)
        return false;
    const RaycastHit hit = Api().Raycast(ox, oy, oz, dx, dy, dz, maxDist);
    out->entityId = hit.entityId;
    out->distance = hit.distance;
    out->hit = hit.hit;
    out->point = {hit.point[0], hit.point[1], hit.point[2]};
    out->normal = {hit.normal[0], hit.normal[1], hit.normal[2]};
    return hit.hit;
}

void AsSetAnimatorFloat(std::uint32_t id, const std::string& name, float value)
{
    Api().SetAnimatorFloat(id, name, value);
}
void AsSetAnimatorBool(std::uint32_t id, const std::string& name, bool value)
{
    Api().SetAnimatorBool(id, name, value);
}
void AsSetAnimatorTrigger(std::uint32_t id, const std::string& name)
{
    Api().SetAnimatorTrigger(id, name);
}

std::uint32_t AsNetConnect(const std::string& host, std::uint32_t port) { return Api().NetConnect(host, port); }
int AsNetState(std::uint32_t handle) { return Api().NetState(handle); }
bool AsNetSend(std::uint32_t handle, const std::string& bytes)
{
    return Api().NetSend(handle, reinterpret_cast<const std::uint8_t*>(bytes.data()),
        static_cast<std::uint32_t>(bytes.size()));
}
std::string AsNetReceive(std::uint32_t handle, std::uint32_t maxBytes)
{
    const std::uint32_t capacity = std::min<std::uint32_t>(maxBytes, 1u << 20);
    std::string bytes(capacity, '\0');
    if (bytes.empty())
        return bytes;
    bytes.resize(Api().NetReceive(handle, reinterpret_cast<std::uint8_t*>(bytes.data()), capacity));
    return bytes;
}
void AsNetClose(std::uint32_t handle) { Api().NetClose(handle); }

std::uint32_t AsPromptText(const std::string& title, const std::string& label, bool secret)
{
    return Api().PromptText(title, label, secret);
}
int AsPromptResult(std::uint32_t promptId, std::string* out)
{
    char buffer[257] = {};
    const int status = Api().PromptResult(promptId, buffer, sizeof(buffer));
    if (out)
    {
        if (status == 1)
            out->assign(buffer);
        else
            out->clear();
    }
    std::memset(buffer, 0, sizeof(buffer));
    return status;
}

void AsSetMaterial(std::uint32_t id, std::uint32_t slot, const std::string& materialAssetId)
{
    Api().SetMaterial(id, slot, materialAssetId);
}
bool AsGetCharacterState(std::uint32_t id, AsCharacterState* out)
{
    if (!out)
        return false;
    CharacterState state{};
    if (!Api().GetCharacterState(id, state))
        return false;
    out->grounded = state.grounded;
    out->moving = state.moving;
    out->running = state.running;
    out->jumped = state.jumped;
    out->planarSpeed = state.planarSpeed;
    return true;
}
void AsSetCharacterAbilities(std::uint32_t id, bool canRun, bool canJump)
{
    Api().SetCharacterAbilities(id, canRun, canJump);
}

std::uint32_t AsUiOpen(const std::string& documentPath) { return Api().UiOpen(documentPath); }
void AsUiClose(std::uint32_t document) { Api().UiClose(document); }
void AsUiSetVisible(std::uint32_t document, bool visible) { Api().UiSetVisible(document, visible); }
void AsUiSetText(std::uint32_t document, const std::string& elementId, const std::string& text)
{
    Api().UiSetText(document, elementId, text);
}
void AsUiSetProperty(std::uint32_t document, const std::string& elementId, const std::string& property,
                     const std::string& value)
{
    Api().UiSetProperty(document, elementId, property, value);
}
void AsUiSetClass(std::uint32_t document, const std::string& elementId, const std::string& className,
                  bool enabled)
{
    Api().UiSetClass(document, elementId, className, enabled);
}
bool AsUiConsumeClick(std::uint32_t document, const std::string& elementId)
{
    return Api().UiConsumeClick(document, elementId);
}

void AsParticlePlay(std::uint32_t id) { Api().ParticlePlay(id); }
void AsParticleStop(std::uint32_t id) { Api().ParticleStop(id); }
void AsParticleRestart(std::uint32_t id) { Api().ParticleRestart(id); }
void AsParticleEmit(std::uint32_t id, std::uint32_t count) { Api().ParticleEmit(id, count); }

// `self` equivalents (defined after the instance class: they read the executing instance).
std::uint32_t AsSelf();
std::string AsParam(const std::string& key);

// --- one running script object on one entity ------------------------------------------------------

class AngelScriptScriptInstance final : public ScriptInstance
{
public:
    AngelScriptScriptInstance(std::uint32_t entityId,
                              asIScriptObject* object,
                              asIScriptContext* context,
                              asIScriptFunction* onStart,
                              asIScriptFunction* onUpdate,
                              asIScriptFunction* onDestroy,
                              asIScriptFunction* onCollision,
                              std::map<std::string, std::string> parameters)
        : m_entityId(entityId)
        , m_object(object)
        , m_context(context)
        , m_onStart(onStart)
        , m_onUpdate(onUpdate)
        , m_onDestroy(onDestroy)
        , m_onCollision(onCollision)
        , m_parameters(std::move(parameters))
    {
    }

    ~AngelScriptScriptInstance() override
    {
        if (m_context)
            m_context->Release();
        if (m_object)
            m_object->Release();
    }

    AngelScriptScriptInstance(const AngelScriptScriptInstance&) = delete;
    AngelScriptScriptInstance& operator=(const AngelScriptScriptInstance&) = delete;

    void OnStart() override
    {
        if (!Prepare(m_onStart))
            return;
        m_context->SetObject(m_object);
        Execute("OnStart");
    }
    void OnUpdate(float dtSeconds) override
    {
        if (!Prepare(m_onUpdate))
            return;
        m_context->SetObject(m_object);
        m_context->SetArgFloat(0, dtSeconds);
        Execute("OnUpdate");
    }
    void OnDestroy() override
    {
        if (!Prepare(m_onDestroy))
            return;
        m_context->SetObject(m_object);
        Execute("OnDestroy");
    }
    void OnCollision(std::uint32_t otherEntityId) override
    {
        if (!Prepare(m_onCollision))
            return;
        m_context->SetObject(m_object);
        m_context->SetArgDWord(0, static_cast<asDWORD>(otherEntityId));
        Execute("OnCollision");
    }

    std::uint32_t EntityId() const { return m_entityId; }
    const std::string& Param(const std::string& key) const
    {
        static const std::string empty;
        const auto it = m_parameters.find(key);
        return it == m_parameters.end() ? empty : it->second;
    }

private:
    bool Prepare(asIScriptFunction* function)
    {
        if (m_dead || !m_context || !function || !m_object)
            return false;
        return m_context->Prepare(function) >= 0;
    }

    void Execute(const char* hook)
    {
        AngelScriptScriptInstance* previous = g_state ? g_state->current : nullptr;
        if (g_state)
            g_state->current = this;
        const int result = m_context->Execute();
        if (g_state)
            g_state->current = previous;
        if (result != asEXECUTION_FINISHED)
        {
            const int line = m_context->GetExceptionLineNumber(nullptr, nullptr);
            const char* what = m_context->GetExceptionString();  // valid while the context holds it
            std::string where;
            if (line > 0)
                where = ":" + std::to_string(line);
            TraceError("[SCRIPT][angelscript] entity=%u %s error%s: %s",
                m_entityId, hook, where.c_str(), what ? what : "unknown");
            m_dead = true;  // one bad call retires this instance; never spams the frame
        }
        m_context->Unprepare();
    }

    std::uint32_t m_entityId = 0;
    asIScriptObject* m_object = nullptr;    // refcounted; released in the dtor
    asIScriptContext* m_context = nullptr;  // one per instance; released in the dtor
    asIScriptFunction* m_onStart = nullptr;
    asIScriptFunction* m_onUpdate = nullptr;
    asIScriptFunction* m_onDestroy = nullptr;
    asIScriptFunction* m_onCollision = nullptr;
    std::map<std::string, std::string> m_parameters;
    bool m_dead = false;
};

std::uint32_t AsSelf() { return g_state->current ? g_state->current->EntityId() : 0; }
std::string AsParam(const std::string& key)
{
    return g_state->current ? g_state->current->Param(key) : std::string();
}

// --- registration helpers -------------------------------------------------------------------------

template <typename Fn>
void RegisterGlobal(asIScriptEngine* engine, const char* declaration, Fn function)
{
    const int result = engine->RegisterGlobalFunction(declaration, asFUNCTION(function), asCALL_CDECL);
    if (result < 0)
        TraceError("[SCRIPT][angelscript] RegisterGlobalFunction failed (%d): %s", result, declaration);
}

void RegisterValueTypes(asIScriptEngine* engine)
{
    const asQWORD floatFlags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_CAK | asOBJ_APP_CLASS_ALLFLOATS;
    const asQWORD podFlags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_CAK;

    int result = 0;
    // float2 is only ever passed BY REFERENCE (out params): an 8-byte all-float POD returned by
    // value hits an AngelScript/MSVC x64 register-return mismatch, so no binding returns it.
    result = engine->RegisterObjectType("float2", sizeof(AsFloat2), podFlags);
    if (result < 0) TraceError("[SCRIPT][angelscript] RegisterObjectType float2 failed (%d)", result);
    result = engine->RegisterObjectProperty("float2", "float x", static_cast<int>(offsetof(AsFloat2, x)));
    result = engine->RegisterObjectProperty("float2", "float y", static_cast<int>(offsetof(AsFloat2, y)));

    result = engine->RegisterObjectType("float3", sizeof(AsFloat3), floatFlags);
    if (result < 0) TraceError("[SCRIPT][angelscript] RegisterObjectType float3 failed (%d)", result);
    result = engine->RegisterObjectProperty("float3", "float x", static_cast<int>(offsetof(AsFloat3, x)));
    result = engine->RegisterObjectProperty("float3", "float y", static_cast<int>(offsetof(AsFloat3, y)));
    result = engine->RegisterObjectProperty("float3", "float z", static_cast<int>(offsetof(AsFloat3, z)));

    result = engine->RegisterObjectType("RaycastHit", sizeof(AsRaycastHit), podFlags);
    if (result < 0) TraceError("[SCRIPT][angelscript] RegisterObjectType RaycastHit failed (%d)", result);
    result = engine->RegisterObjectProperty("RaycastHit", "uint entityId", static_cast<int>(offsetof(AsRaycastHit, entityId)));
    result = engine->RegisterObjectProperty("RaycastHit", "float distance", static_cast<int>(offsetof(AsRaycastHit, distance)));
    result = engine->RegisterObjectProperty("RaycastHit", "bool hit", static_cast<int>(offsetof(AsRaycastHit, hit)));
    result = engine->RegisterObjectProperty("RaycastHit", "float3 point", static_cast<int>(offsetof(AsRaycastHit, point)));
    result = engine->RegisterObjectProperty("RaycastHit", "float3 normal", static_cast<int>(offsetof(AsRaycastHit, normal)));

    result = engine->RegisterObjectType("CharacterState", sizeof(AsCharacterState), podFlags);
    if (result < 0) TraceError("[SCRIPT][angelscript] RegisterObjectType CharacterState failed (%d)", result);
    result = engine->RegisterObjectProperty("CharacterState", "bool grounded", static_cast<int>(offsetof(AsCharacterState, grounded)));
    result = engine->RegisterObjectProperty("CharacterState", "bool moving", static_cast<int>(offsetof(AsCharacterState, moving)));
    result = engine->RegisterObjectProperty("CharacterState", "bool running", static_cast<int>(offsetof(AsCharacterState, running)));
    result = engine->RegisterObjectProperty("CharacterState", "bool jumped", static_cast<int>(offsetof(AsCharacterState, jumped)));
    result = engine->RegisterObjectProperty("CharacterState", "float planarSpeed", static_cast<int>(offsetof(AsCharacterState, planarSpeed)));
    (void)result;
}

void RegisterBindings(asIScriptEngine* engine)
{
    RegisterGlobal(engine, "uint Find(const string &in name)", &AsFind);
    RegisterGlobal(engine, "bool EntityExists(uint id)", &AsEntityExists);
    RegisterGlobal(engine, "string GetName(uint id)", &AsGetName);

    RegisterGlobal(engine, "float3 GetPosition(uint id)", &AsGetPosition);
    RegisterGlobal(engine, "void SetPosition(uint id, float x, float y, float z)", &AsSetPosition);
    RegisterGlobal(engine, "float3 GetRotation(uint id)", &AsGetRotation);
    RegisterGlobal(engine, "void SetRotation(uint id, float x, float y, float z)", &AsSetRotation);
    RegisterGlobal(engine, "float3 GetScale(uint id)", &AsGetScale);
    RegisterGlobal(engine, "void SetScale(uint id, float x, float y, float z)", &AsSetScale);

    RegisterGlobal(engine, "bool IsKeyDown(const string &in key)", &AsIsKeyDown);
    RegisterGlobal(engine, "void MouseDelta(float2 &out delta)", &AsGetMouseDelta);
    RegisterGlobal(engine, "double DeltaTime()", &AsDeltaTime);
    RegisterGlobal(engine, "double ElapsedTime()", &AsElapsedTime);

    RegisterGlobal(engine, "void PlayOneShot(const string &in clipAssetId)", &AsPlayOneShot);
    RegisterGlobal(engine, "void Log(const string &in message)", &AsLog);
    RegisterGlobal(engine, "void LogError(const string &in message)", &AsLogError);

    RegisterGlobal(engine, "uint SpawnMesh(const string &in meshAssetId, float x, float y, float z)", &AsSpawnMesh);
    RegisterGlobal(engine, "uint SpawnPrefab(const string &in prefabAssetId, float x, float y, float z)", &AsSpawnPrefab);
    RegisterGlobal(engine, "void DestroyEntity(uint id)", &AsDestroyEntity);
    RegisterGlobal(engine, "bool Raycast(float ox, float oy, float oz, float dx, float dy, float dz, float maxDist, RaycastHit &out hit)", &AsRaycast);

    RegisterGlobal(engine, "void SetAnimatorFloat(uint id, const string &in name, float value)", &AsSetAnimatorFloat);
    RegisterGlobal(engine, "void SetAnimatorBool(uint id, const string &in name, bool value)", &AsSetAnimatorBool);
    RegisterGlobal(engine, "void SetAnimatorTrigger(uint id, const string &in name)", &AsSetAnimatorTrigger);

    RegisterGlobal(engine, "uint NetConnect(const string &in host, uint port)", &AsNetConnect);
    RegisterGlobal(engine, "int NetState(uint handle)", &AsNetState);
    RegisterGlobal(engine, "bool NetSend(uint handle, const string &in bytes)", &AsNetSend);
    RegisterGlobal(engine, "string NetReceive(uint handle, uint maxBytes = 65536)", &AsNetReceive);
    RegisterGlobal(engine, "void NetClose(uint handle)", &AsNetClose);
    RegisterGlobal(engine, "uint PromptText(const string &in title, const string &in label, bool secret = false)", &AsPromptText);
    RegisterGlobal(engine, "int PromptResult(uint promptId, string &out text)", &AsPromptResult);

    RegisterGlobal(engine, "void SetMaterial(uint id, uint slot, const string &in materialAssetId)", &AsSetMaterial);
    RegisterGlobal(engine, "bool GetCharacterState(uint id, CharacterState &out state)", &AsGetCharacterState);
    RegisterGlobal(engine, "void SetCharacterAbilities(uint id, bool canRun, bool canJump)", &AsSetCharacterAbilities);

    RegisterGlobal(engine, "uint UiOpen(const string &in documentPath)", &AsUiOpen);
    RegisterGlobal(engine, "void UiClose(uint document)", &AsUiClose);
    RegisterGlobal(engine, "void UiSetVisible(uint document, bool visible)", &AsUiSetVisible);
    RegisterGlobal(engine, "void UiSetText(uint document, const string &in elementId, const string &in text)", &AsUiSetText);
    RegisterGlobal(engine, "void UiSetProperty(uint document, const string &in elementId, const string &in property, const string &in value)", &AsUiSetProperty);
    RegisterGlobal(engine, "void UiSetClass(uint document, const string &in elementId, const string &in className, bool enabled)", &AsUiSetClass);
    RegisterGlobal(engine, "bool UiConsumeClick(uint document, const string &in elementId)", &AsUiConsumeClick);

    RegisterGlobal(engine, "void ParticlePlay(uint id)", &AsParticlePlay);
    RegisterGlobal(engine, "void ParticleStop(uint id)", &AsParticleStop);
    RegisterGlobal(engine, "void ParticleRestart(uint id)", &AsParticleRestart);
    RegisterGlobal(engine, "void ParticleEmit(uint id, uint count)", &AsParticleEmit);

    RegisterGlobal(engine, "uint Self()", &AsSelf);
    RegisterGlobal(engine, "string Param(const string &in key)", &AsParam);
}

void MessageCallback(const asSMessageInfo* message, void*)
{
    const bool compiling = g_state && !g_state->compilingAsset.empty();
    const char* asset = compiling ? g_state->compilingAsset.c_str() : "";
    const char* section = message->section ? message->section : "?";
    // Runtime exceptions are reported with entity context by the instance's Execute(); only compile
    // diagnostics (while an asset builds) are worth an ERROR here.
    if (message->type == asMSGTYPE_ERROR && compiling)
        TraceError("[SCRIPT][angelscript] %s %s:%d %s", asset, section, message->row, message->message);
    else
        Tracenf("[SCRIPT][angelscript] %s%s%s:%d %s", asset, *asset ? " " : "", section, message->row, message->message);
}

// One compiled .as asset: its module + cached hook functions. `stale` is set by hot-reload; the next
// CreateInstance compiles a fresh module (the old one stays in the engine until teardown so any
// still-live instance's object stays valid).
struct AsModuleEntry
{
    std::string moduleName;
    asITypeInfo* scriptType = nullptr;
    asIScriptFunction* onStart = nullptr;
    asIScriptFunction* onUpdate = nullptr;
    asIScriptFunction* onDestroy = nullptr;
    asIScriptFunction* onCollision = nullptr;
    bool stale = false;
};

} // namespace

struct AngelScriptBackend::Impl
{
    asIScriptEngine* engine = nullptr;
    std::unordered_map<std::string, std::unique_ptr<AsModuleEntry>> modules;
    BackendState state;
    std::uint32_t generation = 0;

    Impl()
    {
        engine = asCreateScriptEngine();
        if (!engine)
        {
            TraceError("[SCRIPT][angelscript] engine creation failed");
            return;
        }
        g_state = &state;
        engine->SetMessageCallback(asFUNCTION(MessageCallback), nullptr, asCALL_CDECL);
        RegisterStdString(engine);       // std::string as the script `string` type
        RegisterScriptArray(engine, true);  // array<T> (scriptstdstring_utils split/join needs it)
        RegisterStdStringUtils(engine);  // formatInt/formatFloat/parseInt/... helpers
        RegisterValueTypes(engine);
        RegisterBindings(engine);
    }

    ~Impl()
    {
        g_state = nullptr;
        if (engine)
            engine->ShutDownAndRelease();
        engine = nullptr;
    }

    static void SetPropertyFromString(asIScriptEngine* scriptEngine, asIScriptObject* object, asUINT index,
                                      const std::string& value)
    {
        void* address = object->GetAddressOfProperty(index);
        if (!address)
            return;
        switch (object->GetPropertyTypeId(index))
        {
        case asTYPEID_BOOL:
            *static_cast<bool*>(address) = (value == "true" || value == "1");
            break;
        case asTYPEID_INT32:
            *static_cast<std::int32_t*>(address) = static_cast<std::int32_t>(std::strtol(value.c_str(), nullptr, 10));
            break;
        case asTYPEID_UINT32:
            *static_cast<std::uint32_t*>(address) = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
            break;
        case asTYPEID_FLOAT:
            *static_cast<float*>(address) = std::strtof(value.c_str(), nullptr);
            break;
        case asTYPEID_DOUBLE:
            *static_cast<double*>(address) = std::strtod(value.c_str(), nullptr);
            break;
        default:
        {
            const char* declaration = scriptEngine->GetTypeDeclaration(object->GetPropertyTypeId(index));
            if (declaration && std::strcmp(declaration, "string") == 0)
                *static_cast<std::string*>(address) = value;
            break;
        }
        }
    }

    // Fills same-named class members before OnStart: `id`/`entityId` (uint) gets the entity id, every
    // component parameter whose name matches a member sets that member (typed by the member).
    void FillProperties(asIScriptObject* object, std::uint32_t entityId,
                        const std::map<std::string, std::string>& parameters)
    {
        const asUINT count = object->GetPropertyCount();
        for (asUINT index = 0; index < count; ++index)
        {
            const char* name = object->GetPropertyName(index);
            if (!name)
                continue;
            if ((std::strcmp(name, "id") == 0 || std::strcmp(name, "entityId") == 0) &&
                object->GetPropertyTypeId(index) == asTYPEID_UINT32)
            {
                *static_cast<std::uint32_t*>(object->GetAddressOfProperty(index)) = entityId;
                continue;
            }
            const auto it = parameters.find(name);
            if (it != parameters.end())
                SetPropertyFromString(engine, object, index, it->second);
        }
    }

    std::unique_ptr<AsModuleEntry> CompileModule(ScriptApi& api, const std::string& assetId)
    {
        const std::string source = api.LoadScriptSource(assetId);
        if (source.empty())
        {
            api.LogError("AngelScript source '" + assetId + "' could not be loaded (entity skipped)");
            return nullptr;
        }

        const std::string moduleName = "ixas:" + assetId + ":" + std::to_string(++generation);
        asIScriptModule* module = engine->GetModule(moduleName.c_str(), asGM_ALWAYS_CREATE);
        if (!module)
        {
            api.LogError("AngelScript module creation failed for '" + assetId + "'");
            return nullptr;
        }
        module->AddScriptSection(assetId.c_str(), source.c_str(), source.size());
        state.compilingAsset = assetId;
        const int buildResult = module->Build();
        state.compilingAsset.clear();
        if (buildResult < 0)
        {
            api.LogError("AngelScript compile error in '" + assetId + "' (see the log for details)");
            engine->DiscardModule(moduleName.c_str());
            return nullptr;
        }

        asITypeInfo* type = module->GetTypeInfoByDecl("Script");
        if (!type)
        {
            api.LogError("AngelScript '" + assetId + "' must define a class named Script "
                         "(create scripts with the editor template)");
            engine->DiscardModule(moduleName.c_str());
            return nullptr;
        }

        auto entry = std::make_unique<AsModuleEntry>();
        entry->moduleName = moduleName;
        entry->scriptType = type;
        entry->onStart = type->GetMethodByDecl("void OnStart()");
        entry->onUpdate = type->GetMethodByDecl("void OnUpdate(float)");
        entry->onDestroy = type->GetMethodByDecl("void OnDestroy()");
        entry->onCollision = type->GetMethodByDecl("void OnCollision(uint)");
        return entry;
    }

    AsModuleEntry* Module(ScriptApi& api, const std::string& assetId)
    {
        const auto it = modules.find(assetId);
        if (it != modules.end())
        {
            if (!it->second->stale)
                return it->second.get();
            modules.erase(it);  // stale bookkeeping only; the old module stays alive in the engine
        }
        std::unique_ptr<AsModuleEntry> entry = CompileModule(api, assetId);
        if (!entry)
            return nullptr;
        AsModuleEntry* raw = entry.get();
        modules.emplace(assetId, std::move(entry));
        return raw;
    }
};

AngelScriptBackend::AngelScriptBackend() : m_impl(std::make_unique<Impl>()) {}
AngelScriptBackend::~AngelScriptBackend() = default;

void AngelScriptBackend::InvalidateSource(const std::string& assetId)
{
    if (!m_impl)
        return;
    const auto it = m_impl->modules.find(assetId);
    if (it != m_impl->modules.end())
        it->second->stale = true;
}

std::unique_ptr<ScriptInstance> AngelScriptBackend::CreateInstance(std::uint32_t entityId,
                                                                   const ScriptComponent& comp,
                                                                   ScriptApi& api)
{
    if (!m_impl || !m_impl->engine)
    {
        api.LogError("AngelScript engine is not available (entity skipped)");
        return nullptr;
    }
    if (comp.scriptAssetId.empty())
    {
        api.LogError("AngelScript script component has no script asset assigned (entity skipped)");
        return nullptr;
    }
    m_impl->state.api = &api;

    AsModuleEntry* entry = m_impl->Module(api, comp.scriptAssetId);
    if (!entry)
        return nullptr;

    asIScriptObject* object =
        static_cast<asIScriptObject*>(m_impl->engine->CreateScriptObject(entry->scriptType));
    if (!object)
    {
        api.LogError("AngelScript object creation failed for '" + comp.scriptAssetId + "'");
        return nullptr;
    }
    m_impl->FillProperties(object, entityId, comp.parameters);

    asIScriptContext* context = m_impl->engine->CreateContext();
    if (!context)
    {
        object->Release();
        api.LogError("AngelScript context creation failed for '" + comp.scriptAssetId + "'");
        return nullptr;
    }

    return std::make_unique<AngelScriptScriptInstance>(entityId, object, context, entry->onStart,
        entry->onUpdate, entry->onDestroy, entry->onCollision, comp.parameters);
}

} // namespace ixscript
