#include "UiCanvas.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
float Finite(float value, float fallback) { return std::isfinite(value) ? value : fallback; }
std::string Escape(const std::string& value)
{
    std::string out;
    for (char c : value)
        switch (c) { case '&': out += "&amp;"; break; case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break; case '"': out += "&quot;"; break; case '\'': out += "&#39;"; break;
            default: out += c; }
    return out;
}
std::string Color(const float* value)
{
    std::ostringstream s;
    s << "#" << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) s << std::setw(2) << static_cast<int>(std::clamp(value[i], 0.0f, 1.0f) * 255 + 0.5f);
    return s.str();
}
std::string UniqueScriptId(const UiCanvas& canvas, std::string candidate)
{
    if (candidate.empty()) candidate = "element";
    const std::string base = candidate;
    unsigned suffix = 1;
    while (std::any_of(canvas.elements.begin(), canvas.elements.end(), [&](const auto& e) { return e.scriptId == candidate; }))
        candidate = base + "_" + std::to_string(suffix++);
    return candidate;
}
}

const char* UiElementTypeName(UiElementType type)
{
    static constexpr const char* names[] = {"Panel", "Text", "Image", "Button", "Toggle", "Slider", "Input Field", "Progress Bar"};
    return names[std::clamp(static_cast<int>(type), 0, 7)];
}
UiElementType UiElementTypeFromName(const std::string& name)
{
    for (int i = 0; i < 8; ++i) if (name == UiElementTypeName(static_cast<UiElementType>(i))) return static_cast<UiElementType>(i);
    return UiElementType::Panel;
}
UiElement* FindUiElement(UiCanvas& canvas, std::uint32_t id)
{
    const auto it = std::find_if(canvas.elements.begin(), canvas.elements.end(), [id](const auto& e) { return e.id == id; });
    return it == canvas.elements.end() ? nullptr : &*it;
}
const UiElement* FindUiElement(const UiCanvas& canvas, std::uint32_t id)
{
    const auto it = std::find_if(canvas.elements.begin(), canvas.elements.end(), [id](const auto& e) { return e.id == id; });
    return it == canvas.elements.end() ? nullptr : &*it;
}

void SanitizeUiCanvas(UiCanvas& canvas)
{
    for (float& v : canvas.referenceResolution) v = std::clamp(Finite(v, 1080), 1.0f, 16384.0f);
    canvas.matchWidthOrHeight = std::clamp(Finite(canvas.matchWidthOrHeight, 0.5f), 0.0f, 1.0f);
    canvas.scaleMode = canvas.scaleMode == UiCanvas::ScaleMode::ConstantPixels ? canvas.scaleMode : UiCanvas::ScaleMode::ScaleWithScreenSize;
    if (canvas.elements.size() > 4096) canvas.elements.resize(4096);
    std::unordered_set<std::uint32_t> ids;
    std::unordered_set<std::string> scriptIds;
    std::uint32_t next = 1;
    for (auto& e : canvas.elements)
    {
        if (!e.id || ids.contains(e.id)) { while (ids.contains(next)) ++next; e.id = next++; }
        ids.insert(e.id);
        std::string safeId;
        for (unsigned char c : e.scriptId)
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') safeId += c;
        if (safeId.empty()) safeId = "ui_" + std::to_string(e.id);
        if (safeId.ends_with("__control") || safeId.ends_with("__fill") || safeId.ends_with("__label")) safeId += "_element";
        const auto base = safeId;
        for (unsigned n = 1; scriptIds.contains(safeId); ++n) safeId = base + "_" + std::to_string(n);
        e.scriptId = safeId; scriptIds.insert(safeId);
        e.type = static_cast<UiElementType>(std::clamp(static_cast<int>(e.type), 0, 7));
        for (int a = 0; a < 2; ++a)
        {
            e.rect.anchorMin[a] = std::clamp(Finite(e.rect.anchorMin[a], 0.5f), 0.0f, 1.0f);
            e.rect.anchorMax[a] = std::clamp(Finite(e.rect.anchorMax[a], e.rect.anchorMin[a]), e.rect.anchorMin[a], 1.0f);
            e.rect.pivot[a] = std::clamp(Finite(e.rect.pivot[a], 0.5f), 0.0f, 1.0f);
            e.rect.position[a] = std::clamp(Finite(e.rect.position[a], 0), -100000.0f, 100000.0f);
            e.rect.sizeDelta[a] = std::clamp(Finite(e.rect.sizeDelta[a], 50), -100000.0f, 100000.0f);
        }
        for (float& c : e.color) c = std::clamp(Finite(c, 1), 0.0f, 1.0f);
        for (float& c : e.textColor) c = std::clamp(Finite(c, 1), 0.0f, 1.0f);
        for (float& c : e.imageTint) c = std::clamp(Finite(c, 1), 0.0f, 1.0f);
        for (float& c : e.fillColor) c = std::clamp(Finite(c, 1), 0.0f, 1.0f);
        for (float& c : e.handleColor) c = std::clamp(Finite(c, 1), 0.0f, 1.0f);
        e.sliderTrackHeight = std::clamp(Finite(e.sliderTrackHeight, 12), 1.0f, 1024.0f);
        for (float& v : e.sliderHandleSize) v = std::clamp(Finite(v, 24), 1.0f, 1024.0f);
        e.fontSize = std::clamp(Finite(e.fontSize, 24), 1.0f, 512.0f);
        e.cornerRadius = std::clamp(Finite(e.cornerRadius, 0), 0.0f, 1000.0f);
        e.value = std::clamp(Finite(e.value, 0.5f), 0.0f, 1.0f);
        e.textAlignment = std::clamp(e.textAlignment, 0, 2);
    }
    for (auto& e : canvas.elements)
    {
        std::unordered_set<std::uint32_t> ancestors{e.id};
        const auto* parent = FindUiElement(canvas, e.parentId);
        if (e.parentId && !parent) e.parentId = 0;
        while (parent)
        {
            if (!ancestors.insert(parent->id).second) { e.parentId = 0; break; }
            parent = FindUiElement(canvas, parent->parentId);
        }
    }
}

