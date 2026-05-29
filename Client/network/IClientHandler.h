#pragma once

#include "CharacterListItem.h"

#include <cstdint>
#include <string>
#include <vector>

namespace client::net {

struct IClientHandler {
    virtual ~IClientHandler() = default;

    virtual void OnConnectionFailed(const std::string& reason) = 0;
    virtual void OnDisconnected() = 0;

    virtual void OnHandshakeAccepted() = 0;
    virtual void OnHandshakeRejected(const std::string& reason) = 0;

    virtual void OnLoginAccepted(std::uint64_t account_id) = 0;
    virtual void OnLoginRejected(const std::string& reason) = 0;

    virtual void OnCharacterList(const std::vector<CharacterListItem>& characters) = 0;
};

} // namespace client::net
