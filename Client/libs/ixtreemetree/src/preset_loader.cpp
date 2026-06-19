#include "ixtreemetree/preset.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>

namespace ixtreemetree
{
namespace
{
struct JsonValue
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

class JsonParser
{
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool parse(JsonValue& out, std::string& error)
    {
        skipWhitespace();
        if (!parseValue(out, error))
            return false;
        skipWhitespace();
        if (pos_ != text_.size())
        {
            error = "unexpected trailing characters";
            return false;
        }
        return true;
    }

private:
    void skipWhitespace()
    {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])))
            ++pos_;
    }

    bool consume(char expected)
    {
        skipWhitespace();
        if (pos_ >= text_.size() || text_[pos_] != expected)
            return false;
        ++pos_;
        return true;
    }

    bool parseValue(JsonValue& out, std::string& error)
    {
        skipWhitespace();
        if (pos_ >= text_.size())
        {
            error = "unexpected end of input";
            return false;
        }

        const char ch = text_[pos_];
        if (ch == '"')
            return parseStringValue(out, error);
        if (ch == '{')
            return parseObject(out, error);
        if (ch == '[')
            return parseArray(out, error);
        if (ch == '-' || (ch >= '0' && ch <= '9'))
            return parseNumber(out, error);
        if (matchLiteral("true"))
        {
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (matchLiteral("false"))
        {
            out.type = JsonValue::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (matchLiteral("null"))
        {
            out.type = JsonValue::Type::Null;
            return true;
        }

        error = "unexpected token at offset " + std::to_string(pos_);
        return false;
    }

    bool matchLiteral(std::string_view literal)
    {
        if (text_.substr(pos_, literal.size()) != literal)
            return false;
        pos_ += literal.size();
        return true;
    }

    bool parseString(std::string& out, std::string& error)
    {
        if (pos_ >= text_.size() || text_[pos_] != '"')
        {
            error = "expected string";
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < text_.size())
        {
            const char ch = text_[pos_++];
            if (ch == '"')
                return true;
            if (ch != '\\')
            {
                out.push_back(ch);
                continue;
            }
            if (pos_ >= text_.size())
            {
                error = "unterminated escape";
                return false;
            }
            const char esc = text_[pos_++];
            switch (esc)
            {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default:
                error = "unsupported string escape";
                return false;
            }
        }
        error = "unterminated string";
        return false;
    }

    bool parseStringValue(JsonValue& out, std::string& error)
    {
        out.type = JsonValue::Type::String;
        return parseString(out.string, error);
    }

    bool parseNumber(JsonValue& out, std::string& error)
    {
        const size_t start = pos_;
        if (text_[pos_] == '-')
            ++pos_;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
            ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.')
        {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E'))
        {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
        }

        const std::string numberText(text_.substr(start, pos_ - start));
        char* end = nullptr;
        out.number = std::strtod(numberText.c_str(), &end);
        if (!end || *end != '\0')
        {
            error = "invalid number";
            return false;
        }
        out.type = JsonValue::Type::Number;
        return true;
    }

    bool parseArray(JsonValue& out, std::string& error)
    {
        if (!consume('['))
        {
            error = "expected array";
            return false;
        }
        out.type = JsonValue::Type::Array;
        skipWhitespace();
        if (consume(']'))
            return true;

        for (;;)
        {
            JsonValue value;
            if (!parseValue(value, error))
                return false;
            out.array.push_back(std::move(value));
            skipWhitespace();
            if (consume(']'))
                return true;
            if (!consume(','))
            {
                error = "expected comma in array";
                return false;
            }
        }
    }

    bool parseObject(JsonValue& out, std::string& error)
    {
        if (!consume('{'))
        {
            error = "expected object";
            return false;
        }
        out.type = JsonValue::Type::Object;
        skipWhitespace();
        if (consume('}'))
            return true;

        for (;;)
        {
            std::string key;
            if (!parseString(key, error))
                return false;
            if (!consume(':'))
            {
                error = "expected colon in object";
                return false;
            }
            JsonValue value;
            if (!parseValue(value, error))
                return false;
            out.object[std::move(key)] = std::move(value);
            skipWhitespace();
            if (consume('}'))
                return true;
            if (!consume(','))
            {
                error = "expected comma in object";
                return false;
            }
        }
    }

    std::string_view text_;
    size_t pos_ = 0;
};

std::string ToLower(std::string value)
{
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

const JsonValue* Find(const JsonValue& object, const char* key)
{
    if (object.type != JsonValue::Type::Object)
        return nullptr;
    const auto it = object.object.find(key);
    return it == object.object.end() ? nullptr : &it->second;
}

std::string StringOr(const JsonValue& object, const char* key, std::string fallback)
{
    const JsonValue* value = Find(object, key);
    return value && value->type == JsonValue::Type::String ? value->string : fallback;
}

float FloatOr(const JsonValue& object, const char* key, float fallback)
{
    const JsonValue* value = Find(object, key);
    return value && value->type == JsonValue::Type::Number ? static_cast<float>(value->number) : fallback;
}

int IntOr(const JsonValue& object, const char* key, int fallback)
{
    const JsonValue* value = Find(object, key);
    return value && value->type == JsonValue::Type::Number ? static_cast<int>(value->number) : fallback;
}

std::uint32_t UIntOr(const JsonValue& object, const char* key, std::uint32_t fallback)
{
    const JsonValue* value = Find(object, key);
    return value && value->type == JsonValue::Type::Number ? static_cast<std::uint32_t>(std::max(0.0, value->number)) : fallback;
}

bool BoolOr(const JsonValue& object, const char* key, bool fallback)
{
    const JsonValue* value = Find(object, key);
    return value && value->type == JsonValue::Type::Bool ? value->boolean : fallback;
}

Vec2 Vec2Or(const JsonValue& object, const char* key, Vec2 fallback)
{
    const JsonValue* value = Find(object, key);
    if (!value || value->type != JsonValue::Type::Object)
        return fallback;
    return Vec2{FloatOr(*value, "x", fallback.x), FloatOr(*value, "y", fallback.y)};
}

Vec3 Vec3Or(const JsonValue& object, const char* key, Vec3 fallback)
{
    const JsonValue* value = Find(object, key);
    if (!value || value->type != JsonValue::Type::Object)
        return fallback;
    return Vec3{FloatOr(*value, "x", fallback.x), FloatOr(*value, "y", fallback.y), FloatOr(*value, "z", fallback.z)};
}

template <typename T>
T EnumOr(const JsonValue& object, const char* key, T fallback, std::initializer_list<std::pair<const char*, T>> values)
{
    const JsonValue* value = Find(object, key);
    if (!value || value->type != JsonValue::Type::String)
        return fallback;

    const std::string text = ToLower(value->string);
    for (const auto& pair : values)
    {
        if (text == ToLower(pair.first))
            return pair.second;
    }
    return fallback;
}

std::uint32_t ColorOr(const JsonValue& object, const char* key, std::uint32_t fallback)
{
    const JsonValue* value = Find(object, key);
    if (!value)
        return fallback;
    if (value->type == JsonValue::Type::Number)
        return static_cast<std::uint32_t>(std::max(0.0, value->number));
    if (value->type != JsonValue::Type::String)
        return fallback;

    std::string text = value->string;
    int base = 10;
    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0)
    {
        text = text.substr(2);
        base = 16;
    }
    try
    {
        return static_cast<std::uint32_t>(std::stoul(text, nullptr, base));
    }
    catch (...)
    {
        return fallback;
    }
}

template <typename T, size_t N>
void ReadNumericSeries(const JsonValue& parent, const char* key, std::array<T, N>& values)
{
    const JsonValue* source = Find(parent, key);
    if (!source)
        return;

    if (source->type == JsonValue::Type::Array)
    {
        const size_t count = std::min(N, source->array.size());
        for (size_t i = 0; i < count; ++i)
        {
            if (source->array[i].type == JsonValue::Type::Number)
                values[i] = static_cast<T>(source->array[i].number);
        }
        return;
    }

    if (source->type == JsonValue::Type::Object)
    {
        for (const auto& [indexText, value] : source->object)
        {
            if (value.type != JsonValue::Type::Number)
                continue;
            int index = -1;
            const auto* begin = indexText.data();
            const auto* end = begin + indexText.size();
            if (std::from_chars(begin, end, index).ec != std::errc{} || index < 0 || index >= static_cast<int>(N))
                continue;
            values[static_cast<size_t>(index)] = static_cast<T>(value.number);
        }
    }
}

std::string PresetNameFromPath(const std::filesystem::path& path)
{
    std::string stem = path.stem().string();
    bool capitalizeNext = true;
    for (char& ch : stem)
    {
        if (ch == '_' || ch == '-')
        {
            ch = ' ';
            capitalizeNext = true;
        }
        else if (capitalizeNext)
        {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            capitalizeNext = false;
        }
    }
    return stem;
}

void ApplyBark(const JsonValue& root, TreeOptions& options)
{
    const JsonValue* bark = Find(root, "bark");
    if (!bark || bark->type != JsonValue::Type::Object)
        return;
    options.bark.type = EnumOr(*bark, "type", options.bark.type, {
        {"Oak", BarkType::Oak}, {"Bark001", BarkType::Oak},
        {"Birch", BarkType::Birch}, {"Bark002", BarkType::Birch},
        {"Pine", BarkType::Pine}, {"Bark_Pine", BarkType::Pine},
        {"Willow", BarkType::Willow}, {"Ash", BarkType::Ash}
    });
    options.bark.tint = ColorOr(*bark, "tint", options.bark.tint);
    options.bark.flatShading = BoolOr(*bark, "flatShading", options.bark.flatShading);
    options.bark.textured = BoolOr(*bark, "textured", options.bark.textured);
    options.bark.textureScale = Vec2Or(*bark, "textureScale", options.bark.textureScale);
}

void ApplyBranch(const JsonValue& root, TreeOptions& options)
{
    const JsonValue* branch = Find(root, "branch");
    if (!branch || branch->type != JsonValue::Type::Object)
        return;
    options.branch.levels = std::clamp(IntOr(*branch, "levels", options.branch.levels), 0, kMaxBranchLevels - 1);
    ReadNumericSeries(*branch, "angle", options.branch.angle);
    ReadNumericSeries(*branch, "children", options.branch.children);
    ReadNumericSeries(*branch, "gnarliness", options.branch.gnarliness);
    ReadNumericSeries(*branch, "length", options.branch.length);
    ReadNumericSeries(*branch, "radius", options.branch.radius);
    ReadNumericSeries(*branch, "sections", options.branch.sections);
    ReadNumericSeries(*branch, "segments", options.branch.segments);
    ReadNumericSeries(*branch, "start", options.branch.start);
    ReadNumericSeries(*branch, "taper", options.branch.taper);
    ReadNumericSeries(*branch, "twist", options.branch.twist);

    if (const JsonValue* force = Find(*branch, "force"); force && force->type == JsonValue::Type::Object)
    {
        options.branch.forceDirection = Vec3Or(*force, "direction", options.branch.forceDirection);
        options.branch.forceStrength = FloatOr(*force, "strength", options.branch.forceStrength);
    }
    else
    {
        options.branch.forceDirection = Vec3Or(*branch, "forceDirection", options.branch.forceDirection);
        options.branch.forceStrength = FloatOr(*branch, "forceStrength", options.branch.forceStrength);
    }
}

void ApplyLeaves(const JsonValue& root, TreeOptions& options)
{
    const JsonValue* leaves = Find(root, "leaves");
    if (!leaves || leaves->type != JsonValue::Type::Object)
        return;
    options.leaves.type = EnumOr(*leaves, "type", options.leaves.type, {
        {"Oak", LeafType::Oak}, {"leaf_oak", LeafType::Oak}, {"oak", LeafType::Oak},
        {"Ash", LeafType::Ash}, {"leaf_ash", LeafType::Ash}, {"ash", LeafType::Ash},
        {"Pine", LeafType::Pine}, {"leaf_pine", LeafType::Pine}, {"pine", LeafType::Pine},
        {"Willow", LeafType::Willow}, {"leaf_willow", LeafType::Willow}, {"willow", LeafType::Willow},
        {"Birch", LeafType::Birch}, {"leaf_birch", LeafType::Birch}, {"aspen", LeafType::Birch}
    });
    if (const JsonValue* billboard = Find(*leaves, "billboard"); billboard && billboard->type == JsonValue::Type::String)
    {
        const std::string mode = ToLower(billboard->string);
        if (mode == "single")
            options.leaves.cardsPerCluster = 1;
        else if (mode == "double")
            options.leaves.cardsPerCluster = 2;
    }
    options.leaves.cardsPerCluster = std::clamp(IntOr(*leaves, "cardsPerCluster", options.leaves.cardsPerCluster), 1, 7);
    options.leaves.atlasGridX = std::clamp(IntOr(*leaves, "atlasGridX", options.leaves.atlasGridX), 1, 8);
    options.leaves.atlasGridY = std::clamp(IntOr(*leaves, "atlasGridY", options.leaves.atlasGridY), 1, 8);
    options.leaves.angle = FloatOr(*leaves, "angle", options.leaves.angle);
    options.leaves.count = IntOr(*leaves, "count", options.leaves.count);
    options.leaves.start = FloatOr(*leaves, "start", options.leaves.start);
    options.leaves.size = FloatOr(*leaves, "size", options.leaves.size);
    options.leaves.sizeVariance = FloatOr(*leaves, "sizeVariance", options.leaves.sizeVariance);
    options.leaves.tint = ColorOr(*leaves, "tint", options.leaves.tint);
    options.leaves.alphaTest = FloatOr(*leaves, "alphaTest", options.leaves.alphaTest);
}
}

std::optional<Preset> loadPresetFile(const std::filesystem::path& path, std::string* errorOut)
{
    std::ifstream in(path);
    if (!in)
    {
        if (errorOut)
            *errorOut = path.filename().string() + " - file not found";
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();

    const std::string jsonText = buffer.str();
    JsonValue root;
    std::string error;
    JsonParser parser(jsonText);
    if (!parser.parse(root, error))
    {
        if (errorOut)
            *errorOut = path.filename().string() + " - parse error: " + error;
        return std::nullopt;
    }

    if (root.type != JsonValue::Type::Object)
    {
        if (errorOut)
            *errorOut = path.filename().string() + " - top-level JSON value must be an object";
        return std::nullopt;
    }

    Preset preset;
    preset.name = StringOr(root, "name", PresetNameFromPath(path));
    preset.options = defaultTreeOptions();
    preset.options.seed = UIntOr(root, "seed", preset.options.seed);
    preset.options.type = EnumOr(root, "type", preset.options.type, {
        {"Deciduous", TreeType::Deciduous}, {"deciduous", TreeType::Deciduous},
        {"Evergreen", TreeType::Evergreen}, {"evergreen", TreeType::Evergreen}
    });
    ApplyBark(root, preset.options);
    ApplyBranch(root, preset.options);
    ApplyLeaves(root, preset.options);
    return preset;
}

std::vector<Preset> loadAllPresets(const std::filesystem::path& presetDir, std::vector<std::string>* errorsOut)
{
    std::vector<Preset> presets;
    std::error_code ec;
    if (!std::filesystem::exists(presetDir, ec) || !std::filesystem::is_directory(presetDir, ec))
    {
        if (errorsOut)
            errorsOut->push_back(presetDir.string() + " - preset directory not found");
        return presets;
    }

    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(presetDir, ec))
    {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json")
            continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const std::filesystem::path& file : files)
    {
        std::string error;
        if (std::optional<Preset> preset = loadPresetFile(file, &error))
            presets.push_back(std::move(*preset));
        else if (errorsOut)
            errorsOut->push_back(error);
    }
    return presets;
}

std::optional<Preset> loadPreset(const std::filesystem::path& path, std::string* error)
{
    return loadPresetFile(path, error);
}
}
