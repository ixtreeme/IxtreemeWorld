#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "db/DbPool.h"
#include "db/Result.h"
#include "db/Types.h"

namespace gs::db {

enum class HandoffTokenConsumeError {
    None,
    InvalidToken,
    ExpiredToken,
    AlreadyUsed,
    QueryFailed,
};

const char* HandoffTokenConsumeErrorString(HandoffTokenConsumeError error);

struct HandoffTokenData {
    AccountId account_id{0};
    CharacterId character_id{0};
};

struct HandoffTokenConsumeResult {
    HandoffTokenConsumeError error = HandoffTokenConsumeError::None;
    std::string message;
    HandoffTokenData data;

    [[nodiscard]] bool IsOk() const { return error == HandoffTokenConsumeError::None; }
    explicit operator bool() const { return IsOk(); }
};

class HandoffTokenRepository {
public:
    explicit HandoffTokenRepository(DbPool& pool);

    void Store(std::string token_hash,
               AccountId account_id,
               CharacterId character_id,
               std::string game_server,
               std::uint32_t ttl_seconds,
               std::function<void(VoidResult)> completion);

    void Consume(std::string token_hash,
                 std::string game_server,
                 std::function<void(HandoffTokenConsumeResult)> completion);

private:
    DbPool& pool_;
};

} // namespace gs::db
