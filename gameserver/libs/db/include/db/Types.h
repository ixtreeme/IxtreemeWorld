#pragma once

#include <cstdint>

namespace gs::db {

enum class AccountId : std::uint64_t {};
enum class CharacterId : std::uint64_t {};

constexpr AccountId InvalidAccountId{0};
constexpr CharacterId InvalidCharacterId{0};

inline constexpr std::uint64_t ToUint64(AccountId id) noexcept
{
    return static_cast<std::uint64_t>(id);
}

inline constexpr std::uint64_t ToUint64(CharacterId id) noexcept
{
    return static_cast<std::uint64_t>(id);
}

} // namespace gs::db
