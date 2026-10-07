# Editor teljesítményellenőrzés – 2026-10-07

Claude félbemaradt ellenőrzését folytattam. A forrásban megtaláltam az üres Scene View képgyorsítótárát, az ImGui viewport DPI-lekérdezésének gyorsítótárát és a `SceneManager::HasPendingScene()` őrfeltételét. A korábbi transcript szerint Claude már eltávolította a saját mérőkódját, és befejezte a Release buildet és a négy regisztrált tesztet. A végső futtatás és mérés maradt hátra.

## Javított hiba

A Scene View gyorsítótára csak azt vizsgálta, hogy van-e projekt. Az editor projekt nélkül is enged új jelenetet létrehozni, így a nyitott jelenet képe változatlan kamerával nem frissült. A gyorsítótár használatát arra szűkítettem, amikor **sem projekt, sem nyitott jelenet nincs**.

A reprodukciót és a javítást az editor tényleges renderhurkában, ideiglenes számlálókkal ellenőriztem. Ugyanaz a teszt új jelenetet hozott létre projekt nélkül, bezárta, forgatta a kamerát, majd ismét létrehozott egy jelenetet. A régi feltétel külön tesztmódban reprodukálható volt.

| Stabil, nyitott jelenetes mérési ablak | Régi feltétel: újrarajzolás / frame | Javított feltétel: újrarajzolás / frame |
| --- | ---: | ---: |
| Első jelenet, 4. másodperces minta | 0 / 3160 | 3075 / 3075 |
| Első jelenet, 5. másodperces minta | 0 / 3147 | 3071 / 3071 |
| Második jelenet, 10. másodperces minta | 0 / 3241 | 3068 / 3068 |
| Második jelenet, 11. másodperces minta | 0 / 3189 | 3069 / 3069 |

Jelenetbezárás után visszatért a gyorsítótár használata. Kameraforgatáskor újrarajzolt a Scene View, a mozgás megállítása után ismét elmaradt a fölösleges rajzolás. A függő jelenet minden váltásnál feldolgozásra került (`pending=0`). A számláló a renderelés ütemezését igazolja; a képpontok és a részecskeanimáció vizuális ellenőrzését nem helyettesíti.

## Összehasonlító FPS-mérés

Ugyanaz a Release futtatható fájl, ablakméret és editor-elrendezés futott. A swapchain 2560×1369, a Scene View 1510×832 pixel volt. Projekt és nyitott jelenet nélkül mértem. Minden futás 12 másodperces; az első két, egyenként egy másodperces mintát kihagytam, így futásonként 9, összesen 18 stabil minta jutott egy változatra.

A kontrollváltozatban **csak az üres Scene View gyorsítótára** volt kikapcsolva. A DPI-gyorsítótár és a SceneData-őrfeltétel mindkét változatban működött. Ez az összehasonlítás nem a teljes Claude előtti állapotot méri.

| Változat | Első futás átlag FPS | Második futás átlag FPS | Összesített átlag FPS | Átlag CPU-frame |
| --- | ---: | ---: | ---: | ---: |
| Scene View gyorsítótár nélkül | 3741 | 3650 | 3696 | 0,268 ms |
| Javított Scene View gyorsítótárral | 4419 | 4361 | 4390 | 0,226 ms |

A gyorsítótár mért hozzájárulása **+18,8% FPS**, illetve **−15,7% CPU-frame idő**. A mérést másodpercenként egyszer naplózó ideiglenes kód végezte. Claude korábbi, eltérő időpontban készített műszerezett naplója 48 mintából 3935 FPS átlagot mutatott; ezt nem kevertem az új összehasonlítás mintáihoz.

## További célzott próbák

Az üres editorban a present hívás maradt a legnagyobb CPU-tétel: a korábbi profilokban körülbelül 0,11 ms/frame, miközben a teljes CPU-frame körülbelül 0,226 ms. Két rövid, azonos környezetű próba a platform viewport-frissítését csak több viewport esetén engedélyezte; az eredmény nem javult, ezért ezt a módosítást nem tartottam meg. Az átlagok 4,40 kFPS körül maradtak, a különbség a futások zaján belül volt.

A `MAILBOX` present módot is kipróbáltam az alapértelmezett `IMMEDIATE` előtt. Két futás 4,44 kFPS körüli átlagot adott, ami nem jobb az `IMMEDIATE` kontrollnál, ezért az eredeti választási sorrend maradt. A háttérben futó present-kísérletet elvetettem: a számlált iterációk mesterségesen 38–110 kFPS-re ugrottak, miközben ez nem jelentett ugyanilyen tényleges képernyő-frissítést.

Az eldobható present-scheduler most alapértelmezett útvonal lett. A legfeljebb két függő presentet kezelő worker-szál minden normál indításkor elindul; a `--sync-present` kapcsoló vagy az `IX_ASYNC_PRESENT=0` környezeti változó bármikor kikapcsolja, az `IX_ASYNC_PRESENT=1` és a `--async-present` pedig kifejezetten bekapcsolja. A scheduler a semaphore-t és a swapchain image-et megtartja a present befejezéséig, resize és leállítás előtt kiüríti a sort, a present hibáját pedig a következő `BeginFrame` dolgozza fel. A próba 5,3–5,4 kFPS render-loopot adott a 4,58 kFPS kontrollhoz képest. A queue-limit 4-re emelése nem hozott további javulást.

PowerShellből az async útvonal így indítható, ugyanabban a folyamatindításban:

```powershell
& 'D:\IxtreemeWorld\Client\build\apps\client\Release\IxtreemeEngine.exe' --async-present
```

