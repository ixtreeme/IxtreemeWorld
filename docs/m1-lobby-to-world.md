# M1 — Lobby → világ belépés (lobby + game szerver) — Codex-specifikáció

> **Verzió:** v2 (a tényleges kódbázishoz és a kétszerveres topológiához igazítva).
> **Hogyan használd:** add át Codexnek a teljes architektúra-terv (`mmorpg_architektura_es_epitesi_terv.md`) **mellett**, állandó kontextusként. Ez az M1 mérföldkő scope-olt feladata. Ne lépj M2-re, amíg az elfogadási kritériumok (11.) nem teljesülnek.

---

## 1. Cél (egy mondatban)

Egy autentikált játékos a **lobby-szerverről** kiválaszt egy karaktert, kap egy handoff-tokent + a **game-szerver** endpointját, csatlakozik a game-szerverhez, az validálja a tokent, beilleszti a játékost egyetlen flecs worldbe, ahol szerver-autoritatív módon mozog — és két játékos valós időben látja egymást.

## 2. Kiinduló állapot (a meglévő kód — NE építsd újra)

A szerveroldali kódbázis már tartalmazza:
- `libs/common`, `libs/db`, `libs/network` — közös libek (mindkét szerver ezeket linkeli).
- `libs/network`: asio-coroutine `Session`, 4 bájtos hossz-prefixes framing (`Framing`), payload-agnosztikus → a custom bináris hot path is átmegy rajta.
- `libs/db`: `DbPool` (dedikált worker-szálak, completion visszaposztolva az io_context-re), `AccountRepository` (argon2/libsodium `crypto_pwhash_str_verify`, buffer-wipe), `CharacterRepository`.
- A mostani `apps/gameserver` **valójában a lobby-szerver szerepét tölti be**: handshake → login → karakterlista. Állapotgép: `WaitingHandshake → ConnectionEstablished → Authenticating → Authenticated`.
- Séma: `accounts`, `characters` (van `pos_x/pos_y` INT, `map_id`).

Kliensoldal: Vulkan render, terrain/Granny/warrior/nameplate, Noesis UI, Win32+Android, `ClientSession` (asio polling, `io.poll()` az `Update()`-ből), állapotgép a fentiek tükrében.

### M1 átnevezés / új bináris
- A mostani `apps/gameserver` → **`apps/loginserver`** (lobby: login + karakterlista + karakterválasztás + token-kiadás).
- Új: **`apps/gameserver`** (world: flecs world, mozgás-szimuláció, token-validálás, belépés).
- Mindkettő a meglévő `libs/*`-ot linkeli.

## 3. Topológia (M1)

```
[Kliens] --(1) handshake/login/charlist/select--> [Lobby-szerver] --írja--> (handoff_tokens tábla, MariaDB)
[Kliens] <--(2) token + game endpoint-- [Lobby-szerver]
[Kliens] --(3) connect + EnterWorld{token}--> [Game-szerver] --atomi consume/olvas--> (handoff_tokens tábla)
[Game-szerver] --betölti a karaktert--> (characters tábla)  --spawn--> [flecs world]
```

A token-store M1-ben **közös MariaDB tábla** (nincs új infra; Redis csak M8). Atomi, egyszeri felhasználás → két szerver közt is race-mentes.

## 4. Scope

### Benne van (M1)
- Lobby: karakterválasztás/-létrehozás (minimális), handoff-token kiadása + game endpoint.
- `handoff_tokens` tábla + atomi consume.
- Game-szerver: token-validálás, karakter-betöltés, egyetlen flecs world (egy zóna, `[0,1000]×[0,1000]` m, sík).
- Játékos-entitás spawn belépéskor, despawn kilépéskor.
- Irányalapú mozgás-input (kliens→szerver), szerver-autoritatív integrálás, broadcast.
- **Sim-szál ↔ IO-szál tiszta szétválasztása** a game-szerveren (lásd 5.).
- Fix lépésközű tick-loop 20 Hz.
- Custom bináris wire a mozgás hot pathra; Cap'n Proto a handshake/login/charlist/select/spawn üzenetekre.
- Kliens: második (game-szerver) kapcsolat + EnterWorld fázis + bináris mozgás.

