#pragma once

#include <cstdint>
#include <string>

namespace client::net {

struct CharacterListItem {
    std::uint64_t id = 0;
    std::uint8_t slot = 0;
    std::string name;
    std::uint32_t level = 0;
    std::uint16_t classId = 0;
    std::uint16_t appearance = 0;
    std::int32_t posX = 0;
    std::int32_t posY = 0;
    std::uint16_t mapId = 0;
};

} // namespace client::net
