# FBX materialok, textúrák és lenyitható modellassetek

Az editor FBX-importja az eredeti modell mellett megadott relatív textúraútvonalakat is feloldja. A beágyazott képeket kibontja, a külső képeket a projektbe másolja, és natív `.material` asseteket készít GUID-alapú textúrahivatkozásokkal. A modell materialslotjai ezeket használják.

## Használat

1. Importáld az eredeti FBX-et az Asset Browserbe. A külső textúrák maradjanak elérhetők az eredeti export/letöltés mappájában.
2. A modell automatikusan kinyílik. Később a csempe bal oldalán található nyíllal nyitható és csukható.
3. A materialok és textúrák közvetlenül a modell után jelennek meg, több sorba tördelve, ha szükséges. Ezek valódi assetek: kijelölhetők, húzhatók, a material dupla kattintással szerkeszthető.
4. Hiányzó textúráknál az import állapotsora figyelmeztet; a modell fölé vitt egérrel láthatók a hiányzó hivatkozások. Az importjelentés újranyitás, editorból átnevezés és áthelyezés után is megmarad.

Korábbi, hibás textúrahivatkozásokkal importált modellhez importáld újra **az eredeti FBX-et**, abba a projektmappába, ahol a modell található. Egy önmagában átmásolt FBX nem tartalmazza a külső textúrák képadatát.

## Működés és korlátok

- Fájlelrendezés: `<modell>.fbx`, `<modell>.fbx.meta`, `<modell>.fbx.import.json`, `<modell>_materials/*.material`, `<modell>_textures/*`. A lenyitott lista GUID-hivatkozásokat követ; a materialok és képek külön projektassetek maradnak.
- A közös textúra az import során egyszer kerül feldolgozásra. Azonos tartalmú ismételt import újrahasználja a fájlokat és a GUID-okat. Azonos nevű, eltérő képek külön fájlnevet és GUID-ot kapnak.
- Meglévő módosított képet vagy materialt az import nem ír felül: külön fájl/verzió készül, és a modell alapértelmezett materialhivatkozásai az új import eredményére frissülnek. Korábban explicit hozzárendelt materialok továbbra is a saját GUID-jukat követik.
- Base color/diffuse, normal, emissive, AO, roughness, metallic és height/displacement hivatkozások importálhatók. A külön AO/roughness/metallic képekből lineáris RGB ORM készül, a megadott PBR szorzók megőrzésével. A bump/height nem lesz tévesen normal map.
- A motor által támogatott további hivatkozott képek is bekerülnek a modell listájába, de a motor shaderének nincs minden FBX/DCC materialfunkcióra megfelelője. A height map tárolva van; jelenleg nem ad automatikus displacementet. Az import nem konvertálja tetszőleges procedurális DCC shaderek kinézetét.
- A keresés először konkrét útvonalakat használ. Ha csak fájlnév alapján lehet keresni a forráscsomagban, két azonos nevű jelölt közül nem találomra választ: hiányzó hivatkozásként jelzi.
- A lenyitható listák gyorsítótárazottak. Materialmódosítás csak a modell tartalmának gyorsítótárát érvényteleníti; a böngésző mappalistáit nem szkenneli újra minden materialelőnézetnél.

## Ellenőrzés

`FbxImportTest`: valódi ASCII és bináris FBX-fixture-ök, relatív és beágyazott textúrák, közös képek, névütközés, ismételt import, módosított assetek védelme, Maya PBR-mapek és ORM-csatornák, hiányzó hivatkozások, átnevezés/áthelyezés, read-only projektújranyitás az eredeti forrás nélkül. A fixture-export javítja az Assimp 6 tesztgenerátorának üres külső Texture-útvonalait; az engine importerének bemenete szabályos FBX.

Futtatás: `ctest --test-dir Client/build -C Debug --output-on-failure` és `ctest --test-dir Client/build-runtime -C Release --output-on-failure`.

Eredmény: editor Debug és runtime Release CTest **10/10**, az új FBX-teszt **41/41**. Az editor és a runtime Debug/Release fordítása elkészült. Négy FBX-modell renderpróbája sikeres: a materialslotok automatikusan betöltődtek, a base color, normal és ORM képek a projektből töltődtek be. A Debug Vulkan-próba naplóiban nem volt VUID/validációs hiba. A böngésző lenyitásának egérinterakcióját automatizált GUI-teszttel nem ellenőriztem.
