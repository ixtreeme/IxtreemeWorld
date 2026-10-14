#pragma once

#include <optional>
#include <string>

namespace gs::db {

enum class DbError {
    Ok,
    ConnectionFailed,
    QueryFailed,
    NotFound,
    InvalidCredentials,
    AccountBanned,
    DuplicateKey,
    InternalError,
};

const char* DbErrorString(DbError e);

template <typename T>
struct Result {
    DbError error = DbError::Ok;
    std::string message;
    std::optional<T> value;

    [[nodiscard]] bool IsOk() const { return error == DbError::Ok; }
    explicit operator bool() const { return IsOk(); }
};

struct VoidResult {
    DbError error = DbError::Ok;
    std::string message;

    [[nodiscard]] bool IsOk() const { return error == DbError::Ok; }
    explicit operator bool() const { return IsOk(); }
};

} // namespace gs::db
