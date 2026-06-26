#pragma once

// Stage 4: the AnimatorController asset data model (a Unity-Mecanim-style state machine).
// Pure data + JSON (no Vulkan/ImGui). v1 = single flat machine, clip-per-state, conditional
// transitions; blend trees (Stage 5) and the node-graph editor (Stages 6-7) come later.
// The graph lives ONLY in the .controller asset; the scene/prefab just references it by id.

#include <string>
#include <vector>

namespace ixanim
{

enum class ParamType
{
    Float,
    Int,
    Bool,
    Trigger
};

struct AnimatorParameter
{
    std::string name;
    ParamType type = ParamType::Float;
    float defaultValue = 0.0f;  // Bool/Trigger as 0/1, Int as a whole number stored in float
};

// A transition condition: ALL conditions on a transition must pass (AND).
enum class ConditionOp
{
    Greater,    // param > value          (Float/Int)
    Less,       // param < value          (Float/Int)
    Equals,     // param == value         (Int)
    NotEquals,  // param != value         (Int)
    If,         // param is true          (Bool/Trigger)
    IfNot       // param is false         (Bool)
};

struct AnimatorCondition
{
    std::string param;
    ConditionOp op = ConditionOp::Greater;
    float value = 0.0f;
};

// Stage 5: a state's motion is either a single clip or a blend tree (children blended by a param).
enum class BlendTreeType
{
    Single,   // use AnimatorState::clipId (legacy / default)
    Blend1D,  // blend children along one Float param (threshold per child)
    Blend2D   // blend children in a 2D param space (position per child) — Chunk 4
};

struct BlendTreeChild
{
    std::string clipId;
    float threshold = 0.0f;            // Blend1D: param value at which this child reaches full weight
    float position[2] = {0.0f, 0.0f};  // Blend2D: child position in (X,Y) param space
};

struct BlendTree
{
    BlendTreeType type = BlendTreeType::Single;
    std::string blendParam;            // X-axis Float param (Blend1D + Blend2D)
    std::string blendParamY;           // Y-axis Float param (Blend2D only)
    std::vector<BlendTreeChild> children;  // Blend1D: kept sorted by threshold (at apply/bind time)
};

struct AnimatorState
{
    std::uint32_t id = 0;
    std::string name;
    std::string clipId;        // AnimationClip asset id (single-clip motion; blend trees later)
    float speed = 1.0f;
    std::string speedParam;    // optional Float param multiplying speed
    bool loop = true;
    float graphPos[2] = {0.0f, 0.0f};  // node-graph editor position (Stage 6)
    BlendTree blendTree;       // Stage 5: motion source when type != Single (else clipId is used)
};

static constexpr std::uint32_t kAnyStateId = 0xFFFFFFFFu;

struct AnimatorTransition
{
    std::uint32_t fromStateId = 0;  // kAnyStateId = Any-State (evaluated first, highest priority)
    std::uint32_t toStateId = 0;
    bool hasExitTime = false;
    float exitTime = 0.0f;          // normalized 0..1 of the source clip
    float duration = 0.15f;         // crossfade seconds
    bool canTransitionToSelf = false;
    std::vector<AnimatorCondition> conditions;
};

struct AnimatorController
{
    int version = 1;
    std::string id;
    std::string displayName;
    std::vector<AnimatorParameter> parameters;
    std::vector<AnimatorState> states;
    std::uint32_t defaultStateId = 0;
    std::vector<AnimatorTransition> transitions;  // includes Any-State transitions (fromStateId==kAnyStateId)
};

// JSON serialization (hand-rolled, matching the codebase convention).
std::string ControllerToJson(const AnimatorController& controller);
// Parses a .controller body. Returns false on malformed input (out is left default).
bool ControllerFromJson(const std::string& text, AnimatorController& out);

// A ready-to-use idle/walk/run locomotion controller driven by the auto-bound "Speed" param.
// clipIds may be empty (states with no clip render their rest pose) — the editor/user fills them.
AnimatorController MakeDefaultLocomotionController(const std::string& id,
                                                  const std::string& displayName,
                                                  const std::string& idleClipId = {},
                                                  const std::string& walkClipId = {},
                                                  const std::string& runClipId = {});

} // namespace ixanim
