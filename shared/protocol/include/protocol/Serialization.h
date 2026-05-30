#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/array.h>

#include "schema/packet.capnp.h"

namespace gs::protocol {

inline constexpr std::uint8_t kCodecCapnp = 0;
inline constexpr std::uint8_t kCodecBinary = 1;

inline std::vector<std::uint8_t> ToByteVector(kj::ArrayPtr<const kj::byte> bytes)
{
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

inline std::vector<std::uint8_t> SerializeToBytes(capnp::MessageBuilder& builder)
{
    const auto words = capnp::messageToFlatArray(builder);
    auto bytes = words.asBytes();
    std::vector<std::uint8_t> out;
    out.reserve(1 + bytes.size());
    out.push_back(kCodecCapnp);
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

struct ParsedPacket {
    kj::Array<capnp::word> words;
    capnp::FlatArrayMessageReader reader;
    Packet::Reader packet;

    explicit ParsedPacket(kj::Array<capnp::word> owned_words)
        : words(std::move(owned_words))
        , reader(words.asPtr())
        , packet(reader.getRoot<Packet>())
    {
    }
};

inline std::optional<ParsedPacket> ParsePacket(const std::vector<std::uint8_t>& payload)
{
    if (payload.empty() || payload[0] != kCodecCapnp) {
        return std::nullopt;
    }

    const auto body_size = payload.size() - 1;
    if (body_size % sizeof(capnp::word) != 0) {
        return std::nullopt;
    }

    auto words = kj::heapArray<capnp::word>(body_size / sizeof(capnp::word));
    if (body_size > 0) {
        std::memcpy(words.begin(), payload.data() + 1, body_size);
    }
    return std::optional<ParsedPacket>{std::in_place, std::move(words)};
}

} // namespace gs::protocol
