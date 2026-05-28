@0xb8e6f4a5d2c91a73;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("gs::protocol");

# === HIBAKODOK ===
enum HandshakeResult {
  ok @0;
  protocolVersionMismatch @1;
  serverFull @2;
  banned @3;
  internalError @4;
}

# === UZENET-TIPUSOK ===

struct HandshakeRequest {
  protocolVersion @0 :UInt32;
  clientBuild @1 :Text;
}

struct HandshakeResponse {
  result @0 :HandshakeResult;
  serverProtocolVersion @1 :UInt32;
  message @2 :Text;
}

# === LOGIN ===

enum LoginResult {
  ok @0;
  invalidCredentials @1;
  accountBanned @2;
  alreadyLoggedIn @3;
  internalError @4;
}

struct LoginRequest {
  username @0 :Text;
  password @1 :Text;
}

struct LoginResponse {
  result @0 :LoginResult;
  message @1 :Text;
  accountId @2 :UInt64;
}

# === CHARACTER LIST ===

enum CharacterListResult {
  ok @0;
  notAuthenticated @1;
  internalError @2;
}

struct CharacterInfo {
  id @0 :UInt64;
  slot @1 :UInt8;
  name @2 :Text;
  level @3 :UInt32;
  classId @4 :UInt16;
  appearance @5 :UInt16;
  posX @6 :Int32;
  posY @7 :Int32;
  mapId @8 :UInt16;
}

struct CharacterListRequest {
}

struct CharacterListResponse {
  result @0 :CharacterListResult;
  message @1 :Text;
  characters @2 :List(CharacterInfo);
}

# === ROOT PACKET (union) ===

struct Packet {
  union {
    handshakeRequest @0 :HandshakeRequest;
    handshakeResponse @1 :HandshakeResponse;
    loginRequest @2 :LoginRequest;
    loginResponse @3 :LoginResponse;
    characterListRequest @4 :CharacterListRequest;
    characterListResponse @5 :CharacterListResponse;
  }
}