float UiCanvasScale(const UiCanvas& canvas, float width, float height)
{
    if (canvas.scaleMode == UiCanvas::ScaleMode::ConstantPixels) return 1;
    const float x = std::max(width, 1.0f) / std::max(canvas.referenceResolution[0], 1.0f);
    const float y = std::max(height, 1.0f) / std::max(canvas.referenceResolution[1], 1.0f);
    return std::pow(x, 1 - canvas.matchWidthOrHeight) * std::pow(y, canvas.matchWidthOrHeight);
}

std::vector<UiLayoutRect> BuildUiCanvasLayout(const UiCanvas& canvas, float width, float height)
{
    const float scale = UiCanvasScale(canvas, width, height);
    std::vector<UiLayoutRect> result(canvas.elements.size());
    std::unordered_map<std::uint32_t, std::size_t> indices;
    for (std::size_t i = 0; i < canvas.elements.size(); ++i) indices.emplace(canvas.elements[i].id, i);
    std::vector<int> state(result.size());
    const UiLayoutRect root{0, 0, 0, width, height, canvas.enabled};
    const auto visit = [&](auto&& self, std::size_t i) -> void {
        if (state[i] == 2) return;
        state[i] = 1;
        const auto& e = canvas.elements[i];
        UiLayoutRect parent = root;
        if (const auto it = indices.find(e.parentId); it != indices.end() && state[it->second] != 1)
        { self(self, it->second); parent = result[it->second]; }
        const float w = std::max(0.0f, parent.width * (e.rect.anchorMax[0] - e.rect.anchorMin[0]) + e.rect.sizeDelta[0] * scale);
        const float h = std::max(0.0f, parent.height * (e.rect.anchorMax[1] - e.rect.anchorMin[1]) + e.rect.sizeDelta[1] * scale);
        result[i] = {e.id,
            parent.x + parent.width * (e.rect.anchorMin[0] + (e.rect.anchorMax[0] - e.rect.anchorMin[0]) * e.rect.pivot[0]) + e.rect.position[0] * scale - w * e.rect.pivot[0],
            parent.y + parent.height * (e.rect.anchorMin[1] + (e.rect.anchorMax[1] - e.rect.anchorMin[1]) * e.rect.pivot[1]) + e.rect.position[1] * scale - h * e.rect.pivot[1],
            w, h, parent.visible && e.enabled};
        state[i] = 2;
    };
    for (std::size_t i = 0; i < result.size(); ++i) visit(visit, i);
    return result;
}

