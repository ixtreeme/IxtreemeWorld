#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "db/DbPool.h"
#include "db/Result.h"
#include "db/Types.h"

namespace gs::db {

struct Account {
    AccountId id{0};
    std::string username;
    std::string email;
    std::string status;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_login_at;
};

class AccountRepository {
public:
    explicit AccountRepository(DbPool& pool);

    void Authenticate(std::string username,
                      std::string password,
                      std::function<void(Result<Account>)> completion);

    void FindById(AccountId id, std::function<void(Result<Account>)> completion);

private:
    DbPool& pool_;
};

} // namespace gs::db
