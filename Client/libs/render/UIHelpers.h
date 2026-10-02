#pragma once

#include <imgui.h>

#include <cstddef>
#include <string>
#include <utility>

namespace UI
{
struct EditorFonts
{
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
};

void SetEditorFonts(EditorFonts fonts);
EditorFonts GetEditorFonts();

// The editor palette. Colors are sRGB, as written everywhere in the editor; the UI backend decodes
// them for an sRGB render target.
namespace Theme
{
inline constexpr ImVec4 Accent{0.24f, 0.49f, 0.86f, 1.0f};
inline constexpr ImVec4 AccentHovered{0.31f, 0.56f, 0.92f, 1.0f};
inline constexpr ImVec4 AccentActive{0.20f, 0.42f, 0.76f, 1.0f};
inline constexpr ImVec4 Success{0.36f, 0.78f, 0.42f, 1.0f};
inline constexpr ImVec4 Warning{0.95f, 0.70f, 0.25f, 1.0f};
inline constexpr ImVec4 Error{0.93f, 0.36f, 0.34f, 1.0f};
inline constexpr ImVec4 Danger{0.70f, 0.22f, 0.22f, 1.0f};
inline constexpr ImVec4 TextMuted{0.56f, 0.59f, 0.64f, 1.0f};
inline constexpr ImVec4 AxisX{0.89f, 0.35f, 0.32f, 1.0f};
inline constexpr ImVec4 AxisY{0.47f, 0.78f, 0.33f, 1.0f};
inline constexpr ImVec4 AxisZ{0.32f, 0.55f, 0.94f, 1.0f};
inline constexpr ImVec4 PlayMode{0.36f, 0.78f, 0.42f, 1.0f};
inline constexpr ImVec4 PausedMode{0.95f, 0.70f, 0.25f, 1.0f};
}

bool PropertyRow(const char* label, float* value, float min = 0.0f, float max = 1.0f);
bool PropertyRow(const char* label, int* value, int min = 0, int max = 100);
bool PropertyRow(const char* label, bool* value);
bool IconButton(const char* icon, const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool IconOnlyButton(const char* icon, float size = 0.0f);
// A square icon button that stays highlighted while `active` (tool and mode switches).
bool ToggleIconButton(const char* icon, bool active, const char* tooltip, float size = 0.0f);
void SectionHeader(const char* text);
// A small "more" button at the right end of the CollapsingHeader just drawn (the header needs
// ImGuiTreeNodeFlags_AllowOverlap). Returns true when clicked.
bool HeaderMenuButton(const char* tooltip);
void ColoredText(const ImVec4& color, const char* fmt, ...);
void StatusOk(const char* text);
void StatusError(const char* text);
void StatusWarning(const char* text);
void HelpMarker(const char* text);
// A tooltip for the last item, shown on hover.
void ItemTooltip(const char* text);
void DrawImage(ImTextureID texture, const ImVec2& size, bool showBorder = true);

// Inspector-style property rows: the label sits in a column on the left and the widget fills the
// rest of the line, so every value lines up and no label is cut off by a narrow panel.
// PropertyLabel draws the label part of `label` (the text before "##") and returns the hidden id to
// give the widget, or nullptr when the widget should keep ImGui's own layout: no visible label,
// continuing a line after SameLine, or inside a table cell.
const char* PropertyLabel(const char* label, char* idBuffer, std::size_t idBufferSize);

template <typename Widget>
bool Property(const char* label, Widget&& widget)
{
    char id[256];
    const char* hidden = PropertyLabel(label, id, sizeof(id));
    return widget(hidden ? hidden : label);
}

// Drop-in replacements for the labelled ImGui widgets, laid out as property rows.
namespace Prop
{
#define IX_UI_PROPERTY_WIDGET(Name)                                                          \
    template <typename... Args>                                                              \
    bool Name(const char* label, Args&&... args)                                             \
    {                                                                                        \
        return Property(label, [&](const char* id) { return ImGui::Name(id, std::forward<Args>(args)...); }); \
    }
IX_UI_PROPERTY_WIDGET(Checkbox)
IX_UI_PROPERTY_WIDGET(DragFloat)
IX_UI_PROPERTY_WIDGET(DragFloat2)
IX_UI_PROPERTY_WIDGET(DragFloat3)
IX_UI_PROPERTY_WIDGET(DragFloat4)
IX_UI_PROPERTY_WIDGET(DragInt)
IX_UI_PROPERTY_WIDGET(DragInt2)
IX_UI_PROPERTY_WIDGET(DragScalar)
IX_UI_PROPERTY_WIDGET(SliderFloat)
IX_UI_PROPERTY_WIDGET(SliderFloat2)
IX_UI_PROPERTY_WIDGET(SliderFloat3)
IX_UI_PROPERTY_WIDGET(SliderInt)
IX_UI_PROPERTY_WIDGET(SliderAngle)
IX_UI_PROPERTY_WIDGET(InputText)
IX_UI_PROPERTY_WIDGET(InputTextWithHint)
IX_UI_PROPERTY_WIDGET(InputFloat)
IX_UI_PROPERTY_WIDGET(InputFloat2)
IX_UI_PROPERTY_WIDGET(InputFloat3)
IX_UI_PROPERTY_WIDGET(InputInt)
IX_UI_PROPERTY_WIDGET(InputScalar)
IX_UI_PROPERTY_WIDGET(ColorEdit3)
IX_UI_PROPERTY_WIDGET(ColorEdit4)
IX_UI_PROPERTY_WIDGET(Combo)
IX_UI_PROPERTY_WIDGET(BeginCombo)
#undef IX_UI_PROPERTY_WIDGET

// A read-only value in a property row.
void Text(const char* label, const char* fmt, ...);
}

// A property row showing an assigned asset (icon + name) in a field the user can drop assets on:
// call BeginDragDropTarget right after it. Returns true when the field is clicked.
bool AssetField(const char* label, const char* icon, const std::string& value, const char* tooltip);
}
