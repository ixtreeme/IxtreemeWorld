#pragma once

#include <cstdint>

enum class MapEditorTool
{
    Raise,
    Lower,
    Smooth,
    Flatten,
    Paint
};

struct MapEditorSettings
{
    MapEditorTool tool = MapEditorTool::Raise;
    float brushRadiusMeters = 5.0f;
    float brushStrength = 1.0f;
    std::uint32_t textureSlot = 4;
};

struct MapEditorCommands
{
    bool save = false;
    bool reload = false;
    bool undo = false;
};
