# Smart Material rendszer: meglévő alapok és hiányzó feladatok

## Frissítés — 2026. október 12.

A későbbi mérföldkövek már közös UI-független gráfot/IR-t, kezelt Layer Stack-et,
külön menthető GUID-alapú instance-okat és típusos subgraph asseteket adnak. A
statikus mesh geometry baker tényleges AO/curvature/cavity/thickness és object/world
pozíció/normál térképeket készít, paddinggel, megszakítható háttérmunkával és ellenőrzött
GUID-cache-sel. A mesh-atlas preview és a Weathered Stone objektumhoz kötött native
material export is működik.

Hat szerkeszthető `.ixsmartgenerator` preset épül ugyanarra a backend-re: Edge Wear,
Dirt Accumulation, Rust Spread, Moss Growth, Dust Accumulation és Water Streaks.
Menthetők, külön hívásonként paraméterezhetők és rétegmaszkként hozzárendelhetők.
Használat, teszt- és mérési részletek: `SMART_MATERIAL_GENERATORS.md`,
`SMART_MATERIAL_GEOMETRY.md`, `SMART_MATERIAL_TESTS.md` és
`SMART_MATERIAL_PERFORMANCE.md`.

Az Asset Browser felismeri és importálja a smart material formátumokat, külön
kategóriákkal és mentés utáni azonnali frissítéssel. A gráfok, subgraphok és
generátorok dupla kattintással megnyithatók, a mentetlen gráf védelmével.
A teljes smart material egyetlen **Bake & Apply to selected model** művelettel
beköthető a kiválasztott statikus modell Material Slotjába: az aktuális modellmátrix,
automatikus geometry bake, mentett natív anyag és slotbeállítás ugyanabban a folyamatban
van, a közben módosult modell/transzformáció/anyaghely védelmével.

2026. október 14.: a 7. fázis statikus modellekre elkészült. A közzétett anyag gráfja
SPIR-V shaderré fordul (a fordítás a szerkesztőben, közzétételkor történik, a játék
csak a kész shadert tölti be), a renderer pixelenként ezzel rajzolja, a sült textúrák
maradnak tartaléknak, a paraméterek pedig a Material Graph panelből élőben
állíthatók. A gráf szerkezeti módosítása (node, kapcsolat, réteg, textúra) a
háttérben újrafordul ugyanabba a csomagba, új Bake & Apply nélkül. Részletek:
`SMART_MATERIAL_IMPLEMENTATION.md`, 5. mérföldkő; GPU-költség:
`SMART_MATERIAL_PERFORMANCE.md`. Ugyanazon a napon a gráf Scene node-okat kapott
(Time, Scene Depth, Sky Reflection, Noise Normal), a renderer pedig nézetenkénti
környezet-setet (idő, mélységmásolat, égbolt); ezekből épül a Templates > Water
sablon.

Az Asset Browser katalógus/preview thumbnail, a jelenetkijelöléshez kötött 3D mesh
preview, terrain/skinned bake, valamint a skinned modellek és a terrain élő
gráf-shadere továbbra is hátravan. Az alábbi eredeti állapotfelmérés a
2026. október 11-i helyzetet rögzíti; a friss státuszhoz a fenti dokumentumokat használd.

## Korábbi állapotfelmérés

Állapotfelmérés: 2026. október 11., a jelenlegi motor forráskódja alapján.

A tervezett Smart Material újrahasználható, réteges felületi anyag: például egy kő alapanyagára automatikusan moha kerül a felfelé néző részeken, nedvesség az alján, kosz a mélyedésekben. Ugyanez terrainen füvet, sziklát és sarat keverhet. Az alábbi cél egy közös anyagrendszer terrainhez és modellekhez; az első megvalósítás terrainre szűkíthető. A felsorolt új funkciók tervek, még nincsenek megépítve.

## Kiválasztott node editor

