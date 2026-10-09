#include "db/CharacterRepository.h"

#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

#include <mariadb/conncpp.hpp>

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

Character ReadCharacter(sql::ResultSet& rows)
{
    Character character;
    character.id = CharacterId{rows.getUInt64("id")};
    character.account_id = AccountId{rows.getUInt64("account_id")};
    character.slot = static_cast<std::uint8_t>(rows.getUInt("slot"));
    character.name = rows.getString("name").c_str();
    character.level = rows.getUInt("level");
    character.experience = rows.getUInt64("experience");
    character.class_id = static_cast<std::uint16_t>(rows.getUInt("class_id"));
    character.appearance = static_cast<std::uint16_t>(rows.getUInt("appearance"));
    character.pos_x = rows.getInt("pos_x");
    character.pos_y = rows.getInt("pos_y");
    character.map_id = static_cast<std::uint16_t>(rows.getUInt("map_id"));
    character.created_at = ParseDateTime(rows.getString("created_at").c_str());
    if (!rows.isNull("last_played_at")) {
        character.last_played_at = ParseDateTime(rows.getString("last_played_at").c_str());
    }
    return character;
}

} // namespace

CharacterRepository::CharacterRepository(DbPool& pool)
    : pool_(pool)
{
}

void CharacterRepository::ListByAccount(
    AccountId account_id,
    std::function<void(Result<std::vector<Character>>)> completion)
{
    pool_.Submit<std::vector<Character>>(
        [account_id](sql::Connection& conn) {
            Result<std::vector<Character>> result;
            std::unique_ptr<sql::PreparedStatement> stmt(conn.prepareStatement(
                "SELECT id, account_id, slot, name, level, experience, class_id, appearance, "
                "pos_x, pos_y, map_id, created_at, last_played_at "
                "FROM characters WHERE account_id = ? ORDER BY slot ASC"));
            stmt->setUInt64(1, static_cast<std::uint64_t>(account_id));

            std::unique_ptr<sql::ResultSet> rows(stmt->executeQuery());
            std::vector<Character> characters;
            while (rows->next()) {
                characters.push_back(ReadCharacter(*rows));
            }

            result.value = std::move(characters);
            return result;
        },
        std::move(completion));
}

void CharacterRepository::FindById(CharacterId id, std::function<void(Result<Character>)> completion)
{
    pool_.Submit<Character>(
        [id](sql::Connection& conn) {
            Result<Character> result;
            std::unique_ptr<sql::PreparedStatement> stmt(conn.prepareStatement(
                "SELECT id, account_id, slot, name, level, experience, class_id, appearance, "
                "pos_x, pos_y, map_id, created_at, last_played_at "
                "FROM characters WHERE id = ?"));
            stmt->setUInt64(1, static_cast<std::uint64_t>(id));

            std::unique_ptr<sql::ResultSet> rows(stmt->executeQuery());
            if (!rows->next()) {
                result.error = DbError::NotFound;
                result.message = "character not found";
                return result;
            }

            result.value = ReadCharacter(*rows);
            return result;
        },
        std::move(completion));
}

void CharacterRepository::FindByAccountAndId(
    AccountId account_id,
    CharacterId character_id,
    std::function<void(Result<Character>)> completion)
{
    pool_.Submit<Character>(
        [account_id, character_id](sql::Connection& conn) {
            Result<Character> result;
            std::unique_ptr<sql::PreparedStatement> stmt(conn.prepareStatement(
                "SELECT id, account_id, slot, name, level, experience, class_id, appearance, "
                "pos_x, pos_y, map_id, created_at, last_played_at "
                "FROM characters WHERE account_id = ? AND id = ?"));
            stmt->setUInt64(1, static_cast<std::uint64_t>(account_id));
            stmt->setUInt64(2, static_cast<std::uint64_t>(character_id));

            std::unique_ptr<sql::ResultSet> rows(stmt->executeQuery());
            if (!rows->next()) {
                result.error = DbError::NotFound;
                result.message = "character does not belong to account";
                return result;
            }

            result.value = ReadCharacter(*rows);
            return result;
        },
        std::move(completion));
}

} // namespace gs::db
