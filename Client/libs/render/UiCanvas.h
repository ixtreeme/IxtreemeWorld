#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Screen-space Canvas. Positions and sizes use reference pixels, with Y pointing down.
struct UiRectTransform
{
    float anchorMin[2] = {0.5f, 0.5f};
    float anchorMax[2] = {0.5f, 0.5f};
    float pivot[2] = {0.5f, 0.5f};
    float position[2] = {};
    float sizeDelta[2] = {200, 50};
    bool operator==(const UiRectTransform&) const = default;
};

enum class UiElementType { Panel, Text, Image, Button, Toggle, Slider, InputField, ProgressBar };

struct UiElement
{
    std::uint32_t id = 0, parentId = 0;
    std::string name = "Element", scriptId;
    UiElementType type = UiElementType::Panel;
    UiRectTransform rect;
    bool enabled = true, interactable = true, raycastTarget = false, clipChildren = false;
    float color[4] = {0.12f, 0.15f, 0.20f, 0.9f};
    float textColor[4] = {1, 1, 1, 1};
    std::string text, imagePath;
    // Background (Slider: track), interaction states, and control-specific layers.
    std::string hoverImagePath, pressedImagePath, disabledImagePath, checkedImagePath;
    std::string fillImagePath, handleImagePath;
    float imageTint[4] = {1, 1, 1, 1};
    float fillColor[4] = {0.30f, 0.67f, 0.97f, 1};
    float handleColor[4] = {0.30f, 0.67f, 0.97f, 1};
    float sliderTrackHeight = 12, sliderHandleSize[2] = {24, 32};
    float fontSize = 24, cornerRadius = 4, value = 0.5f;
    int textAlignment = 1; // left, centre, right
    bool checked = false;
    bool operator==(const UiElement&) const = default;
};

struct UiCanvas
{
    enum class ScaleMode { ConstantPixels, ScaleWithScreenSize };
    std::uint32_t id = 0;
    std::string name = "Canvas";
    bool enabled = true;
    float referenceResolution[2] = {1920, 1080};
    ScaleMode scaleMode = ScaleMode::ScaleWithScreenSize;
    float matchWidthOrHeight = 0.5f;
    int sortingOrder = 0;
    std::vector<UiElement> elements;
    bool operator==(const UiCanvas&) const = default;
};

struct UiLayoutRect
{
    std::uint32_t id = 0;
    float x = 0, y = 0, width = 0, height = 0;
    bool visible = true;
    bool Contains(float px, float py) const { return px >= x && py >= y && px <= x + width && py <= y + height; }
};

struct UiSliderLayout
{
    UiLayoutRect track, fill, handle;
};
// Absolute bounds shared by the authoring preview and native range-control styling.
UiSliderLayout BuildUiSliderLayout(const UiElement& element, const UiLayoutRect& bounds, float scale);
const std::string& UiElementPreviewImage(const UiElement& element, int state);
const char* UiElementTypeName(UiElementType type);
UiCanvas MakeUiHudCanvas(std::uint32_t id, std::string name = "HUD");
UiElementType UiElementTypeFromName(const std::string& name);
void SanitizeUiCanvas(UiCanvas& canvas);
float UiCanvasScale(const UiCanvas& canvas, float width, float height);
std::vector<UiLayoutRect> BuildUiCanvasLayout(const UiCanvas& canvas, float width, float height);
std::uint32_t HitTestUiCanvas(const UiCanvas& canvas, const std::vector<UiLayoutRect>& layout, float x, float y, float width, float height);
UiElement* FindUiElement(UiCanvas& canvas, std::uint32_t id);
const UiElement* FindUiElement(const UiCanvas& canvas, std::uint32_t id);
UiElement& AddUiElement(UiCanvas& canvas, UiElementType type, std::uint32_t parent = 0);
bool ReparentUiElement(UiCanvas& canvas, std::uint32_t id, std::uint32_t parent, float width, float height);
void DeleteUiElement(UiCanvas& canvas, std::uint32_t id);
std::uint32_t DuplicateUiElement(UiCanvas& canvas, std::uint32_t id);
// Preserves the visual rectangle when an anchor preset changes.
void SetUiRectFromBounds(UiRectTransform& transform, const UiLayoutRect& bounds, const UiLayoutRect& parent, float scale);
std::string UiElementStyle(const UiCanvas& canvas, const UiElement& element,
    const std::vector<UiLayoutRect>& layout, float scale);
std::string BuildUiCanvasRml(const UiCanvas& canvas, float width, float height,
    const std::function<std::string(const std::string&)>& resolveImage = {});
