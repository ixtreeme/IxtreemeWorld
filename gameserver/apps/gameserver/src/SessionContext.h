#pragma once

#include <db/Types.h>

namespace gs::server {

enum class SessionState {
    WaitingHandshake,
    ConnectionEstablished,
    Authenticating,
    Authenticated,
};

struct AuthInfo {
    gs::db::AccountId account_id{0};
};

struct SessionContext {
    SessionState state = SessionState::WaitingHandshake;
    AuthInfo auth;
};

} // namespace gs::server
