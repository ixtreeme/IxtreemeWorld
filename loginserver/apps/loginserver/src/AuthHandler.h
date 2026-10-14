#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <db/AccountRepository.h>
#include <network/Session.h>

#include "SessionContext.h"
#include "schema/packet.capnp.h"

namespace gs::server {

class AuthHandler {
public:
    explicit AuthHandler(gs::db::AccountRepository& accounts);

    using AuthSuccessCallback =
        std::function<void(std::shared_ptr<gs::network::Session> session, AuthInfo auth)>;
    using AuthFailureCallback =
        std::function<void(std::shared_ptr<gs::network::Session> session,
                           const std::string& reason)>;

    void SetAuthSuccessCallback(AuthSuccessCallback cb);
    void SetAuthFailureCallback(AuthFailureCallback cb);

    void HandleLoginRequest(std::shared_ptr<gs::network::Session> session,
                            gs::protocol::LoginRequest::Reader request);

private:
    void SendLoginResponse(std::shared_ptr<gs::network::Session> session,
                           gs::protocol::LoginResult result,
                           const std::string& message,
                           std::uint64_t account_id,
                           bool close_after_send);

    gs::db::AccountRepository& accounts_;
    AuthSuccessCallback on_success_;
    AuthFailureCallback on_failure_;
};

} // namespace gs::server
