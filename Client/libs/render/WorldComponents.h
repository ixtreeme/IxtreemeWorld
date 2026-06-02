#pragma once

#include "network/IClientHandler.h"

#include <array>
#include <cstdint>
#include <string>

namespace client::ecs {

struct Position
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Heading
{
    std::uint16_t angle = 0;
};

struct NetId
{
    std::uint32_t value = 0;
};

struct MoveState
{
    client::net::MoveState value = client::net::MoveState::Idle;
};

struct RenderableModel
{
    std::uint32_t modelId = 0;
    std::uint32_t mobTypeId = 0;
};

struct Nameplate
{
    std::string name;
    std::uint32_t level = 1;
};

struct Health
{
    float current = 1.0f;
    float max = 1.0f;
    float displayed = 1.0f;
};

struct LocalPlayerTag
{
};

struct MobTag
{
};

struct InterpolationSample
{
    Position position;
    Heading heading;
    double serverTimeSeconds = 0.0;
};

struct InterpolationBuffer
{
    static constexpr std::uint8_t Capacity = 4;

    std::array<InterpolationSample, Capacity> samples{};
    std::uint8_t count = 0;
    std::uint8_t next = 0;
};

} // namespace client::ecs
