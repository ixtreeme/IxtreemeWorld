#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/array.h>

#include "schema/packet.capnp.h"

namespace gs::protocol {

inline std::vector<std::uint8_t> ToByteVector(kj::ArrayPtr<const kj::byte> bytes)
{
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

inline std::vector<std::uint8_t> SerializeToBytes(capnp::MessageBuilder& builder)
{
    const auto words = capnp::messageToFlatArray(builder);
    return ToByteVector(words.asBytes());
}

struct ParsedPacket {
    capnp::FlatArrayMessageReader reader;
    Packet::Reader packet;

    explicit ParsedPacket(kj::ArrayPtr<const capnp::word> words)
        : reader(words)
        , packet(reader.getRoot<Packet>())
    {
    }
};

inline std::optional<ParsedPacket> ParsePacket(const std::vector<std::uint8_t>& payload)
{
    if (payload.size() % sizeof(capnp::word) != 0) {
        return std::nullopt;
    }

    auto words = kj::ArrayPtr<const capnp::word>(
        reinterpret_cast<const capnp::word*>(payload.data()),
        payload.size() / sizeof(capnp::word));
    return std::optional<ParsedPacket>{std::in_place, words};
}

} // namespace gs::protocol
