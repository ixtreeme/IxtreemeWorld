#include "AuthHandler.h"

#include <cstdint>
#include <string>
#include <utility>

#include <capnp/message.h>

#include "common/Logging.h"
#include "protocol/Serialization.h"

namespace gs::server {

AuthHandler::AuthHandler(gs::db::AccountRepository& accounts)
    : accounts_(accounts)
{
}

void AuthHandler::SetAuthSuccessCallback(AuthSuccessCallback cb)
{
    on_success_ = std::move(cb);
}

void AuthHandler::SetAuthFailureCallback(AuthFailureCallback cb)
{
    on_failure_ = std::move(cb);
}

void AuthHandler::HandleLoginRequest(std::shared_ptr<gs::network::Session> session,
                                     gs::protocol::LoginRequest::Reader request)
{
    std::string username = request.getUsername().cStr();
    std::string password = request.getPassword().cStr();

    if (username.empty() || password.empty()) {
        SendLoginResponse(session,
                          gs::protocol::LoginResult::INVALID_CREDENTIALS,
                          "Empty username or password",
                          0,
                          true);
        if (on_failure_) {
            on_failure_(session, "empty username or password");
        }
        return;
    }

    LOG_INFO("Session {} login attempt: {}", session->Id(), username);

    accounts_.Authenticate(
        std::move(username),
        std::move(password),
        [this, session](gs::db::Result<gs::db::Account> result) {
            if (!result || !result.value) {
                gs::protocol::LoginResult login_result =
                    gs::protocol::LoginResult::INTERNAL_ERROR;
                switch (result.error) {
                case gs::db::DbError::InvalidCredentials:
                    login_result = gs::protocol::LoginResult::INVALID_CREDENTIALS;
                    break;
                case gs::db::DbError::AccountBanned:
                    login_result = gs::protocol::LoginResult::ACCOUNT_BANNED;
                    break;
                default:
                    login_result = gs::protocol::LoginResult::INTERNAL_ERROR;
                    break;
                }

                LOG_INFO("Session {} login failed: {}",
                         session->Id(),
                         gs::db::DbErrorString(result.error));
                SendLoginResponse(session, login_result, "Login failed", 0, true);
                if (on_failure_) {
                    on_failure_(session, gs::db::DbErrorString(result.error));
                }
                return;
            }

            LOG_INFO("Session {} login OK: account_id={}, username={}",
                     session->Id(),
                     gs::db::ToUint64(result.value->id),
                     result.value->username);

            if (on_success_) {
                on_success_(session, AuthInfo{.account_id = result.value->id});
            }

            SendLoginResponse(session,
                              gs::protocol::LoginResult::OK,
                              "Welcome",
                              gs::db::ToUint64(result.value->id),
                              false);
        });
}

void AuthHandler::SendLoginResponse(std::shared_ptr<gs::network::Session> session,
                                    gs::protocol::LoginResult result,
                                    const std::string& message,
                                    std::uint64_t account_id,
                                    bool close_after_send)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto response = packet.initLoginResponse();
    response.setResult(result);
    response.setMessage(message);
    response.setAccountId(account_id);

    if (close_after_send) {
        session->SendPayloadAndClose(gs::protocol::SerializeToBytes(msg));
    } else {
        session->SendPayload(gs::protocol::SerializeToBytes(msg));
    }
}

} // namespace gs::server
