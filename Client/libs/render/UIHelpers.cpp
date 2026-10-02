#include "UIHelpers.h"

#include "IconsFontAwesome6.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cstdio>

namespace UI
{
namespace
{
EditorFonts g_fonts;
}

void SetEditorFonts(EditorFonts fonts)
{
    g_fonts = fonts;
}

EditorFonts GetEditorFonts()
{
    return g_fonts;
}

bool PropertyRow(const char* label, float* value, float min, float max)
{
    ImGui::PushID(label);
    const float fullWidth = ImGui::GetContentRegionAvail().x;
    const float labelWidth = fullWidth * 0.40f;
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + labelWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool changed = ImGui::SliderFloat("##value", value, min, max, "%.2f");
    ImGui::PopID();
    return changed;
}

bool PropertyRow(const char* label, int* value, int min, int max)
{
    ImGui::PushID(label);
    const float fullWidth = ImGui::GetContentRegionAvail().x;
    const float labelWidth = fullWidth * 0.40f;
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + labelWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool changed = ImGui::SliderInt("##value", value, min, max);
    ImGui::PopID();
    return changed;
}

bool PropertyRow(const char* label, bool* value)
{
    ImGui::PushID(label);
    const float fullWidth = ImGui::GetContentRegionAvail().x;
    const float labelWidth = fullWidth * 0.40f;
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + labelWidth);
    const bool changed = ImGui::Checkbox("##value", value);
    ImGui::PopID();
    return changed;
}

bool IconButton(const char* icon, const char* label, const ImVec2& size)
{
    char text[160]{};
    std::snprintf(text, sizeof(text), "%s  %s", icon ? icon : "", label ? label : "");
    return ImGui::Button(text, size);
}

bool IconOnlyButton(const char* icon, float size)
{
    const float buttonSize = size > 0.0f ? size : ImGui::GetFrameHeight();
    return ImGui::Button(icon ? icon : "", ImVec2(buttonSize, buttonSize));
}

bool ToggleIconButton(const char* icon, bool active, const char* tooltip, float size)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::AccentHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::AccentActive);
    }
    const bool pressed = IconOnlyButton(icon, size);
    if (active)
        ImGui::PopStyleColor(3);
    if (tooltip)
        ItemTooltip(tooltip);
    return pressed;
}

void ItemTooltip(const char* text)
{
    if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

const char* PropertyLabel(const char* label, char* idBuffer, std::size_t idBufferSize)
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    if (labelEnd == label || window->DC.IsSameLine || ImGui::GetCurrentTable() != nullptr ||
        window->DC.CurrentColumns != nullptr)
        return nullptr;

    // Keep a width the caller asked for: drawing the label would consume it.
    const bool hasWidth = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth) != 0;
    const float requestedWidth = g.NextItemData.Width;

    const float rowStartX = ImGui::GetCursorPosX();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float labelWidth = std::clamp(avail * 0.38f, 70.0f, 220.0f);
    const float textRoom = labelWidth - g.Style.ItemInnerSpacing.x;

    ImGui::AlignTextToFramePadding();
    const ImVec2 textSize = ImGui::CalcTextSize(label, labelEnd);
    if (textSize.x <= textRoom)
    {
        ImGui::TextUnformatted(label, labelEnd);
    }
    else
    {
        // Too long for the column: cut with an ellipsis, full text on hover.
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(textRoom, textSize.y);
        ImGui::Dummy(size);
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(),
            pos,
            ImVec2(pos.x + size.x, pos.y + size.y),
            pos.x + size.x,
            label,
            labelEnd,
            &textSize);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%.*s", static_cast<int>(labelEnd - label), label);
    }
    ImGui::SameLine(rowStartX + labelWidth);
    ImGui::SetNextItemWidth(hasWidth ? requestedWidth : -FLT_MIN);

    std::snprintf(idBuffer, idBufferSize, "##%s", label);
    return idBuffer;
}

bool AssetField(const char* label, const char* icon, const std::string& value, const char* tooltip)
{
    return Property(label, [&](const char*) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        const std::string text = std::string(icon ? icon : "") + "  " + value + "##asset_field";
        const bool clicked = ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0.0f));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ItemTooltip(tooltip);
        return clicked;
    });
}

namespace Prop
{
void Text(const char* label, const char* fmt, ...)
{
    char id[256];
    if (PropertyLabel(label, id, sizeof(id)) == nullptr)
    {
        ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
        ImGui::SameLine();
    }
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}
}

void SectionHeader(const char* text)
{
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    if (g_fonts.bold)
        ImGui::PushFont(g_fonts.bold);
    ImGui::TextColored(ImVec4(0.78f, 0.81f, 0.86f, 1.0f), "%s", text);
    if (g_fonts.bold)
        ImGui::PopFont();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
}

bool HeaderMenuButton(const char* tooltip)
{
    const ImVec2 headerMin = ImGui::GetItemRectMin();
    const ImVec2 headerMax = ImGui::GetItemRectMax();
    const float size = headerMax.y - headerMin.y;
    const ImVec2 restore = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(headerMax.x - size - 2.0f, headerMin.y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    const bool pressed = ImGui::Button(ICON_FA_ELLIPSIS_VERTICAL "##header_menu", ImVec2(size, size));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ItemTooltip(tooltip);
    ImGui::SetCursorScreenPos(restore);
    return pressed;
}

void ColoredText(const ImVec4& color, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::TextColoredV(color, fmt, args);
    va_end(args);
}

void StatusOk(const char* text)
{
    ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.40f, 1.0f), ICON_FA_CHECK " %s", text);
}

void StatusError(const char* text)
{
    ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.30f, 1.0f), ICON_FA_XMARK " %s", text);
}

void StatusWarning(const char* text)
{
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s", text);
}

void HelpMarker(const char* text)
{
    ImGui::TextDisabled(ICON_FA_CIRCLE_QUESTION);
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void DrawImage(ImTextureID texture, const ImVec2& size, bool showBorder)
{
    if (showBorder)
        ImGui::Image(texture, size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
            ImVec4(1.0f, 1.0f, 1.0f, 1.0f), ImVec4(0.30f, 0.30f, 0.34f, 1.0f));
    else
        ImGui::Image(texture, size);
}
}