### NINCS benne (későbbi mérföldkövek)
- AOI / interest management → M2. M1: mindenki látja mindenkit (kevés tesztjátékos).
- Több zóna, ghost-replikáció → M3. Migráció → M4. Harc/mobok → M5–M6.
- Terep-magasság, collision → később. M1: sík, `z = 0`.
- Delta-tömörítés, pozíció-kvantálás → M2+. M1: teljes snapshot, `f32` pozíció a wire-en.
- Kliensoldali reconciliation → később. M1: a `seq`-et küldjük/tároljuk, de még nem korrigálunk.

## 5. ELŐFELTÉTELEK — a review-ból, M1 előtt/közben rendezni

1. **io_context szálazás:** dönts a game-szerveren: egyszálú io_context (egyszerű, de nem skálázik) vagy többszálú. Ha többszálú, a handler-állapot (pl. `contexts_` analógia) thread-safe legyen. A login-szerver maradhat egyszálú.
2. **A world a sim-szálé.** A game-szerveren az inline-handler stílust NE terjeszd ki a gameplayre. Az `OnPayload` (IO-szál) a mozgás-inputot thread-safe queue-ba teszi; a sim-szál a tick elején üríti. Az IO-szál SOHA nem nyúl a worldhöz.
3. **Send-queue minta:** a per-tick broadcasthoz a game-szerver küldési útja ne `co_spawn`-oljon packetenként. Vegyél át per-session kimenő queue + `writing` flag + egyetlen drain-coroutine mintát (a kliens már ezt csinálja).
4. **Coordinate-konvenció:** rögzítsd. Javaslat M1: a wire és a sim `f32` méter; a DB `characters.pos_x/pos_y` INT marad fixponttal (definiáld a skálát, pl. 1 INT = 1 mm → 1000 = 1 m), a be/kimentésnél konvertálj. Vagy válts a DB-oszlopokat `FLOAT`/`DOUBLE`-re. A lényeg: egy explicit, dokumentált konvenció.

## 6. Threading-modell (game-szerver, M1)

- **asio IO:** `io_context` szál-pool (kiindulás 2–4). Beérkező packet parse → hot-path mozgás-input: push a **bejövő input queue**-ba (MPSC), `session_id`-vel. Strukturált (EnterWorld stb.): control-path.
- **Sim-szál:** egyetlen szál, fix lépésközű tick-loop (7.), birtokolja a flecs worldöt.
- **Kimenő:** a sim-szál send-jobokat tölt; `asio::post`-tal adja át az IO-rétegnek.
- DB (token-validálás, karakter-betöltés): a meglévő `DbPool` mintán, completion visszaposztolva — a sim-szál SOHA ne blokkoljon DB-n. A belépő spawn a DB-completion után, a sim-szálra posztolva történik.

## 7. Tick-loop (sim-szál, 20 Hz, dt = 50 ms)

```
loop minden 50 ms:
  1. INPUT DRAIN: bejövő input queue ürítése; session→entitás; legutóbbi MoveInput → MoveIntent.
  2. WORLD PROGRESS (flecs world.progress(dt)): ApplyMoveIntent → IntegrateMovement.
  3. SNAPSHOT BUILD: iterálj a játékosokon → S2C_EntityTransforms packet (9.2).
  4. EGRESS: broadcast minden in-world session-nek (M1: nincs AOI); send-job → asio::post.
  5. world_tick++.
```

A belépés/kilépés (spawn/despawn) szintén a sim-szálon, a tick-loop elején vagy a DB-completion sim-szálra posztolásakor dolgozódik fel — soha nem közvetlenül az IO-szálról.