std::uint32_t HitTestUiCanvas(const UiCanvas& canvas, const std::vector<UiLayoutRect>& layout, float x, float y, float width, float height)
{
    if (x < 0 || y < 0 || x > width || y > height || layout.size() != canvas.elements.size()) return 0;
    const auto hit = [&](auto&& self, std::uint32_t parent, unsigned depth) -> std::uint32_t {
        if (depth > canvas.elements.size()) return 0;
        for (std::size_t n = canvas.elements.size(); n-- > 0;)
        {
            const auto& e = canvas.elements[n]; const auto& r = layout[n];
            if (e.parentId != parent || !r.visible) continue;
            if ((!e.clipChildren || r.Contains(x,y)))
                if (const auto id = self(self, e.id, depth + 1)) return id;
            if (r.Contains(x,y)) return e.id;
        }
        return 0;
    };
    return hit(hit, 0, 0);
}

void SetUiRectFromBounds(UiRectTransform& t, const UiLayoutRect& b, const UiLayoutRect& p, float scale)
{
    const float sizes[] = {p.width, p.height}, desired[] = {b.width, b.height};
    const float offset[] = {b.x - p.x, b.y - p.y};
    for (int a = 0; a < 2; ++a)
    {
        t.sizeDelta[a] = (desired[a] - sizes[a] * (t.anchorMax[a] - t.anchorMin[a])) / scale;
        t.position[a] = (offset[a] + desired[a] * t.pivot[a] - sizes[a] *
            (t.anchorMin[a] + (t.anchorMax[a] - t.anchorMin[a]) * t.pivot[a])) / scale;
    }
}

UiElement& AddUiElement(UiCanvas& canvas, UiElementType type, std::uint32_t parent)
{
    UiElement e;
    for (const auto& node : canvas.elements) e.id = std::max(e.id, node.id);
    ++e.id; e.parentId = FindUiElement(canvas, parent) ? parent : 0; e.type = type;
    e.name = UiElementTypeName(type); e.scriptId = UniqueScriptId(canvas, "ui_" + std::to_string(e.id));
    e.text = type == UiElementType::Text ? "New text" : (type == UiElementType::InputField ? "" : e.name);
    e.raycastTarget = type == UiElementType::Button || type == UiElementType::Toggle || type == UiElementType::Slider || type == UiElementType::InputField;
    if (type == UiElementType::Panel) { e.rect.sizeDelta[0] = 400; e.rect.sizeDelta[1] = 250; }
    if (type == UiElementType::Image) { e.rect.sizeDelta[0] = e.rect.sizeDelta[1] = 128; std::fill_n(e.color, 4, 1.0f); }
    if (type == UiElementType::Toggle) e.rect.sizeDelta[0] = e.rect.sizeDelta[1] = 28;
    if (type == UiElementType::ProgressBar) e.rect.sizeDelta[1] = 24;
    if (type == UiElementType::Button) { e.color[0] = 0.15f; e.color[1] = 0.4f; e.color[2] = 0.65f; }
    if (type == UiElementType::Text) e.color[3] = 0;
    canvas.elements.push_back(std::move(e)); return canvas.elements.back();
}

bool ReparentUiElement(UiCanvas& canvas, std::uint32_t id, std::uint32_t parentId, float width, float height)
{
    auto* element = FindUiElement(canvas, id);
    if (!element || id == parentId || (parentId && !FindUiElement(canvas, parentId))) return false;
    std::unordered_set<std::uint32_t> seen{id};
    for (const auto* p = FindUiElement(canvas, parentId); p; p = FindUiElement(canvas, p->parentId)) if (!seen.insert(p->id).second) return false;
    const auto layout = BuildUiCanvasLayout(canvas, width, height);
    UiLayoutRect bounds, parent{0, 0, 0, width, height};
    for (const auto& r : layout) { if (r.id == id) bounds = r; if (parentId && r.id == parentId) parent = r; }
    element->parentId = parentId;
    SetUiRectFromBounds(element->rect, bounds, parent, UiCanvasScale(canvas, width, height));
    return true;
}
void DeleteUiElement(UiCanvas& canvas, std::uint32_t id)
{
    std::unordered_set<std::uint32_t> removed{id};
    bool changed;
    do { changed = false; for (const auto& e : canvas.elements) if (removed.contains(e.parentId)) changed |= removed.insert(e.id).second; } while (changed);
    std::erase_if(canvas.elements, [&](const auto& e) { return removed.contains(e.id); });
}
std::uint32_t DuplicateUiElement(UiCanvas& canvas, std::uint32_t id)
{
    if (!FindUiElement(canvas, id)) return 0;
    std::unordered_set<std::uint32_t> copied{id};
    bool changed;
    do { changed = false; for (const auto& e : canvas.elements) if (copied.contains(e.parentId)) changed |= copied.insert(e.id).second; } while (changed);
    std::uint32_t next = 0; for (const auto& e : canvas.elements) next = std::max(next, e.id);
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    for (const auto& e : canvas.elements) if (copied.contains(e.id)) remap[e.id] = ++next;
    const auto original = canvas.elements;
    for (auto e : original) if (copied.contains(e.id))
    {
        const bool root = e.id == id; e.id = remap[e.id];
        if (remap.contains(e.parentId)) e.parentId = remap[e.parentId];
        e.scriptId = UniqueScriptId(canvas, e.scriptId + "_copy");
        if (root) { e.name += " Copy"; e.rect.position[0] += 20; e.rect.position[1] += 20; }
        canvas.elements.push_back(std::move(e));
    }
    return remap[id];
}

