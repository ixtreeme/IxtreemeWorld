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

A növekedési preset a paramétereket és a beépített textúraválasztást tárolja.
Az egyedi materialok GUID-hozzárendelését az exportált modell tárolja.

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