## 8. flecs adatmodell (M1)

### Komponensek
```cpp
struct Position   { float x, y, z; };           // világkoordináta, méter
struct Velocity   { float x, y, z; };            // m/s
struct Heading    { float angle; };              // radián, 0..2π
enum class MoveState : uint8_t { Idle = 0, Walking = 1, Running = 2 };
struct MoveIntent { float dir_angle; MoveState state; uint32_t last_input_seq; };
struct MoveSpeed  { float walk; float run; };    // m/s, sebesség-cap forrása
struct NetId      { uint32_t value; };           // stabil, wire-re küldött azonosító
struct SessionRef { gs::common::SessionId session; };
// PlayerTag: zero-size tag
```
- `NetId.value` egy dedikált, monoton allokátortól (NE a flecs entity id-t küldd a wire-re).

### Rendszerek
- **ApplyMoveIntent**: `MoveIntent`,`MoveSpeed`,`Heading` → `Velocity`,`Heading`. Idle→0; Walk/Run a sebesség, irány `dir_angle`-ből; `vz=0`.
- **IntegrateMovement**: `Velocity` → `Position += v*dt`; clamp `x,y ∈ [0,1000]`; `z=0`.

### Snapshot-build (NEM rendszer, a tick-loop 3. lépése)
- Iterálj `PlayerTag`+`NetId`+`Position`+`Heading`+`MoveIntent(state)` → S2C_EntityTransforms.

## 9. Wire-formátum

### 9.1 Strukturált — Cap'n Proto (a meglévő `protocol`/`schema` libbe illesztve)

Új üzenetek a meglévő `Packet` union mellé:
```capnp
struct Vec3 { x @0 :Float32; y @1 :Float32; z @2 :Float32; }

# Lobby → Kliens (karakterválasztás után)
struct S2C_EnterWorldToken {
  token    @0 :Data;     # nyers token (a DB-ben a hash-e él — hardening)
  gameHost @1 :Text;
  gamePort @2 :UInt16;
}

# Kliens → Game-szerver
struct C2S_EnterWorld { token @0 :Data; }

# Game-szerver → Kliens
struct S2C_EnterWorldAccept { yourNetId @0 :UInt32; spawnPos @1 :Vec3; serverTick @2 :UInt32; }
struct S2C_EnterWorldReject {
  reason @0 :RejectReason;
  enum RejectReason { invalidToken @0; expiredToken @1; alreadyUsed @2; serverError @3; }
}
struct S2C_EntitySpawn   { netId @0 :UInt32; name @1 :Text; classId @2 :UInt16; spawnPos @3 :Vec3; heading @4 :UInt16; }
struct S2C_EntityDespawn { netId @0 :UInt32; }
```

### 9.2 Hot-path — custom bináris (little-endian)
Szögkvantálás: `q = round(angle/(2π)*65535)` → u16.

**C2S_MoveInput** (8 bájt): `[type:u8=0x01][seq:u32][dir_angle_q:u16][move_state:u8]`

**S2C_EntityTransforms**: fejléc `[type:u8=0x10][server_tick:u32][count:u16]`, majd `count`× rekord (19 bájt): `[net_id:u32][x:f32][y:f32][z:f32][heading_q:u16][move_state:u8]`

> M1: teljes snapshot minden tickben. Delta/kvantálás → M2+.

## 10. handoff-token protokoll + tábla