UiCanvas MakeUiHudCanvas(std::uint32_t id, std::string name)
{
    UiCanvas c; c.id = id; c.name = std::move(name);
    const auto topLeft = [](UiElement& e, float x, float y, float w, float h) {
        e.rect.anchorMin[0] = e.rect.anchorMin[1] = e.rect.anchorMax[0] = e.rect.anchorMax[1] = 0;
        e.rect.pivot[0] = e.rect.pivot[1] = 0; e.rect.position[0] = x; e.rect.position[1] = y;
        e.rect.sizeDelta[0] = w; e.rect.sizeDelta[1] = h;
    };
    auto& panel = AddUiElement(c, UiElementType::Panel); topLeft(panel,32,32,320,140); panel.name = "Player HUD"; panel.scriptId = "player_panel";
    panel.color[0] = .045f; panel.color[1] = .06f; panel.color[2] = .085f; panel.color[3] = .93f; panel.cornerRadius = 12;
    const auto parent = panel.id;
    auto& title = AddUiElement(c, UiElementType::Text, parent); topLeft(title,20,14,280,36); title.name = "Player name";
    title.scriptId = "player_name"; title.text = "PLAYER"; title.fontSize = 26; title.textAlignment = 0;
    auto& bar = AddUiElement(c, UiElementType::ProgressBar, parent); topLeft(bar,20,64,280,16); bar.name = "Health bar"; bar.scriptId = "health_bar"; bar.value = .8f;
    auto& value = AddUiElement(c, UiElementType::Text, parent); topLeft(value,20,90,280,28); value.name = "Health label";
    value.scriptId = "health_text"; value.text = "Health: 80 / 100"; value.fontSize = 18; value.textAlignment = 0;
    auto& button = AddUiElement(c, UiElementType::Button); button.name = "Pause button"; button.scriptId = "pause_button"; button.text = "Pause";
    button.rect.anchorMin[0] = button.rect.anchorMax[0] = button.rect.anchorMin[1] = button.rect.anchorMax[1] = 1;
    button.rect.pivot[0] = button.rect.pivot[1] = 1; button.rect.position[0] = button.rect.position[1] = -32;
    button.rect.sizeDelta[0] = 180; button.rect.sizeDelta[1] = 48;
    return c;
}

const std::string& UiElementPreviewImage(const UiElement& e, int state)
{
    if ((!e.interactable || state == 3) && !e.disabledImagePath.empty()) return e.disabledImagePath;
    if (e.interactable && state != 3)
    {
        if (state == 2 && !e.pressedImagePath.empty()) return e.pressedImagePath;
        if ((state == 1 || state == 2) && !e.hoverImagePath.empty()) return e.hoverImagePath;
    }
    if (e.type == UiElementType::Toggle && e.checked && !e.checkedImagePath.empty()) return e.checkedImagePath;
    return e.imagePath;
}

UiSliderLayout BuildUiSliderLayout(const UiElement& e, const UiLayoutRect& r, float scale)
{
    const float th = std::min(r.height, e.sliderTrackHeight * scale);
    const float hw = std::min(r.width, e.sliderHandleSize[0] * scale), hh = std::min(r.height, e.sliderHandleSize[1] * scale);
    const float offset = std::round((r.width - hw) * std::clamp(e.value, 0.0f, 1.0f));
    return {{0,r.x,r.y+(r.height-th)*.5f,r.width,th}, {0,r.x,r.y+(r.height-th)*.5f,offset,th},
        {0,r.x+offset,r.y+(r.height-hh)*.5f,hw,hh}};
}

