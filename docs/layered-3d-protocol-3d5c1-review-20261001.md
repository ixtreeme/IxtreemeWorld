# 3D-5C1 – rétegzett transform-frame és protokoll-egyeztetés (review, 2026-10-01)

Állapot: **kész, review-ra vár.** Alap: `8035c35` (3D-5B2).

## Cél

A 3D-5A óta a szerver tudja, melyik entitás melyik szinten áll, de a
kliens csak a z-t látta. A 3D-5C1 a szintváltást a hálózaton is
eljuttatja azokhoz a kliensekhez, amelyek ezt kérik, úgy, hogy a régi
(protokoll 1-es) kliensek bájtra ugyanazt kapják, mint eddig.

Döntés (felhasználói utasítás): a kliensoldali protokoll-réteg **nem kerül
az engine-be**, hanem a scene-hez csatolt scriptként készül (3D-5C2). Ez a
dokumentum ezért a teljes wire-formátumot is leírja a script számára.

## Protokoll-egyeztetés

- `gs::protocol::kProtocolVersion = 2` (a build legmagasabb verziója),
  `kMinProtocolVersion = 1`, `kLayeredFramesProtocolVersion = 2`.
- A loginserver és a gameserver a `[1, 2]` tartományt fogadja el; a
  `HandshakeResponse.serverProtocolVersion` az **egyeztetett** verzió
  (`min(kliens, szerver)`), így egy v1 kliens továbbra is 1-et lát.
  Tartományon kívül: `PROTOCOL_VERSION_MISMATCH`, a válasz a szerver
  legmagasabb verzióját (2) adja, a kapcsolat lezárul.
- A gameserver a sessionön tárolja az egyeztetett verziót
  (`Session::ProtocolVersion()`).

## Wire-formátum (bináris codec, `payload[0] = 1`)

Minden csomag 4 bájtos big-endian hosszelőtaggal érkezik; a payload első
bájtja a codec (0 = Cap'n Proto, 1 = bináris). A számok little-endian
kódolásúak.

**v2 frame (opcode `0x11`) – protokoll 1-es sessionöknek, változatlan:**

```
u8 codec=1 | u8 0x11 | u32 world_tick | u16 record_count
viewer record (19 B): u32 net | f32 x | f32 y | f32 z | u16 heading_q | u8 move_state
(record_count - 1) × delta: u32 net | u8 mask | [0x01: 3×f32 pos] [0x02: u16 heading] [0x04: u8 move]
```

**v3 frame (opcode `0x12`) – protokoll ≥ 2 sessionöknek:**

```
u8 codec=1 | u8 0x12 | u32 world_tick | u16 record_count
viewer record (19 B, mint fent)
u32 viewer_volume_id | u32 viewer_layer_id        (0/0 = terep)
(record_count - 1) × delta: u32 net | u8 mask
   [0x01: 3×f32 pos] [0x02: u16 heading] [0x04: u8 move] [0x08: u32 volume_id, u32 layer_id]
```

- A `0x08` bit akkor jön, ha az entitás szintje eltér attól, amit a
  kliens ismer (a spawn-üzenet `volumeId`/`layerId` mezője az alap); a
  periodikus resync teljes rekordja `0x0F`.
- Új Cap'n Proto mezők (additívak): `S2cEntitySpawn.volumeId/layerId`,
  `S2cEnterWorldAccept.spawnVolumeId/spawnLayerId`,
  `C2sEnterWorld.DebugSpawnOverride.volumeId`.
- A v1 (opcode `0x10`) frame csak a `v2_enabled = false` A/B
  konfigurációban él; rétegzett mezőt soha nem visz.

## Szerveroldali megvalósítás

- `ProtocolEncoder`: `kTransformFieldLayer`, `kTransformFieldAllLayered`,
  `kTransformFrameV3Opcode`, `EncodeTransformFrameV3`; az
  `AppendTransformDelta` a layer-mezőt csak a `0x08` bitnél írja (a v2
  bájtok változatlanok).
- `VisibilitySystem`: a címzett-állapot (`RecipientEntity`) kapott
  `volume_id`/`layer_id` mezőt; rétegzett sessionnél a delta a
  szintváltást is jelzi.
- `ReplicationSystem`: sessionönként választ v3 / v2 / v1 frame között.
- `ReplicationValidator`: rétegzett címzettnél a kliens-ismert szintnek is
  konvergálnia kell az autoritatív szinthez.

## Tesztek (lefuttatva, Windows, Debug)

| Csomag | Eredmény |
|---|---|
| `worldbench --mode layeredpresence` | **62/62** (5 új, valódi loopback TCP-sessionökkel) |
| – rétegzett mászó csak v3 frame-et kap; saját volume-sorrend: padló → 4 fok → pihenő | PASS |
| – rétegzett megfigyelő megtudja a mászó minden szintváltását (ismert = pihenő) | PASS |
| – protokoll 1-es session csak v2 frame-et kap (92 db), v3 / layer-mező 0 | PASS |
| – szigorú, független dekóder: minden frame bájtra pontosan elfogy | PASS |
| – replikációs árnyék-audit rétegzett címzettekkel | OK |
| `worldbench --mode netstress` (valódi `GameConnectionHandler`, DB nélkül) | failures=0; új eset: v0/v3 elutasítva, v1→1, v2→2 |
| loginserver build | sikeres |
| worldbench regresszió (21 mód: layeredpresence, aoi, ghost, border, replication, replv2, hygiene, presence, protocol, snapshot, activitytemporal, mapsplit, terrain, worldpackage, map4, mapaudit, layersupport, layerlookup, layerclearance, splitmerge, inputpath) | mind failures=0 |

## Hatókör-bővítések (review-ra jelölve)

1. Protokollverzió 2 és tartomány-alapú elfogadás mindkét szerveren
   (korábban pontos egyezés); a válasz az egyeztetett verziót adja vissza.
2. `gs::network::Session::SetProtocolVersion/ProtocolVersion`.
3. Új frame-opkód (`0x12`) és mezőbit (`0x08`).

## Korlátok

- Kliens még nem fogyasztja (3D-5C2: scene-script kliens).
- A régi standalone kliens (`D:\AurigaGlobal\LiveWork`) érintetlen; v1-es
  kliensként továbbra is működnie kell (a v1 frame-ek bájtra azonosak),
  élőben ezt nem teszteltem.
- Csak Windows/MSVC Debug futott.
