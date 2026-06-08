#include "UIHelpers.h"

#include "IconsFontAwesome6.h"

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

void SectionHeader(const char* text)
{
    ImGui::Spacing();
    if (g_fonts.bold)
        ImGui::PushFont(g_fonts.bold);
    ImGui::TextColored(ImVec4(0.94f, 0.96f, 1.0f, 1.0f), "%s", text);
    if (g_fonts.bold)
        ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
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
