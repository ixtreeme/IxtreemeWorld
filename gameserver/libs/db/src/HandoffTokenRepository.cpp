#include "db/HandoffTokenRepository.h"

#include <memory>
#include <utility>

#include <mariadb/conncpp.hpp>

namespace gs::db {

const char* HandoffTokenConsumeErrorString(HandoffTokenConsumeError error)
{
    switch (error) {
    case HandoffTokenConsumeError::None:
        return "None";
    case HandoffTokenConsumeError::InvalidToken:
        return "InvalidToken";
    case HandoffTokenConsumeError::ExpiredToken:
        return "ExpiredToken";
    case HandoffTokenConsumeError::AlreadyUsed:
        return "AlreadyUsed";
    case HandoffTokenConsumeError::QueryFailed:
        return "QueryFailed";
    }

    return "Unknown";
}

HandoffTokenRepository::HandoffTokenRepository(DbPool& pool)
    : pool_(pool)
{
}

void HandoffTokenRepository::Store(std::string token_hash,
                                   AccountId account_id,
                                   CharacterId character_id,
                                   std::string game_server,
                                   std::uint32_t ttl_seconds,
                                   std::function<void(VoidResult)> completion)
{
    pool_.SubmitVoid(
        [token_hash = std::move(token_hash),
         account_id,
         character_id,
         game_server = std::move(game_server),
         ttl_seconds](sql::Connection& conn) {
            VoidResult result;
            std::unique_ptr<sql::PreparedStatement> stmt(conn.prepareStatement(
                "INSERT INTO handoff_tokens "
                "(token_hash, account_id, character_id, game_server, expires_at) "
                "VALUES (?, ?, ?, ?, DATE_ADD(NOW(), INTERVAL ? SECOND))"));
            stmt->setString(1, token_hash);
            stmt->setUInt64(2, static_cast<std::uint64_t>(account_id));
            stmt->setUInt64(3, static_cast<std::uint64_t>(character_id));
            stmt->setString(4, game_server);
            stmt->setUInt(5, ttl_seconds);
            stmt->executeUpdate();
            return result;
        },
        std::move(completion));
}

void HandoffTokenRepository::Consume(std::string token_hash,
                                     std::string game_server,
                                     std::function<void(HandoffTokenConsumeResult)> completion)
{
    pool_.Submit<HandoffTokenConsumeResult>(
        [token_hash = std::move(token_hash),
         game_server = std::move(game_server)](sql::Connection& conn) {
            Result<HandoffTokenConsumeResult> outer;
            HandoffTokenConsumeResult consume;

            std::unique_ptr<sql::PreparedStatement> update(conn.prepareStatement(
                "UPDATE handoff_tokens SET consumed=1 "
                "WHERE token_hash=? AND game_server=? AND consumed=0 AND expires_at>NOW()"));
            update->setString(1, token_hash);
            update->setString(2, game_server);
            const auto affected = update->executeUpdate();

            if (affected == 1) {
                std::unique_ptr<sql::PreparedStatement> select(conn.prepareStatement(
                    "SELECT account_id, character_id FROM handoff_tokens WHERE token_hash=?"));
                select->setString(1, token_hash);
                std::unique_ptr<sql::ResultSet> rows(select->executeQuery());
                if (!rows->next()) {
                    consume.error = HandoffTokenConsumeError::InvalidToken;
                    consume.message = "token disappeared after consume";
                } else {
                    consume.data.account_id = AccountId{rows->getUInt64("account_id")};
                    consume.data.character_id = CharacterId{rows->getUInt64("character_id")};
                }
                outer.value = std::move(consume);
                return outer;
            }

            std::unique_ptr<sql::PreparedStatement> select(conn.prepareStatement(
                "SELECT consumed, IF(expires_at <= NOW(), 1, 0) AS expired "
                "FROM handoff_tokens WHERE token_hash=? AND game_server=?"));
            select->setString(1, token_hash);
            select->setString(2, game_server);
            std::unique_ptr<sql::ResultSet> rows(select->executeQuery());

            if (!rows->next()) {
                consume.error = HandoffTokenConsumeError::InvalidToken;
                consume.message = "token not found";
            } else if (rows->getBoolean("consumed")) {
                consume.error = HandoffTokenConsumeError::AlreadyUsed;
                consume.message = "token already used";
            } else if (rows->getBoolean("expired")) {
                consume.error = HandoffTokenConsumeError::ExpiredToken;
                consume.message = "token expired";
            } else {
                consume.error = HandoffTokenConsumeError::QueryFailed;
                consume.message = "token consume failed";
            }

            outer.value = std::move(consume);
            return outer;
        },
        [completion = std::move(completion)](Result<HandoffTokenConsumeResult> result) mutable {
            if (!result || !result.value) {
                HandoffTokenConsumeResult consume;
                consume.error = HandoffTokenConsumeError::QueryFailed;
                consume.message = result.message;
                completion(std::move(consume));
                return;
            }
            completion(std::move(*result.value));
        });
}

} // namespace gs::db
