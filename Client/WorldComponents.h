#pragma once

#include "network/IClientHandler.h"

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
};

struct Nameplate
{
    std::string name;
    std::uint32_t level = 1;
};

struct LocalPlayerTag
{
};

} // namespace client::ecs
