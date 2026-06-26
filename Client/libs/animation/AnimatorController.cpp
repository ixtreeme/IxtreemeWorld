#include "AnimatorController.h"

#include <cstddef>
#include <cstdlib>
#include <sstream>

namespace ixanim
{
namespace
{

// --- enum <-> string -------------------------------------------------------------------------

const char* ParamTypeName(ParamType t)
{
    switch (t)
    {
    case ParamType::Float: return "float";
    case ParamType::Int: return "int";
    case ParamType::Bool: return "bool";
    case ParamType::Trigger: return "trigger";
    }
    return "float";
}

ParamType ParseParamType(const std::string& s)
{
    if (s == "int") return ParamType::Int;
    if (s == "bool") return ParamType::Bool;
    if (s == "trigger") return ParamType::Trigger;
    return ParamType::Float;
}

const char* ConditionOpName(ConditionOp op)
{
    switch (op)
    {
    case ConditionOp::Greater: return "greater";
    case ConditionOp::Less: return "less";
    case ConditionOp::Equals: return "equals";
    case ConditionOp::NotEquals: return "not_equals";
    case ConditionOp::If: return "if";
    case ConditionOp::IfNot: return "if_not";
    }
    return "greater";
}

ConditionOp ParseConditionOp(const std::string& s)
{
    if (s == "less") return ConditionOp::Less;
    if (s == "equals") return ConditionOp::Equals;
    if (s == "not_equals") return ConditionOp::NotEquals;
    if (s == "if") return ConditionOp::If;
    if (s == "if_not") return ConditionOp::IfNot;
    return ConditionOp::Greater;
}

std::string EscapeJson(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 2);
    for (char c : value)
    {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// --- minimal JSON extraction (flat fields within one object substring) -----------------------

std::string JStr(const std::string& text, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t pos = text.find(needle);
    if (pos == std::string::npos)
        return {};
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return {};
    const std::size_t open = text.find('"', pos + 1);
    if (open == std::string::npos)
        return {};
    const std::size_t close = text.find('"', open + 1);
    if (close == std::string::npos)
        return {};
    return text.substr(open + 1, close - open - 1);
}

bool JHas(const std::string& text, const char* key)
{
    return text.find(std::string("\"") + key + "\"") != std::string::npos;
}

double JNum(const std::string& text, const char* key, double fallback)
{
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t pos = text.find(needle);
    if (pos == std::string::npos)
        return fallback;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return fallback;
    // skip whitespace; handle true/false too
    std::size_t i = pos + 1;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
        ++i;
    if (text.compare(i, 4, "true") == 0)
        return 1.0;
    if (text.compare(i, 5, "false") == 0)
        return 0.0;
    char* end = nullptr;
    const double v = std::strtod(text.c_str() + i, &end);
    return (end != text.c_str() + i) ? v : fallback;
}

bool JBool(const std::string& text, const char* key, bool fallback)
{
    return JNum(text, key, fallback ? 1.0 : 0.0) != 0.0;
}

// Reads a two-element numeric array ("key": [a, b]) into out[0]/out[1]; leaves out unchanged on absence.
void JVec2(const std::string& text, const char* key, float out[2])
{
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos)
        return;
    const std::size_t lb = text.find('[', keyPos + needle.size());
    if (lb == std::string::npos)
        return;
    char* end = nullptr;
    const double a = std::strtod(text.c_str() + lb + 1, &end);
    if (end == text.c_str() + lb + 1)
        return;
    out[0] = static_cast<float>(a);
    const std::size_t comma = text.find(',', static_cast<std::size_t>(end - text.c_str()));
    if (comma == std::string::npos)
        return;
    char* end2 = nullptr;
    const double b = std::strtod(text.c_str() + comma + 1, &end2);
    if (end2 != text.c_str() + comma + 1)
        out[1] = static_cast<float>(b);
}

// Returns the body inside the [...] of the named array (brace-matched), or "" if absent.
std::string ArrayBody(const std::string& text, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t p = text.find(needle);
    if (p == std::string::npos)
        return {};
    const std::size_t lb = text.find('[', p + needle.size());
    if (lb == std::string::npos)
        return {};
    int depth = 0;
    for (std::size_t i = lb; i < text.size(); ++i)
    {
        if (text[i] == '[')
            ++depth;
        else if (text[i] == ']')
        {
            if (--depth == 0)
                return text.substr(lb + 1, i - lb - 1);
        }
    }
    return {};
}

// Splits a JSON array body into its top-level "{...}" object substrings.
std::vector<std::string> SplitObjects(const std::string& body)
{
    std::vector<std::string> out;
    int depth = 0;
    std::size_t start = std::string::npos;
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        if (body[i] == '{')
        {
            if (depth == 0)
                start = i;
            ++depth;
        }
        else if (body[i] == '}')
        {
            if (depth > 0 && --depth == 0 && start != std::string::npos)
            {
                out.push_back(body.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return out;
}

} // namespace

std::string ControllerToJson(const AnimatorController& c)
{
    std::ostringstream o;
    o << "{\n"
      << "  \"version\": 1,\n"
      << "  \"id\": \"" << EscapeJson(c.id) << "\",\n"
      << "  \"display_name\": \"" << EscapeJson(c.displayName) << "\",\n"
      << "  \"default_state_id\": " << c.defaultStateId << ",\n";

    o << "  \"parameters\": [";
    for (std::size_t i = 0; i < c.parameters.size(); ++i)
    {
        const AnimatorParameter& p = c.parameters[i];
        o << (i ? ",\n    " : "\n    ")
          << "{ \"name\": \"" << EscapeJson(p.name) << "\", \"type\": \"" << ParamTypeName(p.type)
          << "\", \"default\": " << p.defaultValue << " }";
    }
    o << (c.parameters.empty() ? "" : "\n  ") << "],\n";

    o << "  \"states\": [";
    for (std::size_t i = 0; i < c.states.size(); ++i)
    {
        const AnimatorState& s = c.states[i];
        o << (i ? ",\n    " : "\n    ")
          << "{ \"id\": " << s.id << ", \"name\": \"" << EscapeJson(s.name)
          << "\", \"clip_id\": \"" << EscapeJson(s.clipId)
          << "\", \"speed\": " << s.speed
          << ", \"speed_param\": \"" << EscapeJson(s.speedParam)
          << "\", \"loop\": " << (s.loop ? 1 : 0)
          << ", \"pos\": [" << s.graphPos[0] << ", " << s.graphPos[1] << "] }";
    }
    o << (c.states.empty() ? "" : "\n  ") << "],\n";

    o << "  \"transitions\": [";
    for (std::size_t i = 0; i < c.transitions.size(); ++i)
    {
        const AnimatorTransition& t = c.transitions[i];
        o << (i ? ",\n    " : "\n    ")
          << "{ \"from\": " << t.fromStateId << ", \"to\": " << t.toStateId
          << ", \"has_exit_time\": " << (t.hasExitTime ? 1 : 0)
          << ", \"exit_time\": " << t.exitTime
          << ", \"duration\": " << t.duration
          << ", \"can_self\": " << (t.canTransitionToSelf ? 1 : 0)
          << ", \"conditions\": [";
        for (std::size_t k = 0; k < t.conditions.size(); ++k)
        {
            const AnimatorCondition& cond = t.conditions[k];
            o << (k ? ", " : "")
              << "{ \"param\": \"" << EscapeJson(cond.param) << "\", \"op\": \"" << ConditionOpName(cond.op)
              << "\", \"value\": " << cond.value << " }";
        }
        o << "] }";
    }
    o << (c.transitions.empty() ? "" : "\n  ") << "]\n}\n";
    return o.str();
}

bool ControllerFromJson(const std::string& text, AnimatorController& out)
{
    out = AnimatorController{};
    if (text.find('{') == std::string::npos)
        return false;
    out.version = static_cast<int>(JNum(text, "version", 1.0));
    out.id = JStr(text, "id");
    out.displayName = JStr(text, "display_name");
    out.defaultStateId = static_cast<std::uint32_t>(JNum(text, "default_state_id", 0.0));

    for (const std::string& obj : SplitObjects(ArrayBody(text, "parameters")))
    {
        AnimatorParameter p;
        p.name = JStr(obj, "name");
        p.type = ParseParamType(JStr(obj, "type"));
        p.defaultValue = static_cast<float>(JNum(obj, "default", 0.0));
        out.parameters.push_back(std::move(p));
    }

    for (const std::string& obj : SplitObjects(ArrayBody(text, "states")))
    {
        AnimatorState s;
        s.id = static_cast<std::uint32_t>(JNum(obj, "id", 0.0));
        s.name = JStr(obj, "name");
        s.clipId = JStr(obj, "clip_id");
        s.speed = static_cast<float>(JNum(obj, "speed", 1.0));
        s.speedParam = JStr(obj, "speed_param");
        s.loop = JBool(obj, "loop", true);
        JVec2(obj, "pos", s.graphPos);
        out.states.push_back(std::move(s));
    }

    for (const std::string& obj : SplitObjects(ArrayBody(text, "transitions")))
    {
        AnimatorTransition t;
        t.fromStateId = static_cast<std::uint32_t>(JNum(obj, "from", 0.0));
        t.toStateId = static_cast<std::uint32_t>(JNum(obj, "to", 0.0));
        t.hasExitTime = JBool(obj, "has_exit_time", false);
        t.exitTime = static_cast<float>(JNum(obj, "exit_time", 0.0));
        t.duration = static_cast<float>(JNum(obj, "duration", 0.15));
        t.canTransitionToSelf = JBool(obj, "can_self", false);
        for (const std::string& cobj : SplitObjects(ArrayBody(obj, "conditions")))
        {
            AnimatorCondition cond;
            cond.param = JStr(cobj, "param");
            cond.op = ParseConditionOp(JStr(cobj, "op"));
            cond.value = static_cast<float>(JNum(cobj, "value", 0.0));
            t.conditions.push_back(std::move(cond));
        }
        out.transitions.push_back(std::move(t));
    }
    return true;
}

AnimatorController MakeDefaultLocomotionController(const std::string& id,
                                                  const std::string& displayName,
                                                  const std::string& idleClipId,
                                                  const std::string& walkClipId,
                                                  const std::string& runClipId)
{
    AnimatorController c;
    c.id = id;
    c.displayName = displayName;
    c.parameters = {
        {"Speed", ParamType::Float, 0.0f},
        {"IsGrounded", ParamType::Bool, 1.0f},
        {"Jump", ParamType::Trigger, 0.0f},
    };
    c.states = {
        {1u, "Idle", idleClipId, 1.0f, "", true, {120.0f, 80.0f}},
        {2u, "Walk", walkClipId, 1.0f, "", true, {340.0f, 80.0f}},
        {3u, "Run", runClipId, 1.0f, "", true, {560.0f, 80.0f}},
    };
    c.defaultStateId = 1u;
    c.transitions = {
        {1u, 2u, false, 0.0f, 0.15f, false, {{"Speed", ConditionOp::Greater, 0.1f}}},
        {2u, 1u, false, 0.0f, 0.15f, false, {{"Speed", ConditionOp::Less, 0.1f}}},
        {2u, 3u, false, 0.0f, 0.15f, false, {{"Speed", ConditionOp::Greater, 3.5f}}},
        {3u, 2u, false, 0.0f, 0.15f, false, {{"Speed", ConditionOp::Less, 3.5f}}},
    };
    return c;
}

} // namespace ixanim