namespace {
std::string ImageColor(const UiElement& e)
{
    float tint[4];
    for (int i=0;i<4;++i) tint[i]=e.imageTint[i]*(e.type==UiElementType::Image ? e.color[i] : 1.0f);
    return Color(tint);
}
std::string CssString(const std::string& value)
{
    // RCSS quoted tokens; protect the inline stylesheet delimiter as well.
    std::string result="\"";
    for (unsigned char c : value)
    {
        if (c=='\\' || c=='\"') { result+='\\'; result+=c; }
        else if (c=='<' || c=='>' || c<32) { std::ostringstream escaped; escaped << '\\' << std::hex << static_cast<int>(c) << ' '; result+=escaped.str(); }
        else result+=c;
    }
    return result+'\"';
}
std::string Skin(const std::string& path, const UiElement& e, const std::function<std::string(const std::string&)>& resolve)
{
    if (path.empty()) return {};
    return "background-color:transparent;image-color:"+ImageColor(e)+";decorator:image("+CssString(resolve ? resolve(path) : path)+");";
}
}

std::string UiElementStyle(const UiCanvas&, const UiElement& e, const std::vector<UiLayoutRect>& layout, float scale)
{
    UiLayoutRect rect, parent;
    for (const auto& r : layout) { if (r.id == e.id) rect = r; if (e.parentId && r.id == e.parentId) parent = r; }
    std::ostringstream s; s.imbue(std::locale::classic()); s << std::fixed << std::setprecision(3);
    s << "position:absolute;box-sizing:border-box;left:" << rect.x - parent.x << "px;top:" << rect.y - parent.y
      << "px;width:" << rect.width << "px;height:" << rect.height << "px;display:" << (e.enabled ? "block" : "none")
      << ";pointer-events:" << (e.raycastTarget && e.interactable ? "auto" : "none")
      << ";image-color:" << ImageColor(e) << ";color:" << Color(e.textColor) << ";font-size:" << e.fontSize * scale
      << "px;border-radius:" << e.cornerRadius * scale << "px;overflow:" << (e.clipChildren ? "hidden" : "visible")
      << ";text-align:" << (e.textAlignment == 0 ? "left" : e.textAlignment == 2 ? "right" : "center") << ";";
    return s.str();
}

