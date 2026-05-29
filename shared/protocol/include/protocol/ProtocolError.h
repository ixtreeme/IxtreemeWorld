#pragma once

namespace gs::protocol {

enum class ProtocolError {
    None,
    InvalidMessage,
    UnexpectedPacketType,
    WrongState,
    PayloadTooLarge,
    InternalError,
};

const char* ProtocolErrorString(ProtocolError e);

} // namespace gs::protocol
