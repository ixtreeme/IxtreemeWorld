#include "protocol/ProtocolError.h"

namespace gs::protocol {

const char* ProtocolErrorString(ProtocolError e)
{
    switch (e) {
    case ProtocolError::None:
        return "none";
    case ProtocolError::InvalidMessage:
        return "invalid message";
    case ProtocolError::UnexpectedPacketType:
        return "unexpected packet type";
    case ProtocolError::WrongState:
        return "wrong state";
    case ProtocolError::PayloadTooLarge:
        return "payload too large";
    case ProtocolError::InternalError:
        return "internal error";
    }

    return "unknown protocol error";
}

} // namespace gs::protocol