std::string BuildUiCanvasRml(const UiCanvas& canvas, float width, float height,
    const std::function<std::string(const std::string&)>& resolveImage)
{
    const auto layout = BuildUiCanvasLayout(canvas, width, height);
    const float scale = UiCanvasScale(canvas, width, height);
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::fixed << std::setprecision(3);
    out << "<rml><head><title>" << Escape(canvas.name) << "</title><style>body{position:absolute;left:0;top:0;width:100%;height:100%;margin:0;"
        "background-color:transparent;pointer-events:none;font-family:Roboto;} button:hover{opacity:0.85;} button:active{opacity:0.65;}"
        " input{padding:0;border-width:0;} input sliderarrowdec,input sliderarrowinc{display:none;}"
        " .ix-fill{position:absolute;left:0;top:0;height:100%;pointer-events:none;}";
    for (std::size_t i=0;i<canvas.elements.size();++i)
    {
        const auto& e=canvas.elements[i]; const auto& r=layout[i];
        const bool input=e.type==UiElementType::Toggle || e.type==UiElementType::Slider || e.type==UiElementType::InputField;
        const std::string selector="#"+e.scriptId+(input ? "__control" : "");
        if (e.type==UiElementType::Slider)
        {
            const auto parts=BuildUiSliderLayout(e,r,scale);
            out << selector << "{background-color:transparent;}" << selector << " slidertrack{height:" << parts.track.height
                << "px;margin-top:" << parts.track.y-r.y << "px;background-color:" << Color(e.color)
                << ";border-radius:" << e.cornerRadius*scale << "px;" << Skin(e.imagePath,e,resolveImage) << "}"
                << selector << " sliderprogress{height:auto;pointer-events:none;background-color:" << Color(e.fillColor)
                << ';' << Skin(e.fillImagePath,e,resolveImage) << "}"
                << selector << " sliderbar{width:" << parts.handle.width << "px;height:" << parts.handle.height
                << "px;margin-top:" << parts.handle.y-r.y << "px;background-color:" << Color(e.handleColor)
                << ";border-radius:" << e.cornerRadius*scale << "px;" << Skin(e.handleImagePath,e,resolveImage) << "}";
            if (!e.hoverImagePath.empty()) out << selector << " sliderbar:hover{" << Skin(e.hoverImagePath,e,resolveImage) << "}";
            if (!e.pressedImagePath.empty()) out << selector << " sliderbar:active{" << Skin(e.pressedImagePath,e,resolveImage) << "}";
            if (!e.disabledImagePath.empty()) out << selector << ":disabled sliderbar{" << Skin(e.disabledImagePath,e,resolveImage) << "}";
        }
        else
        {
            out << selector << "{background-color:" << (e.type==UiElementType::Image ? "transparent" : Color(e.color))
                << ';' << Skin(e.imagePath,e,resolveImage) << "}";
            if (e.type==UiElementType::Toggle)
                out << selector << ":checked{background-color:" << Color(e.fillColor) << ';' << Skin(e.checkedImagePath,e,resolveImage) << "}";
            if (!e.hoverImagePath.empty()) out << selector << ":hover{opacity:1;" << Skin(e.hoverImagePath,e,resolveImage) << "}";
            if (!e.pressedImagePath.empty()) out << selector << ":active{opacity:1;" << Skin(e.pressedImagePath,e,resolveImage) << "}";
            if (!e.disabledImagePath.empty()) out << selector << (input ? ":disabled" : ".ix-disabled") << "{opacity:1;" << Skin(e.disabledImagePath,e,resolveImage) << "}";
        }
        if (e.type==UiElementType::ProgressBar)
            out << '#' << e.scriptId << "__fill{background-color:" << Color(e.fillColor) << ';' << Skin(e.fillImagePath,e,resolveImage) << "}";
    }
    out << "</style></head><body>";
    const auto write = [&](auto&& self, std::uint32_t parent, unsigned depth) -> void {
        if (depth > canvas.elements.size()) return;
        for (const auto& e : canvas.elements) if (e.parentId == parent)
        {
            const bool input = e.type == UiElementType::Toggle || e.type == UiElementType::Slider || e.type == UiElementType::InputField;
            const char* tag = e.type == UiElementType::Button ? "button" : "div";
            out << '<' << tag << " id=\"" << Escape(e.scriptId) << "\" style=\"" << UiElementStyle(canvas, e, layout, scale) << "\"";
            if (!e.interactable) out << " class=\"ix-disabled\"";
            if (e.type == UiElementType::ProgressBar) out << " value=\"" << e.value << "\"";
            out << ">";
            if (input)
            {
                out << "<input id=\"" << Escape(e.scriptId) << "__control\" style=\"width:100%;height:100%;box-sizing:border-box;"
                    << "pointer-events:" << (e.raycastTarget && e.interactable ? "auto" : "none") << ";\"";
                if (!e.interactable) out << " disabled=\"disabled\"";
                out << " type=\"" << (e.type == UiElementType::Toggle ? "checkbox" : e.type == UiElementType::Slider ? "range" : "text") << "\"";
                if (e.type == UiElementType::Toggle && e.checked) out << " checked=\"checked\"";
                if (e.type == UiElementType::Slider) out << " min=\"0\" max=\"1\" step=\"0.01\" value=\"" << e.value << "\"";
                if (e.type == UiElementType::InputField) out << " value=\"" << Escape(e.text) << "\"";
                out << "/>";
            }
            else
            {
                if (e.type == UiElementType::ProgressBar) out << "<div id=\"" << Escape(e.scriptId) << "__fill\" class=\"ix-fill\" style=\"width:" << e.value * 100 << "%\"/>";
                else if (e.type == UiElementType::Text || e.type == UiElementType::Button)
                    out << "<div style=\"position:absolute;left:0;top:0;width:100%;height:100%;overflow:hidden;"
                        "pointer-events:none;display:flex;align-items:center;justify-content:"
                        << (e.textAlignment == 0 ? "flex-start" : e.textAlignment == 2 ? "flex-end" : "center")
                        << ";\"><div id=\"" << Escape(e.scriptId) << "__label\" style=\"width:100%;min-width:0;pointer-events:none;white-space:pre-wrap;line-height:1.2;\">"
                        << Escape(e.text) << "</div></div>";
            }
            self(self, e.id, depth + 1);
            out << "</" << tag << ">";
        }
    };
    write(write, 0, 0); out << "</body></rml>"; return out.str();
}