A naplóban normál indításkor a `[VULKAN] async present scheduler enabled (default, ...)` sornak kell megjelennie. Ha a Vulkan-sor helyett `async present scheduler disabled` látszik, a mérés továbbra is a körülbelül 4 kFPS-es szinkron útvonalat mutatja.

A Scene View bal felső sarkában külön `Scene render: ... FPS` mérő jelenik meg. Ez csak a tényleges offscreen Scene View pass lezárásait számolja; a cache-elt üres képet, illetve a rejtett vagy másik dock-tab mögötti Scene View-t nem tekinti új Scene View frame-nek. Emiatt az üres, stabil nézetben a mérő akár 0 FPS-t is mutathat, miközben a panel továbbra is helyesen megjelenik.

A Game View saját overlayt kapott: `FPS`, processz-`CPU`, rendszer-`RAM` százalék és a folyamat working-set memóriája, per-process `GPU` engine-terhelés, valamint a `Vulkan / <device>` renderer neve látható a képen. A CPU-érték a meglévő processz-mintavétel, a RAM a `GlobalMemoryStatusEx`, a folyamat memória a working set, a GPU pedig a Windows PDH `GPU Engine(*)\\Utilization Percentage` számlálóinak folyamat-PID szerinti összege, 100%-ra korlátozva. A gépen a folyamat GPU-engine példányai ténylegesen elérhetők voltak; ha egy driver nem adja őket, a mező `n/a`.

Ez a mód a present host-oldali blokkolását választja le; az ImGui panelépítés és a játék-szimuláció továbbra is ugyanazon az alkalmazási szálon fut. A render-loop FPS ezért nem azonos a ténylegesen megjelenített FPS-sel. A mód kapcsolható és visszavonható a kapcsoló elhagyásával, de alapértelmezetté csak külön, valós present-FPS és késleltetés-méréssel lenne célszerű tenni.

## Végleges build és ellenőrzések

Az ideiglenes kódot eltávolítottam, majd a forrás időbélyegét frissítve ténylegesen újrafordítottam az alkalmazást. Az `EngineApplication.cpp` újrafordítása szerepel a végleges Release és Debug buildnaplóban. Az audit környezetiváltozó és naplójelölő hiányzik a végleges forrásból, a `[CODEX-AUDIT]` jelölő a Release binárisból is hiányzik.

- Teljes Release build: sikeres.
- Teljes Debug build: sikeres.
- Release CTest: **4/4 sikeres**.
- Debug CTest: **4/4 sikeres**.
- Regisztrált tesztek: `IXRHISmoke`, `SceneLayerGroundTest`, `SceneLayerAuthoringTest`, `SceneWorldPackageTest`.
- Végleges, mérőkód nélküli Release: 8 másodperces indítási teszt, szabályos bezárás, **exit=0**.
- Vulkan-validáció: a loaderrel bekapcsolt `VK_LAYER_KHRONOS_validation` mellett a jelenetváltási/kameramozgatási körben és a végleges mérőkód nélküli Release indítási tesztjében is **0 bájtos hiba- és figyelmeztetésnapló**. A stderr igazolja a validációs réteg tényleges betöltését.
- `git diff --check`: sikeres.

Az opcionális runtime RmlUi dokumentumok (`assets/ui/*.rml`) hiánya mindkét változatnál azonos indítási figyelmeztetést okozott. A napló ezeket opcionálisként azonosítja; az editor elindult és szabályosan bezárult.

## Korlátok

A Computer Use eszköz a kernel assetek írásakor elakadt (`failed to write kernel assets`, Windows error 3), reset után is. Ezért a tényleges képpontok, a leválasztott ablakok és az eltérő DPI-jű monitorok vizuális ellenőrzése nem történt meg.

A végleges, mérőkód nélküli bináris külső FPS-mérését xperf/ETW segítségével is megpróbáltam. A Windows a recorder indítását hozzáférési hibával elutasította (`0x5`). Recorder session nem indult; a tesztpéldányt leállítottam. **A 4390 FPS tehát a műszerezett mérés eredménye**, míg a végleges bináris működését és validációját külön ellenőriztem.

A betöltött, nagy projekt god-ray/részecske terhelése, a panel elrejtése/visszanyitása és a monitorok közötti DPI-váltás külön tesztet igényel. Ezekre nem állítok mért eredményt.

## Bizonyítékok

A munkakönyvtáron belül: `Client/build/codex-editor-audit-20261007/`.

- `benchmark-summary.json`: összesített mérés.
- `benchmark-*/summary.json`, `samples.json`, `ixtreeme_engine.log`: futásonkénti minták.
- `regression-before/` és `regression-after/`: a hiba reprodukciója és a javított renderütemezés.
- `validation-1-mode3/`: jelenetváltási validáció.
- `final-release-smoke/` és `final-validation-smoke/`: végleges bináris futtatása.
- `scheduler-baseline/`, `scheduler-async/`, `scheduler-async-q4/`: a kapcsolható present-scheduler FPS-próbái.
- `async-smoke/` és `async-validation/`: a workeres útvonal indítási és validation próbái.
- `final-release-build.log`, `final-debug-build.log`, `final-release-tests.log`, `final-debug-tests.log`: végleges ellenőrzések.
- `EngineApplication.instrumented.cpp`: a méréshez használt ideiglenes forrás archivált példánya.
- `EngineApplication.final.cpp`: a mérőkód nélküli forrás mentett példánya.
- `Run-Audit.ps1`: az ideiglenesen műszerezett bináris futtatója; a végleges binárishoz nem használható automatikus időkorlátként.

A módosításokat nem commitoltam. Claude egyéb változtatásait megőriztem.