### Tábla (új migration a meglévő `schema.sql` mellé)
```sql
CREATE TABLE IF NOT EXISTS handoff_tokens (
    token_hash    CHAR(64) NOT NULL,            -- a token SHA-256 hex-e (ne plaintext)
    account_id    BIGINT UNSIGNED NOT NULL,
    character_id  BIGINT UNSIGNED NOT NULL,
    game_server   VARCHAR(64) NOT NULL,
    issued_at     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at    DATETIME NOT NULL,
    consumed      TINYINT(1) NOT NULL DEFAULT 0,
    PRIMARY KEY (token_hash),
    KEY idx_handoff_account (account_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

### Folyamat
1. Lobby (Authenticated állapotban) karakterválasztáskor: generál egy nyers tokent (`randombytes_buf` → hex), kiszámolja a hash-ét, beírja a `handoff_tokens`-be `(token_hash, account_id, character_id, game_server, expires_at = NOW()+30s)`.
2. Lobby → kliens: `S2C_EnterWorldToken{ token(nyers), gameHost, gamePort }`.
3. Kliens connect a game-szerverhez → handshake → `C2S_EnterWorld{ token }`.
4. Game-szerver: kiszámolja a hash-t, atomi consume:
   `UPDATE handoff_tokens SET consumed=1 WHERE token_hash=? AND consumed=0 AND expires_at>NOW();`
   ha érintett sor == 1 → érvényes; betölti a karaktert (`character_id`), spawnol, `S2C_EnterWorldAccept`. Különben `S2C_EnterWorldReject`.
5. Kilépés/disconnect: despawn + `S2C_EntityDespawn` broadcast, session lezárás.

## 11. Elfogadási kritériumok

- Két kliens: lobby login → charlist → select → token → game-szerver belépés sikeres.
- Mindkettő spawnol, és **valós időben látja a másik mozgását**, 20 Hz-en.
- Pozíció **szerver-autoritatív**: a kliens csak inputot küld; túlsebesség a szerveren cap-elődik.
- **Sim↔IO szétválasztás** sértetlen (kód-review igazolja: IO-szál nem éri el a worldöt).
- Lejárt vagy újrahasznált token elutasítva (`expiredToken`/`alreadyUsed`).
- Disconnectkor a többi kliens despawnt kap, nincs „szellem" entitás.

## 12. Kötelező tesztek

1. **Determinisztikus mozgás-integrálás:** fix kezdő + fix input + fix dt → ismert végpozíció N tick után.
2. **Sebesség-cap:** a `MoveSpeed`-en felüli input cap-elődik.
3. **Zóna-clamp:** `[0,1000]`-re korlátozódik.
4. **Token életciklus:** érvényes token elfogadva+konzumálva; ugyanaz másodszorra `alreadyUsed`; lejárt `expiredToken`. (Az atomi UPDATE konkurens hívásokkal: csak egy nyer.)
5. **Input-queue konkurencia:** több termelő + egy fogyasztó → nincs vesztés/korrupció, `seq` monoton session-önként.
6. **Snapshot-build:** N játékos → `count==N`, a rekordok egyeznek a world-állapottal.

## 13. Projektszerkezet (a meglévőre építve)

```
/apps/loginserver   # volt "gameserver": login + charlist + select + token-kiadás
/apps/gameserver    # ÚJ: world (flecs), sim-szál, mozgás, token-validálás, belépés
/libs/common /libs/db /libs/network   # közös, mindkét app linkeli
/libs/sim           # ÚJ (vagy a gameserver appon belül): komponensek, rendszerek, tick-loop
/libs/proto         # capnp séma + generált kód (a hiányzó protocol lib helye)
```
- Build: CMake; C++ szabvány a meglévőhöz igazítva (a kód C++20-as elemeket használ — `std::span`, `std::format`).

## 14. Invariánsok, amiket a promptban explicitté kell tenni Codexnek

- „A world a sim-szálé; IO-szál nem nyúl hozzá."
- „A kliens inputot küld, nem pozíciót; a szerver autoritatív."
- „Fix lépésköz, determinisztikus integrálás."
- „A `NetId` dedikált, stabil, monoton — nem a flecs entity id."
- „A handoff-token egyszer használatos, rövid életű; a DB-ben a hash-e él; consume atomi UPDATE-tel."
- „A hot path (mozgás) custom bináris; minden más Cap'n Proto."
- „A DB-completion a sim-szálra posztolva spawnol; az IO-szál nem spawnol a worldbe."