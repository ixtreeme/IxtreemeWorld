# EXR képek támogatása – 2026-10-09

Az `.exr` fájlok textúraassetként importálhatók és húzhatók az editor megfelelő textúrahelyeire. Az eredeti EXR változatlan marad; az asset felbontása és GUID-ja bekerül az adatbázisba, és külön PNG bélyegkép készül hozzá.

## Felhasználás

| Terület | Feldolgozás |
| --- | --- |
| Sky / Panorama | Lineáris HDR, RGBA16F a GPU-n; equirektanguláris, 2:1 arányú kép ajánlott. A meglévő Exposure és tint működik. |
| Sky / Cubemap | Hat egyforma méretű négyzetes kép; EXR és LDR arcok keverhetők. HDR jelenlétében minden arc lineáris RGBA16F lesz. |
| Statikus modell és natív material | Lineáris RGBA16F, gamma és tónusleképzés nélkül; a meglévő base color, normal és ORM helyeken. |
| Skinned modell textúrája | A meglévő külső diffuse/base color betöltési útvonal EXR-t is olvas, RGBA16F formátumban. |
| Particle sprite | Lineáris RGBA16F; az alpha megmarad. |
| Terrain és vízmaterial | A meglévő RGBA8 textúratömbhöz konvertálódik. Színtextúrán sRGB kódolás, adattextúrán lineáris [0,1] konverzió; itt nincs HDR radiance megőrzés. |
| AO / roughness / metallic csomagolás | Lineáris, normalizált adatként kerül a meglévő 8 bites ORM képbe; nincs gamma vagy exposure. |
| Asset- és faelőnézet | Külön Reinhard tónusleképzés és sRGB konverzió; az alpha lineáris. A faexport levélkivágása EXR alphaadatot is tud olvasni. |

Az editorban importáld a képet, majd húzd a megfelelő material- vagy sky-helyre. Ez az editor nélküli játék buildjében is működik.

## Formátum és korlátok

- Flat, egyetlen képet tartalmazó EXR; scanline és tiled tárolás, HALF/FLOAT adatok. A tiled fájl legfelső felbontási szintje kerül beolvasásra.
- Gyökérszintű `R`, `G`, `B`, opcionális `A`; hiányzó alpha = 1. A `Y` luminance csatorna RGB-re másolódik.
- A tárolt színértékek lineárisként kerülnek feldolgozásra. Nincs automatikus ACES/színtér-konverzió vagy alpha-unpremultiply.
- Deep, multipart, rétegzett nevű színcsatornák és alámintavételezett csatornák nem támogatottak. Ezeket flat RGB/RGBA EXR-re kell exportálni; az import érthető hibát ad.
- Maximum 16384 pixel bármely tengelyen és 33 554 432 pixel összesen; a dekódolt RGBA float puffert 512 MiB-ra korlátozzuk. A meglévő sky korlát ezután panorámánál 4096 pixel szélességre, cube-arcnál 2048 pixelre csökkenti a képet.
- A GPU half-float tartománya ±65504. A nem véges értékek nullára cserélődnek; a normál HDR fényességek 1 felett is megmaradnak.
- EXR nem szabványos glTF textúraformátum. A motor saját betöltője tudja olvasni, más glTF nézők számára PNG/JPEG vagy megfelelő szabványos textúraformátum szükséges.

## Implementáció és ellenőrzés

A közös dekóder: `Client/libs/asset/ExrImage.{h,cpp}`. Az OpenEXR könyvtár a projekt vcpkg/CMake függősége; a dekóder memóriából is működik, ezért az asset reader útvonalai használhatják. Képkockánként nem végez új dekódolást.

`ExrImageTest`: HALF/FLOAT, scanline/tiled, ZIP/PIZ/DWAA, eltolt data window, RGB/Y/alpha, hibás adatok, HDR GPU-payload, preview/adat/szín konverzió, assetimport, bélyegkép, manifest újratöltés és ORM csomagolás.

Eredmények: 41 EXR-ellenőrzés sikeres; editor Debug és runtime Release CTest 9/9. Az editor és a runtime Debug/Release buildje elkészült. RTX 2060-on a runtime EXR panorámával, hatlapos EXR cubemappal és EXR modelltextúrával is elindult és renderelt. A Debug runtime Vulkan sync-validációs futásai nem jeleztek VUID vagy SYNC-HAZARD hibát. A tesztprojekt külön, a `Client/build/exr-audit-20261009` mappában van.

A könyvtári beolvasás és frame buffer használatának forrása: [OpenEXR dokumentáció](https://openexr.com/en/latest/ReadingAndWritingImageFiles.html).