A felhasználó választása alapján a materialgráf szerkesztőfelületének alapja a [thedmd/imgui-node-editor](https://github.com/thedmd/imgui-node-editor). A könyvtár MIT licencű; a források beépítésekor meg kell őrizni a [licencszöveget és a szerzői jogi értesítést](https://github.com/thedmd/imgui-node-editor/blob/master/LICENSE).

A node editor csak az editor build függősége legyen. A materialgráf adatai, típusellenőrzése, mentése és shadergenerálása külön motoroldali rétegbe kerüljenek, ImGui- és node-editor-típusok nélkül. A játék a lefordított materialt és annak paramétereit használja; a szerkesztőfelület nem fut a játékban.

Az első integrációs mérföldkő a dokkolható Material Graph panel és néhány összeköthető mintanode. Ez a szerkesztő alapja; a Smart Material kiértékeléséhez az alábbi motoroldali feladatok is szükségesek.

### Beépített első mérföldkő

Az imgui-node-editor `021aa0ea4da13fed864bafb2a92d4c5205076866` verziója helyi, editor-only függőségként bekerült. Az ImGui 1.92.8 kompatibilitását verziófüggő vektoroperátor biztosítja. A **View > Material Graph** panel kereshető, kategorizált node-készletet, hat csatlakozótípust, ciklusellenőrzést, Inspector-paramétereket, Undo/Redo-t és verziózott `.materialgraph` mentést/betöltést ad.

A további beépítés működő Scalar/Color/Vector és parameter node-okat, UV/Tiling Offset/World Position/Normal node-okat, Texture Sample/Triplanar mintavételezést, matematikai és színműveleteket, seeded Noise/Height/Slope maszkokat, Material Layer/Layer Blend/Height Blend/Normal Blend és teljes PBR Output kiértékelést ad. Az `IXMaterialGraph` könyvtár UI-független dokumentumot, topológiai kiértékelőt, HLSL-generálást és UV-sütést tartalmaz; a runtime build is használhatja. A vendored `crude_json` külön ebben fordul, az ImGui/node editor továbbra is csak editorfüggőség.

Az Inspector élő anyag- és csatornaelőnézetet, külön instance-paramétereket, fájl-/assettextúra-bekötést ad. A cache a pozíció-/névváltozásokat kihagyja, csúszkázáskor összevonja az előnézeti frissítéseket; hibánál megtartja az utolsó sikeres képet. A **Templates** rozsdás fém és mohás kő mintákat ad. A csak UV-tól függő gráf a meglévő material rendszerbe süthető, teljes textúrakészlettel és ORM-csomagolással. A world/geometry-függő gráfok objektum-/terrain-sütése és a közvetlen dinamikus GPU-gráf-hozzárendelés még nincs megépítve. Az AO/Curvature/Cavity bemenetek külső felületi adatot igényelnek; nem helyettesítenek geometriai bakert. Az exposed instance-paraméterek jelenleg preview/bake munkamenetértékek, külön menthető runtime Smart Material instance asset még nincs.

## Ami már megvan

| Meglévő alap | Mire használható |
| --- | --- |
| Material assetek és GUID-alapú textúrahivatkozások | Az új rendszer használhatja a meglévő anyagokat rétegforrásként. |
| Albedo, normal, AO, roughness, metallic, emissive adatmezők | A felületi tulajdonságok alapja; a rendererek támogatása még nem egységes. |
| ORM-csomagolás | AO, roughness és metallic egy textúrába kerülhet; ezt nem kell újra megírni. |
| Nyolc terrainréteg és két RGBA splatmap | A rétegek jelenlegi súlyozása és kézi festése újrahasználható. |
| Terrain triplanar vetítés, mipmapek, kis súlyú rétegek kihagyása | A vetítés és a teljesítménykezelés használható alapja. |
| Material Inspector, textúrabekötés, materialváltozások követése | Bővíthető réteglistával, generátorokkal és maszkelőnézettel. |
| Objektumonkénti materialparaméter-felülírások | Részleges alap a Smart Material példányokhoz. |

**Lényeges különbség:** a jelenlegi terrainrétegek súlyai festett splatmapekből származnak. A triplanar slope threshold a textúravetítést váltja, nem választ automatikusan füvet vagy sziklát. A Height textúra importálható és menthető, de a jelenlegi materialshaderek még nem olvassák.

## Mi hiányzik a működő rendszerhez?

### 1. Smart Material asset és adatmodell

- [ ] Verziózott assetformátum; javasolt külön `.smartmaterial` asset.
- [ ] Rétegek: alapanyag-hivatkozás, név, sorrend, engedélyezés, fedés, csatornánkénti hatás, vetítési beállítások.
- [ ] Egy réteg módosíthasson csak roughnesst vagy normált is: a nedvességhez például nem szükséges új albedo.
- [ ] Mentett maszk- és generátorbeállítások, stabil azonosítók, hiányzó források biztonságos alapértékei.
- [ ] Smart Material példány: közös alapanyagból objektumonként vagy terrainenként eltérő moha-, kosz- és nedvességérték, a forrásasset módosítása nélkül.
- [ ] Függőségek, gyorsítótárak és újraimport kezelése; a régi `.material` assetek továbbra is működjenek.

Javasolt közös fogalmak: `SmartMaterialAsset`, `SmartMaterialLayer`, `MaskGenerator`, `SmartMaterialInstance`. Az editor, a mentés és a runtime ugyanazt az adatmodellt használja.

### 2. Automatikus maszkgenerátorok

- [ ] Geometriai meredekség és felületirány: szikla a meredek oldalakon, hó vagy moha a felső részeken.
- [ ] Világ- vagy objektumtérbeli magasság, méterben értelmezhető határokkal és lágy átmenettel.
- [ ] Noise és nagy léptékű szín-/maszkvariáció, állítható mérettel és mentett seed értékkel.
- [ ] Importált maszk, csatornaválasztás, invertálás, kontraszt és fedés.
- [ ] Maszkok kombinálása: szorzás, összeadás, minimum/maximum, illetve kivonás.
- [ ] Kézzel festett korrekció és védett területek, amelyek automatikus újrageneráláskor is megmaradnak.

A generátorokhoz a geometriai normált kell használni: a normal map apró részletei ne változtassák meg az egész terület fű/szikla besorolását. A terrain magassága és az anyag saját Height textúrája külön bemenet.

Későbbi generátorok: curvature/élkopás, cavity/mélyedések, sült AO-alapú kosz, nedvesség helyi zónákból vagy a közeli Water Bodyból. A vízhez kötött maszk a tényleges vízterületet és annak saját szintjét vegye figyelembe.

### 3. Rétegkeverés a shaderekben

- [ ] Közös felületi kiértékelés: albedo, normal, roughness, metallic, AO és emissive kimenetek.
- [ ] Rétegenkénti fedés és prioritás, egyértelmű keverési szabályokkal és biztonságos alapréteggel.
- [ ] Height alapú átmenet: a sár vagy homok először a kövek közötti mélyebb részeket fedje.
- [ ] A Height textúrák tényleges GPU-bekötése és mintavételezése, skála/bias/átmeneti szélesség paraméterekkel.
- [ ] Rétegzett normálokhoz megfelelő keverés; közös koordinátatér és helyes triplanar normáltranszformáció.
- [ ] Terrain- és static mesh-integráció; a skinned mesh későbbi bővítésként ugyanazt az anyagkimenetet használja.
- [ ] Következetes materialkiértékelés a fő képben, víztükröződésben és egyéb releváns renderpassokban. Ha a rétegek alfát is módosítanak, a shadow/depth/alpha-mask útvonalaknak is ezt kell követniük.

A Height alapú keverés nem jelent geometriai displacementet. Parallax/POM és a tényleges geometriai eltolás külön, későbbi feladat lehet.

### 4. Terrainhez szükséges kiegészítések

- [ ] Automatikus alaprétegsúlyok számítása a meglévő nyolc réteghez.
- [ ] Az automatikus súlyok és a kézi festési szándék külön tárolása. A jelenlegi splatmap önmagában nem jelzi, melyik súlyt kell védeni az újragenerálástól.
- [ ] Sculpt után csak az érintett chunkok és a szabályokhoz szükséges szomszédos terület frissítése.
- [ ] Varratmentes maszkok chunkhatárokon, mentett seed és azonos koordinátaértelmezés.
- [ ] Újragenerálás, kézi korrekció törlése, helyi frissítés és Undo/Redo az Inspectorból.

Javasolt megoldás: a magasság/lejtő/noise alapmaszkok gyorsítótárazása vagy sütése, majd kézi korrekció hozzákeverése. Például a kézi fedés súlyozhatja az automatikus és a festett rétegsúlyokat, amelyek utána normalizálódnak. Így a terep átformálható anélkül, hogy elveszne egy megfestett ösvény.

### 5. Modellekhez szükséges kiegészítések

- [ ] UV-, objektumtér- és világtér-alapú vetítés választása, valamint triplanar támogatás a megfelelő mesh-anyagútvonalon.
- [ ] A méret, forgatás és nem egyenletes scale helyes kezelése; mozgatott objektumnál választható, hogy az anyag az objektumhoz vagy a világhoz kötődjön.
- [ ] Imported/baked maszkok bekötése; később curvature, cavity és AO generálása vagy importálása.
- [ ] Vertex color vagy mesh-maszk festési útvonal, ha modellekre is kell kézi korrekció. A jelenlegi vizsgált mesh shaderekben nincs ilyen színmaszk-bemenet.
- [ ] Materialslotonkénti hozzárendelés és példányparaméterek megőrzése prefabokban és jelenetekben.
- [ ] Smart Material módosításakor a hozzá tartozó LOD/imposztor sütések és előnézetek érvénytelenítése.

Egyszerű magasság-/irány-/noise-alapú mesh-anyaghoz még nem szükséges saját curvature baker. A hiteles élkopáshoz és mélyedésmaszkokhoz később objektumhoz tartozó felületi adatok kellenek.

### 6. Szerkesztő és használhatóság

- [ ] Réteglista az Inspectorban: hozzáadás, másolás, törlés, sorrend, ki-/bekapcsolás.
- [ ] Rétegenkénti textúrabekötés és generátorparaméterek, drag-and-drop támogatással.
- [ ] Maszk külön megjelenítése, réteg izolálása, automatikus/kézi eredmény összehasonlítása.
- [ ] Anyagelőnézet és Scene View frissítés, Undo/Redo, mentés, másolható presetek.
- [ ] Induló presetek: füves-sziklás terrain, sáros part, mohás kő, koszos beton, rozsdás fém.
- [ ] Generálási állapot, hibás assethivatkozások és a várható renderköltség látható jelzése.
- [x] Az imgui-node-editor beépítése külön editorfüggőségként, rögzített forrásverzióval és megőrzött MIT licenccel.
- [x] Dokkolható Material Graph panel, kereshető node-létrehozás, típus szerint jelölt csatlakozók és hibás összekötések elutasítása.
- [x] Saját, stabil azonosítójú materialgráf-vázlat, mentés/betöltés és Undo/Redo; a node-ok helyzete mentett szerkesztőadat, a nézet navigációja külön marad.
- [x] Kezdő node-készlet: Parameter, Texture Sample, Noise, Height Mask, Slope Mask, Layer Blend és PBR Output.
- [x] Gráfellenőrzés, ciklusok felismerése és node-azonosítóval jelölt HLSL-generálás; tesztben DXIL és SPIR-V fordítás.
- [ ] Dinamikus GPU-shader-hozzárendelés és fordítási hibák visszajelölése a vászon node-jára.
- [x] Élő kiértékelt materialelőnézet és kimeneti csatornák; hibánál az utolsó helyes előnézet marad.
- [ ] Jelenetbeli GPU-materialcsere, az utolsó sikeresen lefordított shader megőrzésével.

A kiválasztott node editor a materialgráf felülete; az Inspector a kijelölt node vagy materialpéldány paramétereit szerkeszti. A réteglista használható egyszerűbb, előre meghatározott réteges anyagokhoz. A felületek ugyanazokat a motoroldali materialadatokat és mentési útvonalakat használják.

### 7. Teljesítmény és textúrafeldolgozás

- [ ] Az aktív rétegek és generátorok korlátozása; kiinduló cél a kevés, jellemzően 2–4 érdemi réteg pixelenként.
- [ ] Statikus maszkok sütése/cache-elése; a ritkán változó curvature vagy terrainmaszk ne számolódjon újra minden pixelben minden képkockán.
- [ ] Távolságfüggő részletesség és fokozatos átmenetek; a LOD ne okozzon látható materialváltást.
- [ ] Mesh-textúrák teljes mipmaplánca. A vizsgált `StaticMeshRenderer::UploadTexture` jelenleg egy mip szintet tölt fel.
- [ ] Szemantikának megfelelő színtér és mipgenerálás: színtextúrákhoz sRGB-kezelés, adatmaszkokhoz lineáris mintavétel, normálokhoz megfelelő szűrés.
- [ ] Textúrák megosztása, tömörítés, VRAM-korlátok, shader-változatok és fordítási cache.
- [ ] Opcionális ORM+Height csomagolás vizsgálata: a meglévő ORM színcsatornái mellé kerülhet Height az alfába, ha a felbontás és a minőség ezt megengedi.
- [ ] GPU-idő mérése azonos kameránál és jelenetnél, külön a fő kép és a tükröződés költségével. A teljesítménycélt mérés alapján kell rögzíteni; pusztán LOD-tól nem várható ingyenes rétegkeverés.

### 8. Mentés, csomagolás és ellenőrzés

- [ ] Smart Materialok, példányok, generátorok és kézi maszkok mentése, verzióváltás és régi jelenetek betöltése.
- [ ] Az összes forrásmaterial, textúra és sütött maszk bekerül a kiadott játékba, projektből feloldható hivatkozásokkal.
- [ ] Materialmódosítás és újraimport csak a szükséges cache-eket érvénytelenítse; ne épüljön újra az egész terrain vagy assetkönyvtár.
- [ ] Tesztjelenet dombbal, sziklafallal, parttal, mozgatott és átméretezett kövekkel, valamint LOD/imposztor példányokkal.
- [ ] Ellenőrzés: stabil maszkok, lágy átmenetek, varratmentes chunkhatárok, megmaradó kézi festés, helyes normálok, mentés/betöltés, runtime/editor egyezés és mért GPU-költség.

## Javasolt megépítési sorrend

| Szakasz | Konkrét eredmény |
| --- | --- |
| 0. Node editor integrációs próba | imgui-node-editor az editor buildben, dokkolható Material Graph panel és mintanode-ok; ImGui-kompatibilitás és gráfnézet teljesítményének ellenőrzése. |
| 1. Terrain automatizálás | Magasság + lejtő + noise alapján fű/szikla/sár, menthető szabályokkal, cache-elt súlyokkal és védett kézi festéssel. |
| 2. Smart rétegkeverés és materialgráf | Smart Material asset, Height alapú átmenetek, rétegenkénti csatornaszabályok, az első működő materialnode-ok és shadergenerálás, Inspector és maszkelőnézet. |
| 3. Static mesh anyagok | Ugyanazok az assetek köveken, falakon és tárgyakon; példányparaméterek, vetítés, mipmapek, teljesítményellenőrzés. |
| 4. Kopás és környezeti hatások | Curvature/cavity/AO maszkok, élkopás, kosz, moha, helyi nedvesség; szükség szerinti baker és mesh-festés. |
| 5. További szerkesztési és runtime funkciók | Összetettebb node-ok és újrahasználható részgráfok, skinned mesh bővítés, export/sütés egyszerű materialba, opcionális parallax vagy displacement. |

A teljesítmény, a mentés és a runtime támogatás minden szakasz része. Az első szakasz még automatikus terrainanyag; a második hozza létre a Smart Material asset és a réteges shader közös alapját.

## A felmérés fő forrásai

- [MaterialAsset és a Height jelenlegi státusza](../Client/libs/asset/MaterialAssetManager.h#L13).
- [ORM-csomagolás](../Client/libs/asset/MaterialAssetManager.cpp#L327).
- [MaterialData](../Client/libs/asset/AssetLibrary.h#L52), [terrainrétegek](../Client/libs/render/MapEditorTypes.h#L64), [objektumfelülírások](../Client/libs/render/MapEditorTypes.h#L444).
- [Terrain shader](../Client/shaders/Terrain.hlsl#L431): splatmapes súlyozás, triplanar és PBR-keverés.
- [Terrain mipfeltöltés](../Client/libs/render/TerrainRenderer.cpp#L1005), [static mesh textúrafeltöltés](../Client/libs/render/StaticMeshRenderer.cpp#L3095).
- [Static mesh shader](../Client/shaders/StaticMesh.hlsl#L136) és [skinned mesh shader](../Client/shaders/SkinnedMesh.hlsl#L116): a material- és megvilágítási útvonalak egységesítése külön figyelmet igényel.
