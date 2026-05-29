#pragma once

#include "IClientHandler.h"

#include <cstdint>
#include <memory>
#include <string>

namespace client::net {

class ClientSession {
public:
    explicit ClientSession(IClientHandler& handler);
    ~ClientSession();

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    void Connect(const std::string& host, std::uint16_t port);
    void Disconnect();
    bool IsConnected() const;
    bool IsAuthenticated() const;

    void SendHandshake(std::uint32_t protocol_version = 1,
                       const std::string& client_build = "vulkan-client-0.1");
    void SendLogin(const std::string& username, const std::string& password);
    void SendCharacterListRequest();

    void Update();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace client::net
