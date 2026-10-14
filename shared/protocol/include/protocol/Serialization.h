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

    explicit ParsedPacket(kj::Array<capnp::word> owned_words,
                          capnp::ReaderOptions options = capnp::ReaderOptions())
        : words(std::move(owned_words))
        , reader(words.asPtr(), options)
        , packet(reader.getRoot<Packet>())
    {
    }
};

// Reader limits for UNTRUSTED input (a server reading client packets). The
// library defaults (8 Mi words = 64 MiB traversal, nesting 64) are sized for
// trusted bulk data: a crafted 64 KiB frame whose pointers alias one blob
// can make a reader walk hundreds of times its own size (Cap'n Proto
// amplification). Here the traversal budget is proportional to the message
// itself and the nesting limit matches the shallow packet schema (Packet ->
// message -> nested struct), so a reader never walks more than a small
// multiple of what the peer actually sent.
struct UntrustedReaderLimits {
    static constexpr std::uint64_t kTraversalWordsPerMessageWord = 4;
    static constexpr std::uint64_t kMinTraversalWords = 64;
    static constexpr int kNestingLimit = 16;

    static capnp::ReaderOptions For(std::size_t message_words) noexcept
    {
        capnp::ReaderOptions options;
        const std::uint64_t budget = static_cast<std::uint64_t>(message_words) *
                                     kTraversalWordsPerMessageWord;
        options.traversalLimitInWords = budget > kMinTraversalWords ? budget : kMinTraversalWords;
        options.nestingLimit = kNestingLimit;
        return options;
    }
};

namespace detail {

inline std::optional<ParsedPacket> ParsePacketWith(const std::vector<std::uint8_t>& payload,
                                                   bool untrusted)
{
    if (payload.empty() || payload[0] != kCodecCapnp) {
        return std::nullopt;
    }

    const auto body_size = payload.size() - 1;
    if (body_size % sizeof(capnp::word) != 0) {
        return std::nullopt;
    }

    const auto word_count = body_size / sizeof(capnp::word);
    auto words = kj::heapArray<capnp::word>(word_count);
    if (body_size > 0) {
        std::memcpy(words.begin(), payload.data() + 1, body_size);
    }
    const auto options =
        untrusted ? UntrustedReaderLimits::For(word_count) : capnp::ReaderOptions();
    return std::optional<ParsedPacket>{std::in_place, std::move(words), options};
}

} // namespace detail

inline std::optional<ParsedPacket> ParsePacket(const std::vector<std::uint8_t>& payload)
{
    return detail::ParsePacketWith(payload, false);
}

// Same wire format, bounded reader (see UntrustedReaderLimits). Use for every
// packet received from a peer that is not trusted.
inline std::optional<ParsedPacket> ParseUntrustedPacket(const std::vector<std::uint8_t>& payload)
{
    return detail::ParsePacketWith(payload, true);
}

} // namespace gs::protocol
