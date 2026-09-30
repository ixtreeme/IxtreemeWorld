#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "db/DbPool.h"
#include "db/Result.h"
#include "db/Types.h"

namespace gs::db {

struct Character {
    CharacterId id{0};
    AccountId account_id{0};
    std::uint8_t slot{0};
    std::string name;
    std::uint32_t level{1};
    std::uint64_t experience{0};
    std::uint16_t class_id{0};
    std::uint16_t appearance{0};
    std::int32_t pos_x{0};
    std::int32_t pos_y{0};
    std::uint16_t map_id{0};
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_played_at;
};

class CharacterRepository {
public:
    explicit CharacterRepository(DbPool& pool);

    void ListByAccount(AccountId account_id,
                       std::function<void(Result<std::vector<Character>>)> completion);

    void FindById(CharacterId id, std::function<void(Result<Character>)> completion);

    void FindByAccountAndId(AccountId account_id,
                            CharacterId character_id,
                            std::function<void(Result<Character>)> completion);

private:
    DbPool& pool_;
};

} // namespace gs::db
