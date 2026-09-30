#include "db/AccountRepository.h"

#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include <mariadb/conncpp.hpp>
#include <sodium.h>

namespace gs::db {
namespace {

std::chrono::system_clock::time_point ParseDateTime(const std::string& value)
{
    if (value.empty()) {
        return {};
    }

    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (stream.fail()) {
        return {};
    }

    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

Account ReadAccount(sql::ResultSet& rows)
{
    Account account;
    account.id = AccountId{rows.getUInt64("id")};
    account.username = rows.getString("username").c_str();
    account.email = rows.getString("email").c_str();
    account.status = rows.getString("status").c_str();
    account.created_at = ParseDateTime(rows.getString("created_at").c_str());
    if (!rows.isNull("last_login_at")) {
        account.last_login_at = ParseDateTime(rows.getString("last_login_at").c_str());
    }
    return account;
}

bool VerifyPassword(const std::string& stored_hash, std::string& password)
{
    if (sodium_init() < 0) {
        return false;
    }

    std::vector<char> password_buffer(password.begin(), password.end());
    const auto verified = crypto_pwhash_str_verify(stored_hash.c_str(),
                                                   password_buffer.data(),
                                                   static_cast<unsigned long long>(
                                                       password_buffer.size())) == 0;
    if (!password_buffer.empty()) {
        sodium_memzero(password_buffer.data(), password_buffer.size());
    }
    if (!password.empty()) {
        sodium_memzero(password.data(), password.size());
    }
    return verified;
}

void WipeString(std::string& value)
{
    if (!value.empty()) {
        sodium_memzero(value.data(), value.size());
    }
}

} // namespace

AccountRepository::AccountRepository(DbPool& pool)
    : pool_(pool)
{
}

void AccountRepository::Authenticate(std::string username,
                                     std::string password,
                                     std::function<void(Result<Account>)> completion)
{
    pool_.Submit<Account>(
        [username = std::move(username), password = std::move(password)](
            sql::Connection& conn) mutable {
            Result<Account> result;
            std::unique_ptr<sql::PreparedStatement> stmt(conn.prepareStatement(
                "SELECT id, username, email, password_hash, status, created_at, last_login_at "
                "FROM accounts WHERE username = ?"));
            stmt->setString(1, username);

            std::unique_ptr<sql::ResultSet> rows(stmt->executeQuery());
            if (!rows->next()) {
                WipeString(password);
                result.error = DbError::InvalidCredentials;
                result.message = "invalid username or password";
                return result;
            }

            const std::string stored_hash = rows->getString("password_hash").c_str();
            const std::string status = rows->getString("status").c_str();
            if (status == "banned") {
                WipeString(password);
                result.error = DbError::AccountBanned;
                result.message = "account is banned";
                return result;
            }

            if (!VerifyPassword(stored_hash, password)) {
                result.error = DbError::InvalidCredentials;
                result.message = "invalid username or password";
                return result;
            }

            auto account = ReadAccount(*rows);

            std::unique_ptr<sql::PreparedStatement> update(
                conn.prepareStatement("UPDATE accounts SET last_login_at = NOW() WHERE id = ?"));
            update->setUInt64(1, static_cast<std::uint64_t>(account.id));
            update->executeUpdate();

            account.last_login_at = std::chrono::system_clock::now();
            result.value = std::move(account);
            return result;
        },
        std::move(completion));
}

void AccountRepository::FindById(AccountId id, std::function<void(Result<Account>)> completion)
{
    pool_.Submit<Account>(
        [id](sql::Connection& conn) {
            Result<Account> result;
            std::unique_ptr<sql::PreparedStatement> stmt(conn.prepareStatement(
                "SELECT id, username, email, status, created_at, last_login_at "
                "FROM accounts WHERE id = ?"));
            stmt->setUInt64(1, static_cast<std::uint64_t>(id));

            std::unique_ptr<sql::ResultSet> rows(stmt->executeQuery());
            if (!rows->next()) {
                result.error = DbError::NotFound;
                result.message = "account not found";
                return result;
            }

            result.value = ReadAccount(*rows);
            return result;
        },
        std::move(completion));
}

} // namespace gs::db
