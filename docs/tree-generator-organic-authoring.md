# Organikus fagenerátor és arany vörösfenyő

Az új modell saját procedurális megvalósítás. Nem használ SpeedTree-kódot,
modellt vagy textúrát. A korábbi generátor és presetek kimenete változatlan;
az új ágnövekedést az **Organic growth** kapcsoló engedélyezi.

## Használat

1. Tools → Tree Generator, majd **Golden Larch - Organic** preset.
2. A **Use preset's built-in materials** bekapcsolva betölti a preset kéreg- és
   lombválasztását is. Kikapcsolva megtarthatók az egyedi materialok.
3. Trunk / Level 1 / Level 2 / Level 3: törzs, vázágak, oldalhajtások, finom vesszők.
4. **Parent profiles**: az ág hossza és indulási szöge a szülőn elfoglalt helye szerint.
   **Branch profiles**: vastagság és hajlás az ág tövétől a csúcsáig.
   A görbék öt pontja húzható, alatta számértékekkel is szerkeszthető.
5. **Save growth preset**: külön JSON a projekt `TreePresets` mappájába.
   A lista az eszköz megnyitásakor és a **Reload presets** gombbal frissül.
6. **Save as Asset**: GLB, materialok, textúrák és opcionális távoli impostor.
   A modell mellett `.tree.json` növekedési recept is készül.

## Korona- és lombfinomítás

- **Crown silhouette → Prune to crown envelope**: az organikus ágak növekedését
  elliptikus koronatérfogat határolja. A Radius X/Z a két vízszintes féltengely,
  a Crown base a koronatő magassága a beállított törzshossz arányában, a Center X/Z
  pedig a térfogat oldalirányú eltolása. A koronagörbe alulról felfelé szabja meg
  a sugarat. A törzs megmarad; a levéllapok túlnyúlhatnak az ágak határán.
- A **Trunk / Level ... → Tip foliage length** a szülőág végén is növeszt lombot.
  Így a vezérhajtásnak lehet tűleveles csúcsa akkor is, ha oldalágakat hordoz.
  Nulla kikapcsolja; organikus növekedés, Cards és Align leaves to twigs szükséges.
- **Leaves → Foliage profiles**: a density görbe a levélhordó hajtásszakaszon
  a csomók számát és elhelyezését együtt szabályozza. A nulla sűrűségű részek üresek
  maradnak. A size görbe a teljes hajtás mentén a levéllapok méretét szabályozza;
  nulla méretnél nem készül degenerált levéllap. Align leaves to twigs szükséges.
- **Preview lighting**: napirány, napmagasság, intenzitás, környezeti fény,
  levélen áteresztett fény, expozíció és alpha-kivágott árnyék. Az árnyék iránya
  a napot követi. Ezek előnézeti beállítások; az exportált fa a jelenet fényeit
  használja, a megvilágítás nem része a növekedési receptnek.

A Vulkan szerkesztő sRGB célfelület mellett az előnézeti és bélyegképeket sRGB
textúraként mintavételezi. Így a képek színei nem világosodnak ki egy második
gamma-kódolástól. A lineáris/UNORM célfelület megtartja a korábbi mintavételezést.

A finomított Golden Larch változatosabb ágirányokat, kisebb levélpermeteket,
ritkább belső hajtásszakaszokat és lombos vezérhajtást használ. A koronahatár,
a csúcslomb és a lombgörbék a mentett növekedési recept részei. A korábbi presetek
alapbeállításait ezek az új, alapból kikapcsolt/semleges funkciók nem módosítják.

A növekedési preset a paramétereket és a beépített textúraválasztást tárolja.
Az egyedi materialok GUID-hozzárendelését az exportált modell tárolja.

Az előnézet projekt- vagy assetmentés nélkül is használja a beépített textúrákat.
A **Built-in texture** materialmező ezt jelzi. Projektváltáskor az előző projekt
egyedi material-hozzárendelései törlődnek, a fa paraméterei megmaradnak. A beépített
textúrák teljes útvonalat használnak; betöltési hibánál az előnézet jelzi, hogy
a kéreg vagy a lomb textúrája hiányzik. A preset újbóli alkalmazása újratölti őket.

## Mi változott

