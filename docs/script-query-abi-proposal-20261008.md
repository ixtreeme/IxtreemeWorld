# Script lekérdezések és entitás-engedélyezés: ABI-javaslat

Állapot: a felhasználó 2026-10-08-án elfogadta az ABI 7 javasolt működését; implementálva. A natív modul ABI-verziója 7
(`Client/sdk/include/ixtreeme/IxModuleApi.h`). A virtuális felület bővítése a régi DLL-ek újrafordítását
igényli; az engine továbbra is elutasítja az eltérő ABI-verziót.

## Elfogadott változat: ABI 7, a meglévő hívások megtartásával

- A régi `Raycast` változatlanul megmarad (minden réteg, a jelenlegi trigger-szabályok).
- Új `RaycastFiltered`: layer mask, trigger-szűrés, figyelmen kívül hagyott entitás.
- Új `OverlapSphere`: ugyanezzel a szűrővel, hívó által birtokolt találati tömbbel.
- Új `SetEntityEnabled`: deferred művelet; az aktuális callback nem változtatja meg a bejárt listákat.

Az alábbi aláírások megvalósultak. A natív ABI-n csak fix szélességű mezők, pointerek és hívó által birtokolt
buffer mennek át; `std::vector` és engine által foglalt memória nem adható vissza.

```cpp
struct QueryFilter {
    uint32_t layerMask;
    uint32_t flags;           // bit 0: include triggers; többi fenntartott
    uint32_t ignoreEntityId;  // 0: nincs
    uint32_t reserved;        // 0
};
struct SphereOverlapHit {
    uint32_t entityId;        // 0: nem entitás-test, például terep
    uint32_t flags;           // például trigger
};
// A pontos POD RaycastHit és C-ABI adapter a jelenlegi SDK típusait követi.
RaycastHit RaycastFiltered(float ox, float oy, float oz,
    float dx, float dy, float dz, float maxDistance, const QueryFilter* filter);
uint32_t OverlapSphere(float x, float y, float z, float radius,
    const QueryFilter* filter, SphereOverlapHit* output, uint32_t capacity,
    uint32_t* truncated);
void SetEntityEnabled(uint32_t entityId, bool enabled);
```

## Lekérdezési szerződés

- Nem pozitív/nem véges sugár vagy távolság: üres eredmény; nincs részleges world-módosítás.
- A layer mask bitjei a meglévő fizikai rétegek publikus, rögzített mappingjét követik; 0: nincs találat.
- Egy többtestes entitás egyetlen entitás-találatként szerepel. A 0-s tereptalálat külön kezelendő.
- Stabil entity-id sorrendben adunk vissza, maximum `capacity` elemet; a túlcsordulást jelzi a `truncated`.
- A kimenet maximum 1024 találat; ez a kimeneti tömböt korlátozza, a fizikai keresés költségét nem.
- A lekérdezés a legutóbb befejezett fizikai lépés állapotát látja. A még deferred spawn/destroy/enable
  változás csak a commit után jelenik meg.
- Lua/AngelScript kapjon idiomatikus találati listát; a natív adapter tölti ki a hívó tömbjét.

## Enabled jelentése

Egy mesh-entitás `enabled` flagje alapból true, mentődik a jelenetbe és prefab-ba.
A tiltott entitás nem renderel, nem vet árnyékot, nem léptet scriptet, animációt vagy particle-t, nem ad
hangot, és nem szerepel az aktív fizikai világ lekérdezéseiben. Az entitás adatai megmaradnak; visszakapcsolás
nem példányosítja újra a prefab-ot. Az id, transform és paraméterek tiltott állapotban is lekérdezhetők.

Elfogadott döntések:

1. A gyermekek öröklik a szülő tiltását, a saját lokális flagjük megtartásával.
2. Fizika: visszakapcsoláskor visszaáll a tárolt lineáris és szögsebesség.
3. Particle/audio: szüneteltetés; a befejezett one-shot hang nem indul újra.
4. Script: nem vezetünk be automatikusan új `OnEnable`/`OnDisable` hookot ebben az ABI-váltásban;
   az `OnStart` egyszer, az első aktívvá váláskor fut; az `OnDestroy` valódi törléskor fut.

Ez nagyobb runtime-változás, ezért nem oldható meg azzal, hogy csak a rajzolási listából kivesszük a mesh-t.

## Megvalósítás és ellenőrzés

1. SDK ABI 7 és adapter; hibaszöveg az ABI 6 modul elutasításakor.
2. Physics query filter bekötése a már meglévő `PhysicsWorld` filtered raycast/overlap függvényeibe.
3. Lua és AngelScript azonos jelentésű bindingok; a meglévő raycast regressziós tesztje.
4. Enabled adat és deferred commit: render-rekord, spatial index, fizikai test, árnyék-cache, script,
   animátor, audio és particle állapot átmeneteinek együtt kezelése.
5. Teszt: rétegszűrés, trigger, ignore id, üres/korlátos tömb, tiltás-visszakapcsolás, szülő-gyermek
   viselkedés, sebesség megőrzése, ABI-eltérés. Editor Edit/Play és standalone runtime.

A kanonikus és a szállított SDK-header azonos; az ABI 6 elutasítását a ScriptQueryTest ellenőrzi. A konkrét bindingok és korlátok a Client/sdk/README.md fájlban szerepelnek.

