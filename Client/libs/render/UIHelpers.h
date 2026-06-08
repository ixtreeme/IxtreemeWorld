#pragma once

#include <imgui.h>

#include <string>

namespace UI
{
struct EditorFonts
{
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
};

void SetEditorFonts(EditorFonts fonts);
EditorFonts GetEditorFonts();

bool PropertyRow(const char* label, float* value, float min = 0.0f, float max = 1.0f);
bool PropertyRow(const char* label, int* value, int min = 0, int max = 100);
bool PropertyRow(const char* label, bool* value);
bool IconButton(const char* icon, const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool IconOnlyButton(const char* icon, float size = 0.0f);
void SectionHeader(const char* text);
void ColoredText(const ImVec4& color, const char* fmt, ...);
void StatusOk(const char* text);
void StatusError(const char* text);
void StatusWarning(const char* text);
void HelpMarker(const char* text);
void DrawImage(ImTextureID texture, const ImVec2& size, bool showBorder = true);
}