| Terület | Működés |
| --- | --- |
| Ágcsatlakozás | Folytonos interpoláció; nem a törzs poligongyűrűihez ugrik. |
| Növekedés | Rögzített, 64 lépéses növekedési pálya, sima véletlen eltéréssel. A háló felbontása nem mozgatja el a hajtásokat vagy a lombot. |
| Ágak felülete | Szállított keresztmetszeti keret, vékony vesszők, lezárt ágcsúcsok. |
| Eloszlás | Véletlen, örvös, spirális és váltakozó oldalhajtások; állítható eloszlási kitevő. |
| Változatosság | Szintenkénti hossz-, vastagság-, szög- és körirányú eltérés. |
| Hierarchia | A szülő méretskálájának öröklése külön állítható. A rövid felső hajtások is viselnek lombot. |
| Lomb | Vesszőhöz igazított síklapok; méterenkénti sűrűség, oldalarány és csúcsméret. |
| Előnézet | Valódi UV-textúra, pixelenkénti alpha-kivágás, mélységi takarás és kivágott lomb által vetett lágyított árnyék. |
| Interaktív méret | Az organikus modell ág-, csúcs- és lombkeretet alkalmaz; a korlát elérését az Output jelzi. |

Az arany vörösfenyő saját eljárással előállított, négy változatot tartalmazó
tűlevél-atlaszt és világosabb kérget használ. Az ágrendszert és a teljes lombot
a fagenerátor hozza létre; az exportot a meglévő GLB- és impostorfolyamat végzi.

## Ellenőrzés

`TreeGeneratorTest`: régi geometriai hash-ek, folytonos csatlakozások,
felbontásfüggetlen lomb, iránykövető levéllapok, görbék, korai végágak,
generálási keret és minden preset mentés/visszatöltés utáni geometriai azonossága.

`TreeAuthoringTest`: textúra/alpha/mélység/árnyék, vörösfenyő részletességi kerete,
három nézet renderelése, kérésre valódi GLB- és impostorexport. Ablak nélküli
ImGui-teszt ellenőrzi a szerkesztő előnézetének feltöltését, gyorsítótárát,
regenerálását és a textúra felszabadítását.
Külön ellenőrzés fedi az Unicode fájlneveket, a munkamappaváltás után is textúrázott
előnézetet, valamint a hiányzó textúrák jelzését és helyreállítását.
A koronametszést, a lombeloszlást, a csúcslombot és a recept visszatöltését
geometriai tesztek ellenőrzik. Az előnézeti tesztek a fényirányt, a környezeti
fényt, az áteresztést, az expozíciót és a gyorsítótár frissülését is vizsgálják.

2026-10-17: a materialbetöltési javítás teljes Release-buildje és 36/36 CTestje
sikeres. A natív szerkesztőben a Golden Larch - Organic előnézete projekt nélkül,
majd új, üres projekt létrehozása után is textúrázott; a fa nem lett assetként mentve.
A felhasználónál látott eredeti szürke állapotot ebben a buildben nem sikerült
reprodukálni; a javítás a relatív útvonalat, a projektváltáskor megmaradó egyedi
materialokat és a néma textúrabetöltési hibát kezeli.
A felhasználó később visszaigazolta, hogy a materialhiba megszűnt.

A korona/lomb bővítés teljes Release-buildje és 36/36 CTestje is sikeres
(`build/tree-refinement-release-build.log`, `build/tree-refinement-ctest.log`).
A Vulkan színtérjavítása külön újrafordítva (`build/tree-refinement-color-build.log`),
natív ablakban ellenőrizve: a fakulás megszűnt, a koronametszés kapcsolója
újragenerálja a fát, az expozíció a geometria újragenerálása nélkül változtatja
a képet. Előtte/utána nézetek és GLB+impostor export: `output/tree-refinement-final`.

Példa egy új, még nem létező kimeneti mappával:

```powershell
build/client-gui-check/libs/render/Release/TreeAuthoringTest.exe --write-preview output/tree-authoring-demo
```

## Jelenlegi határok

Ez a verzió négy szintű procedurális hierarchiát ad görbékkel. Nincs benne
szabad generátorgráf, ágankénti kézi szobrászat, ütközésalapú térkitöltés,
növekedésszimuláció, animált szélrendszer vagy folyamatos geometriát csökkentő LOD.
A távoli megjelenítést a meglévő impostor biztosítja. Az előnézet gyorsítótárazott
CPU-render, alpha-kivágással és irányfénnyel; a motor teljes PBR-megjelenítése ettől
eltérhet. Ez még nem a kereskedelmi SpeedTree teljes funkcionalitása.

Nyilvános fogalmi referenciák: [növekedési görbék](https://docs8.speedtree.com/modeler/doku.php?id=curves_overview),
[hierarchikus modellezés](https://docs8.speedtree.com/modeler/doku.php?id=kcmodelingapproach).
