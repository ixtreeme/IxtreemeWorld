# IxtreemeEngine – magyar használati útmutató

Az útmutató a Help > Editor Guide / Súgó ablakban offline is elérhető. A menük és mezők neve az editorban látható angol elnevezést követi. A kereső a magyar és az angol szövegben is keres; az ékezetek elhagyhatók.

## Kezdőlépések

<!-- topic: quick-start -->
### Első projekt öt perc alatt

#### Mire jó?
Egy menthető jelenetet készítesz, majd kipróbálod a játék módot. Az Asset Browser a fájlokat, a Hierarchy a jelenetben lévő példányokat, az Inspector a kijelölt elem beállításait mutatja.

#### Rövid tutorial
1. File > New Project...: adj nevet és válassz projektmappát. A projekt létrehozása kezdő jelenetet is készít.
2. Create > Cube: helyezz el egy egyszerű tárgyat. A Hierarchy listában jelöld ki.
3. Az Inspector > Transform alatt állítsd a Position Y értéket 1-re. A Scene View-ban az F a kijelölésre irányítja a kamerát.
4. Ctrl+S: ments. F5: indítsd a Play módot, majd F5-tel állítsd le.

#### Eredmény és gyakori hiba
Van egy saját projekted és egy kockát tartalmazó jeleneted. Ha a létrehozás szürke, előbb nyiss jelenetet, és állítsd le a Play módot.

<!-- topic: projects-scenes -->
### Projektek, jelenetek és mentés

#### Mire jó?
A projekt összefogja az asseteket, jeleneteket és beállításokat. Egy jelenet az objektumokat, terrain-t, fényeket és környezeti beállításokat tárolja.

- File > Open Project... és Recent Projects: projekt megnyitása.
- File > New Scene / Open Scene / Recent Scenes: jelenetváltás.
- Ctrl+S: projekt és jelenet mentése; Ctrl+Shift+S: Save Scene As....
- A Hierarchy jelenetcsomópontjai és az Asset Browser jelenetfájljai segítik a váltást.

#### Rövid tutorial
1. Nyiss projektet, majd Ctrl+N-nel készíts jelenetet.
2. File > Save Scene As... alatt adj saját nevet.
3. Helyezz el egy Sphere-t, és Ctrl+S-sel ments.
4. File > Open Scene alatt válts másik jelenetre, majd nyisd újra a sajátodat.

#### Eredmény és gyakori hiba
A mentett objektum visszatér. Jelenetváltás előtt kezeld a mentési kérdést; Play közbeni módosításokra ne építs tartós szerkesztést.

<!-- topic: layout -->
### Ablakok, dokkolás és a súgó

#### Mire jó?
A View menüből előhívhatod a Hierarchy, Inspector, Scene Settings, Asset Browser, Scripts, Console, Build Output, Animator és Statistics paneleket, illetve a Scene View / Game View nézetet.

#### Rövid tutorial
1. A fül fejlécét húzd egy dokkolási célra, vagy engedd el külön ablakként.
2. View > Game View segítségével hozd elő a játék kameráját. Külön ablakba húzva a két nézet egyszerre látható.
3. View > Reset Layout visszaállítja a megszokott elrendezést.
4. Help > Editor Guide / Súgó: válassz Magyar vagy English nyelvet, majd keress például a „festés” szóra.

#### Eredmény és gyakori hiba
A panelek saját görgetéssel használhatók. Az útmutató címei, kiemelései és számozott lépései tagolják a szöveget; a Leírás másolása gomb a teljes témát másolja. Egy inaktív viewport-fül nem renderel új képet.

<!-- topic: navigation-transform -->
### Navigáció, kijelölés és transzformáció

#### Mire jó?
A Scene View szabad kamerával szerkeszthető. A kijelölt modell kontúrt kap; az üres tér nem a régi nagy befoglaló doboz alapján választ objektumot.

- Jobb egérgomb + WASD: repülő kamera. F: kijelölés keretezése.
- W / E / R: Move / Rotate / Scale eszköz, amikor nem kamerát mozgatsz és nem szöveget írsz.
- Transform: Position méterben, Rotation fokban, Scale tengelyenként.
- Az eszköztár mágnes ikonja lépésközre illeszt; Delete törli a kijelölést.

#### Rövid tutorial
1. Jelölj ki egy kockát a Hierarchy-ban, majd nyomj F-et.
2. W-vel mozgasd a tengelynyilat; E-vel forgasd, R-rel méretezd.
3. Kapcsold be a mágnest, válassz 1.0 lépést, és próbáld újra a mozgatást.
4. Pontos értékhez írj közvetlenül az Inspector mezőibe, majd ments.

#### Eredmény és gyakori hiba
Az objektum új helyen áll. Ha az importált modell túl nagy, ellenőrizd a forrás mértékegységét; centiméteres modellnél gyakran 0.01 Scale kell.

<!-- topic: hierarchy -->
### Hierarchy, szülők, láthatóság és példányok

#### Mire jó?
A Hierarchy a jelenet objektumait rendezi. Kereséskor a szükséges szülőágak is megmaradnak; azonos nevű tárgyak külön példányok maradnak.

#### Rövid tutorial
1. A + gombbal vagy Create menüből adj hozzá Cube, Sphere, Capsule, Point Light, Spot Light, Water Body vagy Terrain elemet.
2. Jobb kattintással keresd a Rename, Duplicate és Delete műveletet.
3. Húzd egyik objektum sorát a másikra az átszülőzéshez. Ellenőrizd a transzformációt az új hierarchiában.
4. A szem ikonnal változtasd az editoros láthatóságot. Az Enabled állapot a működést szabályozza; a két fogalom különbözik.
5. Írj a keresőbe, majd jelöld ki az eredményt és nyomj F-et.

#### Eredmény és gyakori hiba
A példányok átláthatóan rendezhetők. Törlés előtt ellenőrizd a kijelölést és a gyerekeket; az assetfájl és a jelenetpéldány eltérő elem.

<!-- topic: play-cameras -->
### Play, Pause és a két kamera

#### Mire jó?
A Scene View szerkesztőkamerája és a Game View játék-kamerája külön nézet. Play alatt fut a fizika, script, audio, karaktervezérlés és animáció.

#### Rövid tutorial
1. Edit módban állítsd össze és mentsd a jelenetet.
2. F5 vagy a Play gomb: indítás. Az eszköztár zöld jelzése mutatja az aktív játék módot.
3. F6: Pause / Resume; Shift+F5: Stop.
4. Stop után ellenőrizd, hogy a szerkesztési állapot tért vissza.

#### Eredmény és gyakori hiba
A játék ideiglenes állapotát teszteled. **A Play közbeni változtatások Stop után eldobódnak.** A Game a projekt Main Camera nézetét használja, illetve aktív játékos Character Controller mellett annak kameráját. A Scene View szabadkamerájának mozgatása önmagában nem állítja át a játék kameráját.

## Assetek és materialok

<!-- topic: asset-browser -->
### Asset Browser és fájlkezelés

#### Mire jó?
A projekt erőforrásait importálod, rendezheted és használhatod. A Create menü mappát, Lua/AngelScript scriptet, Material, Water Material, Physics Material, Animator Controller és Particle Effect assetet készít.

#### Rövid tutorial
1. Válassz mappát, majd Create > New Folder segítségével készíts Models és Materials almappát.
2. Az Asset Browser importgombjával vagy a helyi menü Import Asset... műveletével válassz fájlt.
3. A modellt húzd a Scene View-ba vagy a Hierarchy megfelelő céljára.
4. Dupla kattintással nyisd meg a szerkeszthető assetet; jobb kattintással éred el a típusának megfelelő műveleteket.

#### Eredmény és gyakori hiba
Az asset a projektben, a példánya a jelenetben található. A modell melletti nyíl alatt a materialok és textúrák is böngészhetők. Átnevezéshez és törléshez az editor műveleteit használd, hogy a kapcsolódó metaadatok kezelése is megtörténjen.

<!-- topic: model-import -->
### FBX és más modellek importja

#### Mire jó?
A modellimport geometriát, támogatott materialadatokat, hivatkozott textúrákat és animációs tartalmat hoz be. Az FBX-hez natív materialok és textúrafüggőségek készülhetnek.

#### Rövid tutorial
1. Tartsd elérhető helyen az FBX által hivatkozott képeket, majd importáld a modellt.
2. Az Asset Browser modell melletti nyilával nyisd le a tartalmát.
3. Húzd be a modellt a jelenetbe. Az Inspector > Material Slots alatt ellenőrizd a slotokat.
4. Egy materialra duplán kattintva nyisd meg a Material Editort, és ellenőrizd a Diffuse / Albedo és Normal Map képeket.
5. Szükség esetén korrigáld a Scale-t, majd ments.

#### Eredmény és gyakori hiba
A modell saját materialokkal látható. Ha a forrásprogram speciális shaderét nem lehet megfeleltetni, a natív materialt kézzel pontosítsd. Hiányzó textúránál a forrás FBX hivatkozásait és az importnaplót ellenőrizd.

<!-- topic: textures-exr -->
### Textúrák, normal map és EXR

#### Mire jó?
A textúrák a material felületét adják. A Diffuse / Albedo szín, a Normal Map részletes felületi irány, az AO árnyékolási tényező; a Roughness és Metallic más-más fizikai tulajdonság.

#### Rövid tutorial
1. Importáld a felülethez tartozó képeket; a rendszer EXR képet is kezel.
2. Nyiss Materialt, és húzd a képeket a megfelelő textúramezők előnézetére.
3. A Normal Strength csúszkával ellenőrizd a domborulatot, majd a Roughness értékével a fényességet.
4. Save után próbáld ki különböző fényirányból.

#### Eredmény és gyakori hiba
A material textúraelőnézeteket mutat. EXR-nél a lebegőpontos HDR adatok kezelhetők; a kis előnézet megjelenítési kép, nem a tárolt dinamikatartomány mérése. Ne cseréld fel a színképet a roughness/normal adattal; hibás normal-irány esetén ellenőrizd a forrás konvencióját.

<!-- topic: materials -->
### Material Editor, PBR és material override

#### Mire jó?
A közös Material Editor Material és Water fület tartalmaz. A PBR material közös asset; az objektum Material override-ja csak az adott példányhoz tartozik.

- Textures: Diffuse / Albedo, Normal Map, AO, Roughness, Metallic, Height.
- Parameters: tiling, tint, normal/AO/roughness/metallic erősség és emissive beállítások.
- Shading Mode: Lit vagy Unlit; Alpha Mode: Opaque, Mask vagy Blend.
- Mask a levélhez hasonló kivágott felületekhez; Blend valódi áttetszőséghez.

#### Rövid tutorial
1. Asset Browser > Create > New Material; duplán kattints rá.
2. Adj albedo textúrát, és állítsd a tilingot.
3. Save, majd húzd a materialt egy modell Material Slots mezőjére.
4. Egyetlen példány eltéréséhez válaszd ki a megfelelő Material Slotot, és kapcsold be az Override Active jelölést.

#### Eredmény és gyakori hiba
A közös material szerkesztése minden használóján változtat; az override csak a példányon. Az Inspector slotjára dupla kattintás közvetlenül megnyitja a materialt.

A Material Slots kisebb kártyákat mutat az Inspector szélességéhez igazodó rácsban, a material nevével és albedo-előnézetével; albedotextúra nélkül az alapszín látszik. Az egérrel rámutatva a teljes nevet és a submesh sorszámát is láthatod. GUID-sor nincs itt. Materialt az előnézetre vagy névre húzva rendelhetsz hozzá; az X kiüríti a slotot.

<!-- topic: prefabs -->
### Prefab készítése és frissítése

#### Mire jó?
A prefab újrahasználható objektum-összeállítás; több példány követheti ugyanazt az assetet, saját eltérésekkel.

#### Rövid tutorial
1. Edit módban állíts össze egy modellt komponensekkel vagy gyerekobjektumokkal.
2. A Hierarchy helyi menüjében válaszd a Create Prefab műveletet.
3. A prefab assetet húzd a jelenetbe többször.
4. A példány Inspectorában az Open a prefab forrását nyitja meg, a Select kijelöli azt az Asset Browserben.
5. Az Overrides menü eltérésenként Apply/Revert, illetve Apply All/Revert All műveletet kínál. A More... menüben a példány vagy az összes kapcsolt példány frissítése és az Unpack található.

#### Eredmény és gyakori hiba
Közös sablonból több példány készül. Apply előtt ellenőrizd, hogy az összes használóra szánt változtatást adod át. A prefab asset dupla kattintásával az asset saját szerkesztőfelülete nyílik meg.

- A prefab asset Inspectorában a név, entitáshierarchia/transzformáció és materialslotok szerkeszthetők; Add Point Light / Add Spot Light lámpát ad hozzá.
- Save Prefab Asset menti az asset szerkesztését; Reload Prefab Asset eldobja a még nem mentett változtatásokat. Instantiate Prefab jelenetpéldányt készít. Szülőciklus vagy hiányzó mesh-hivatkozás blokkolja a mentést.
- A prefab készítése és példányosítása megőrzi a LOD Group beállításait, a kikapcsolt csoportokét is. Az Overrides menü külön is alkalmazhatja vagy visszavonhatja a LOD Group eltéréseit.

<!-- topic: model-export -->
### Modell exportálása FBX-be

#### Mire jó?
A támogatott modellassetet külső modellezőprogramban is felhasználhatod. Ez modell-export; nem azonos a játék teljes csomagolásával.

#### Rövid tutorial
1. Az Asset Browserben válassz modellassetet.
2. Jobb kattintás > Export to FBX....
3. A megnyíló exportablakban ellenőrizd az elérhető opciókat és válassz célfájlt.
4. Nyisd meg az exportot a célprogramban, és ellenőrizd a méretet, materialokat és orientációt.

#### Eredmény és gyakori hiba
Külön FBX fájlt kapsz. A jelenet példányainak transzformációi, scriptek és editorfunkciók nem válnak automatikusan külső alkalmazásban futó játékká. A teljes futtatható játékhoz a File > Build Game... kell.

## Terrain és víz

<!-- topic: terrain-create -->
### Terrain létrehozása és felbontása

#### Mire jó?
A terrain szerkeszthető magasságrács és materialrétegek együttese. A cellaméret határozza meg a geometria sűrűségét; a textúrák tilingja ettől független.

#### Rövid tutorial
1. Nyiss jelenetet, majd Create > Terrain.... Meglévő terrainnél Replace Terrain... jelenik meg.
2. Állítsd be a méretet és cellafelbontást a létrehozóablakban.
3. Jelöld ki a terrain-t a Hierarchy-ban; az Inspector Sculpt, Paint és Settings fület mutat.
4. Készíts egy kis dombot és teszteld közelről, mielőtt nagy területet építesz.

#### Eredmény és gyakori hiba
Menthető terrain áll rendelkezésre. Kisebb cellák több geometriát és munkát jelentenek. A Replace Terrain a meglévő terepet helyettesíti; készíts mentett másolatot, ha a régi területre még szükséged lesz.

- Settings > Save Terrain menti a magasság- és festési adatokat; Ctrl+S a projektet/jelenetet is menti.
- Reload From Disk eldobja a még nem mentett terrain-változtatásokat. A Sculpt/Paint/Settings lapok váltása leteszi az aktív ecsetet.

<!-- topic: terrain-sculpt -->
### Terrain formázása: Sculpt

#### Mire jó?
Raise emel, Lower süllyeszt, Smooth kisimít, Flatten megadott Target height magassághoz igazít.

- Brush radius: méret méterben; Strength: módosítás erőssége.
- Falloff: 0 keményebb, 1 lágyabb ecsetszél.
- A bal egérgomb húzása a Scene View-ban formáz; az aktív eszközre újra kattintva kikapcsolod.

#### Rövid tutorial
1. Terrain > Sculpt > Raise; válassz közepes sugarat és alacsony Strength értéket.
2. Rövid húzásokkal építs dombot.
3. Smooth segítségével simítsd a lejtőt.
4. Flatten és Target height használatával készíts egy építési padot, majd Ctrl+S.

#### Eredmény és gyakori hiba
A magasság és a megjelenített geometria változik. Nagy ecsettel könnyű távoli részeket is módosítani; a textúrafestés nem változtat a magasságon.

<!-- topic: terrain-paint -->
### Terrain festése: három ecsetcsúszka

#### Mire jó?
A Paint fül nyolc materialréteget kezel előnézetekkel. Egy festési mód van; a korábbi Mix/Replace választót három csúszka váltja ki.

- **Diameter:** a teljes ecsetátmérő méterben.
- **Edge smoothing:** a perem átmenetének lágysága; a geometriai/textúrarács minimális élsimítása kis értéknél is megmarad.
- **Overall coverage:** 100% a belső részen lecseréli a korábbi réteget; kisebb érték kever.

#### Rövid tutorial
1. Húzz két Materialt vagy textúrát két réteghelyre, majd válaszd a másodikat.
2. Diameter 8 m, Edge smoothing 10%, Overall coverage 100%: fess egy jól fedő sávot.
3. Edge smoothing 80%-kal próbálj lágy átmenetet.
4. Overall coverage 40%-kal kevert felületet készíts. Undo Last Stroke visszavonja az utolsó húzást; Stop painting leállítja az eszközt.

#### Eredmény és gyakori hiba
A perem és a belső fedés külön állítható. Egy helyen tartott ecset nem halmoz korlátlan fedést; meglévő erősebb fedés megmaradhat. Ne az ecset sebességével próbáld szabályozni az átfedést.

<!-- topic: terrain-mapping -->
### Terrain rétegparaméterek és triplanar mapping

#### Mire jó?
A réteg tiling, tint, normal, roughness, metallic, AO, UV offset és rotation értékei a festett felület kinézetét szabályozzák. A triplanar vetítés a meredek oldalakon csökkenti a textúra nyúlását.

#### Rövid tutorial
1. Terrain > Paint alatt válassz réteget, és a Tiling értékével állítsd a minta méretét.
2. Egy meredekre formázott dombnál nyisd a Settings fület, és kapcsold be a Triplanar mapping jelölést.
3. Sharpness a vetítési irányok keverését, Slope threshold és Slope transition a lejtő szerinti átmenetet szabályozza.
4. Közelről és távolról is ellenőrizd, majd ments.

#### Eredmény és gyakori hiba
Kevésbé nyúlik a felület a lejtőn. A nagy tiling sűrű mintát ad, nem növeli a terrain geometriai felbontását. A rétegparaméter és a közös materialasset módosítása eltérő hatókörű.

<!-- topic: water-body -->
### Water Body és a víz alakjának festése

#### Mire jó?
A Water Body önálló jelenetobjektum, saját szinttel, területtel és materiallal. A Shape ecset a víz jelenlétét adja hozzá vagy távolítja el.

#### Rövid tutorial
1. Create > Water Body, majd jelöld ki az új vízobjektumot.
2. Állítsd be a középpontot, kiterjedést és vízszintet az Inspectorban.
3. A Shape alatt válaszd az Add vagy Remove eszközt és a Brush radius értékét.
4. Bal egérgombbal fess a Scene View-ban; az eszközre újra kattintva állítsd le.
5. Rendelj Water Materialt, és ments.

#### Eredmény és gyakori hiba
Tó vagy szabálytalan vízfelület készül. A víz alakja és a terrain magassága külön adat: a medret Sculpt-tal kell kialakítani. Ha nem látszik a víz, ellenőrizd a szintet, Enabled állapotot és a kamera helyét.

<!-- topic: water-material -->
### Water Material: szín, hullám, hab és fénytörés

#### Mire jó?
A Material Editor > Water fül ugyanabban az ablakban szerkeszti a vízmaterialt. A Colors mélységi színeket és fade-et, Wave a kis/nagy hullámokat, Foam a habot szabályozza.

- Caustic: a vízalatti fényminta módja és megjelenése.
- Refraction / Reflection: fénytörés, mélységi hatás és tükröződés.
- Edge Fade: a part közelében történő eltűnés távolsága és görbéje.
- Textures és Normal Tiling: a hullámfelület részletei; Fresnel a nézési szög szerinti reflexió.

#### Rövid tutorial
1. Asset Browser > Create > New Water Material, majd nyisd meg.
2. Válassz sekély és mély színt; mérsékelt Wave Scale és Speed értékeket használj.
3. Kapcsold be a Foam-ot, és ellenőrizd a partnál.
4. Save, majd rendeld egy Water Bodyhoz.

#### Eredmény és gyakori hiba
Ugyanaz a material több vízobjektumon használható. Túl nagy normal/hullám/refraction érték torz képet adhat; a tükröződés külön GPU-költséget jelent.

## Objektumok és fizika

<!-- topic: components -->
### Inspector és komponensek

#### Mire jó?
Az Inspector mutatja a kijelölt objektum beépített Transform, renderer, material és animációs szakaszait. Az Add Component a támogatott Collider/Trigger alakzatokat, Rigidbody, Fixed/Hinge Joint, Character Controller, Script, Audio Source/Listener, Particle System, LOD Group és Note komponenseket kínálja.

#### Rövid tutorial
1. Jelölj ki egy Cube-ot; az Inspector alján nyisd az Add Component menüt.
2. Keress a Collider szóra és add hozzá.
3. A komponens fejlécének menüjéből nézd meg a rendelkezésre álló eltávolítási műveletet.
4. Note komponensben hagyj szerkesztői megjegyzést, majd ments.

#### Eredmény és gyakori hiba
Csak az objektumtípuson használható, még hiányzó komponensek adhatók hozzá. A Note szerkesztői információ. Az Inspector hibakeresési ikonja extra azonosítókat és runtime adatokat fedhet fel, nem a szokásos tartalomkészítéshez szükséges.

<!-- topic: lights -->
### Point Light, Spot Light és napfény

#### Mire jó?
A Point Light minden irányba világít; a Spot Light kúpot vetít, ezért forgatható. A Sun a Scene Settings > Environment globális irányfénye.

#### Rövid tutorial
1. Create > Point Light; emeld a föld fölé, és állítsd a Color, Intensity és Radius értékeket.
2. Egy második teszthez Create > Spot Light; a Rotation segítségével fordítsd egy tárgyra.
3. A spot sugár és belső/külső kúpszögek meghatározzák a világított területet.
4. Scene Settings > Environment alatt állítsd a Sun irányát, magasságát és a Casts shadows jelölést.

#### Eredmény és gyakori hiba
A point lámpát nem kell lefelé/felfelé forgatni: nincs kitüntetett iránya. Ha egy oldala sötétnek látszik, vizsgáld a felületnormált, takarást és materialt. Sok helyi fény és árnyék növelheti a renderköltséget.

<!-- topic: camera -->
### Main Camera és látótér

#### Mire jó?
A Camera objektum pozíciója, forgatása és vetítése határozza meg a Game View képét, ha nincs aktív játékosvezérlő kamera.

#### Rövid tutorial
1. Jelöld ki a Main Camera objektumot a Hierarchy-ban. Új jelenet létrehozásakor automatikusan elkészül a kezdő kamera.
2. Az Inspector Transform alatt állítsd az irányt és helyet.
3. Camera > Field of View: látószög; Near Plane / Far Plane: közeli és távoli vágás.
4. Set as Main Camera választja ki a játék fő kameráját; nyisd a Game View-t az ellenőrzéshez.

#### Eredmény és gyakori hiba
Más a Scene View és a Game képe, mert két kamerát használnak. Túl nagy Near Plane levágja a közeli modellt; túl kicsi Near és túl nagy Far ronthatja a mélységi pontosságot.

<!-- topic: colliders -->
### Collider, trigger és Physics Material

#### Mire jó?
A Collider adja a fizikai ütközési alakot. Az Is Trigger érintkezést jelez, de nem szilárd akadály. A Physics Material a súrlódás és rugalmasság közös beállítása.

- Shape: Box, Sphere, Capsule, Mesh és Convex Hull. A Mesh a modell háromszögeit követi; főleg statikus/kinematic testhez használd. A Convex Hull mozgó dinamikus testhez alkalmas.
- A Fit műveletek a Box/Sphere/Capsule méretét a mesh-hez igazítják; Center alaphelyzetbe állítja az eltolást. Edit Collider In Scene az objektum transzformációjától külön mozgatja a collider középpontját.
- Physics Presets Default/Bouncy/Ice/Heavy/Trigger az érintkezést és a kapcsolódó Rigidbody értékeit állítja. Preset után ellenőrizd a mezőket.

#### Rövid tutorial
1. Adj Collider-t egy kockához. Válassz hozzá illő Shape-et, és állítsd a méretet/középpontot.
2. Edit Collider In Scene segítségével ellenőrizd az alakot.
3. Válassz Physics Layer-t; a rétegpárok ütközését az Edit > Project Settings határozza meg.
4. Create > New Physics Material az Asset Browserben: Friction, Bounciness és a Combine módok beállítása után rendeld a colliderhez.
5. Play alatt próbálj ütközni a tárggyal; triggerhez az Is Trigger jelölést kapcsold be.

#### Eredmény és gyakori hiba
A látható mesh és a collider alakja különbözhet. Hibás layer-párnál nem történik ütközés; a NoCollision réteg szándékosan ütközés nélküli.

<!-- topic: rigidbody -->
### Rigidbody és jelenetfizika

#### Mire jó?
Static rögzített test, Dynamic szimulált test, Kinematic vezérelt mozgás. A Mass, gravity, damping, sleeping, Continuous Collision és tengelyrögzítések a viselkedést szabályozzák.

#### Rövid tutorial
1. Készíts collideres talajt és egy felette lévő collideres kockát.
2. A felső kockához adj Rigidbody-t, válaszd a Dynamic típust és Use Gravity-t.
3. Scene Settings > Physics: induláshoz Gravity 0, -9.81, 0; a Fixed timestep a fizika lépésideje, Max substeps a pótló lépések korlátja.
4. Play alatt figyeld az esést. Live physics (Play mode) alatt az impulzusműveletekkel tesztelhetsz.

#### Eredmény és gyakori hiba
A test a collideres talajon megáll. A Mass csak dynamic testre hat. Gyors kis testeknél Continuous Collision segíthet az áthaladás ellen; a túl sok substep vagy túl sűrű mesh-collider költséges.

<!-- topic: joints -->
### Fixed Joint és Hinge Joint

#### Mire jó?
A Fixed Joint két Rigidbody kapcsolatát rögzíti, a Hinge Joint tengely körüli forgást enged. Egy kapcsolat komponensét csak az egyik testre add.

#### Rövid tutorial
1. Készíts két collideres, Rigidbody-val rendelkező mesh-t.
2. Az egyikhez adj Hinge Jointot, a Connected Entity ID mezőbe írd a másik test azonosítóját.
3. Állítsd az Anchor és Axis értékeket, majd Limits mellett a Min Angle / Max Angle értéket.
4. Friction Torque-kal csillapítsd a forgást, és Play alatt teszteld.
5. Merev kapcsolathoz Fixed Jointot használj a hinge helyett.

#### Eredmény és gyakori hiba
Ajtószerű vagy merev kapcsolat jön létre. A nulla, hiányzó vagy Rigidbody nélküli cél nem megfelelő. Az azonosító megjelenítéséhez használhatod az Inspector hibakeresési nézetét.

<!-- topic: character-controller -->
### Játékos és Character Controller

#### Mire jó?
A Character Controller a kiválasztott mesh-t játékosként kezeli, kapszulás mozgással és saját kamera móddal.

#### Rövid tutorial
1. Egy karaktermodellhez add a Character Controller komponenst, és legyen bekapcsolva.
2. Állítsd be a Capsule Radius / Height méretet, Walk Speed, Run Speed és Jump Height értékeket.
3. Slope Limit és Step Height szabályozza a járható lejtőt és lépcsőt.
4. Válassz First Person, Third Person vagy Top Down Camera Mode-ot; állítsd a kapcsolódó magasság/távolság mezőket.
5. Play: WASD mozgat, Shift fut, Space ugrik; a Game View-ban jobb gombos húzás körbenéz.

#### Eredmény és gyakori hiba
Irányítható játékos készül. A kapszula mérete feleljen meg a modell méterben mért méretének. Elakadásnál ellenőrizd a talaj colliderét, lépcsőmagasságot és physics layer kapcsolatot.

## Animáció, effektek és hang

<!-- topic: animation-clips -->
### Animált modellek és klipek

#### Mire jó?
A skinned modell csontváza és az animációs klip együtt adja a mozgást. Az Animation (debug) közvetlen klippróba, az Animator Controller pedig állapotvezérlés.

#### Rövid tutorial
1. Importálj csontvázas modellt és hozzá illő animációt.
2. Húzd a modellt a jelenetbe és jelöld ki.
3. Az Inspector Animation (debug) > Clip mezőben válassz animációt; ellenőrizd a mozgást.
4. Tartós játéklogikához rendelj Controller assetet az Animator részben.

#### Eredmény és gyakori hiba
Az aktuális animált alak renderelődik és kap kijelölési kontúrt. Ha a klip nem illik a csontvázhoz vagy a modell csak statikus mesh-ként importálódott, nem lesz megfelelő mozgás. A debug-klip és a controller eltérő tesztelési út.

<!-- topic: animator -->
### Animator állapotgép és átmenetek

#### Mire jó?
Az Animator Controller állapotok, klipek, paraméterek és átmenetek grafikonja. Entry jelöli a belépést, Any State több állapotból elérhető átmenetet ad; Float, Bool és Trigger paraméter használható.

#### Rövid tutorial
1. Asset Browser > Create > New Animator Controller; nyisd meg az Animator panelen.
2. A vászon helyi menüjével Add State Here, majd rendelj klipet. Jobb kattintás > Set as Default választ kezdőállapotot.
3. Jobb egérgombbal húzd az egyik állapotot a másikra az átmenet létrehozásához. Bal kattintással válaszd ki az él közepét a feltételek szerkesztéséhez.
4. Has Exit Time, exit time és Duration az átmenet időzítését szabályozza. A Parameters részen adj például Speed Floatot.
5. Rendeld a controllert a modellhez; Play alatt az aktív állapotot figyeld.

#### Eredmény és gyakori hiba
A paraméterváltozás vezérli a klipek közötti váltást. A névnek egyeznie kell a scriptből állított paraméterével. Rename/Delete és Delete Transition a megfelelő node/él helyi menüjében található.

<!-- topic: blend-trees -->
### Animator blend tree-k

#### Mire jó?
A blend tree paraméterekből több mozgást kever; például állás, séta és futás folyamatosan változó Speed alapján. 1D küszöbértékeket, 2D pozíciókat használ.

#### Rövid tutorial
1. Válassz Animator-állapotot és annak blend tree beállítását.
2. Válassz 1D módot és egy Float paramétert.
3. Add Motion: adj Idle, Walk és Run klipet növekvő threshold értékekkel.
4. A paramétert változtatva Play alatt ellenőrizd a keverést.
5. Kétirányú mozgáshoz a támogatott 2D módot, két paramétert és gyermekpozíciókat állíts be.

#### Eredmény és gyakori hiba
A klipek fokozatosan keverednek. Azonos csontvázhoz illő animációkat használj, és ne adj minden motionnek azonos küszöböt. A gyermeklistában a + Add Motion és X gombok szerkesztik a készletet.

<!-- topic: particles -->
### Particle System és Particle Effect asset

#### Mire jó?
Füst, szikra és hasonló effektek készíthetők emitterrel. Az Emission, Lifetime, Speed, Size, Blending, Texture, Shape, Color, Collision és Soft Particles más-más részét szabályozza; GPU Simulation választható.

- Duration, Rate (/s), Burst és Max Particles a kibocsátást és korlátját adja; a Min/Max mezők életkort, sebességet és méretet véletlenítenek.
- Húzd a Size Over Life görbe négy pontját; Color 0–3 az élettartam alatti szín/átlátszóság változása. Gravity, Drag és Spin a mozgást alakítja.
- A Turbulence sima, kavargó mozgást ad CPU- és GPU-szimulációban is. Az Enable Turbulence kapcsolja be; Strength a gyorsulás erőssége, Scale az örvények térbeli mérete méterben, Speed a sodródási tér időbeli változását állítja (0 = állandó áramlási tér). A beállítások jelenetben, prefabban és effektpresetben is mentődnek.
- Atlas Columns/Rows élettartam alatt lejátszott flipbookot ad. Shape: Point/Sphere/Box/Circle/Edge; Direction/Cone Angle irányítja a kibocsátást.
- Local Space mellett a részecskék követik az emittert, de nincs talajütközés. Soft Distance szabályozza a geometria közeli elhalványulást.

#### Rövid tutorial
1. Adj Particle System komponenst egy mesh-példányhoz.
   Láthatatlan emitterhez válaszd a Create > Empty Object menüpontot (a Hierarchy + gombjánál és helyi menüjében is elérhető), majd az Inspector > Add Component > Particle System elemet. Az üres objektumnak nincs modellje, materialja vagy collidere, a Scale kezdetben 0/0/0; mozgatás és forgatás közben is megmarad. A részecskék méretét és kibocsátási térfogatát a Particle System beállításai adják.
2. Válassz Texture-t, mérsékelt kibocsátást és rövid élettartamot; állítsd a Shape és Color részt.
3. GPU Simulation mellett és nélküle is nézd meg az eredményt. Collide With Ground talajütközést ad; Soft Particles lágyítja a felületmetszést.
4. Save As New Effect menti a beállított emittert újrahasználható assetként; válaszd az Effect Preset mezőben. Re-apply Preset újratölti az értékeit.
5. Play On Start és Loop segítségével állítsd az indulást és ismétlődést.

#### Eredmény és gyakori hiba
Az emitter Edit módban is előnézhető. Nagy, áttetsző részecskék egymásra rajzolása GPU-költséges; a sok élő részecske számát az élettartam is növeli.

Lebegő porhoz helyezz Box emittert a talaj fölé (az Extents félméreteket jelent), állíts Cone Angle 180°-ot, Gravity 0-t, Speed Min/Max 0.02/0.08-at, Drag 0.3-at és Lifetime 15/25 másodpercet. Kapcsold be a Turbulence-t Strength 0.03, Scale 3 és Speed 0.35 értékekkel. Kis Alpha sprite-okat használj, a Color 0–3 alfaértékei legyenek 0/0.15/0.15/0, a Size Over Life pedig állandó. Az új részecskék a teljes térfogatban születnek, a meglévők kisodródhatnak belőle. Burst 0 és Rate 15 mellett fokozatosan telik meg.

<!-- topic: audio -->
### Audio Source, Listener és hangerőbuszok

#### Mire jó?
Az Audio Source klipet játszik, az Audio Listener a hallgatási helyet adja. Master, Music és SFX buszokkal közös hangerő állítható.

#### Rövid tutorial
1. Importálj hangassetet; az Asset Browser helyi menü Play Preview műveletével ellenőrizd.
2. Adj Audio Source-t egy objektumhoz és válassz Clipet.
3. Enabled, Play On Start, Loop, Volume és Pitch után válassz Bus-t.
4. 3D hanghoz kapcsold be a 3D-t és állítsd a Min Distance / Max Distance értéket; legyen megfelelő Audio Listener.
5. Edit > Project Settings > Audio alatt állítsd a buszokat, majd Play-ben tesztelj.

#### Eredmény és gyakori hiba
A hang helyhez és távolsághoz köthető. Néma eredménynél ellenőrizd a klipet, a listener állapotát, a source és busz hangerőt, illetve az indulási jelölést.

Audio Listener komponens hiányában a kamera adja a hallgatási helyet.

## Scriptek és játékfelület

<!-- topic: scripts -->
### Lua és AngelScript: első script

#### Mire jó?
A projekt scriptjei Play alatt futnak. Lua és AngelScript szerkesztéséhez nem kell a motor C++ buildje; a Scripts panelből megnyithatók a fájlok.

#### Rövid tutorial
1. Asset Browser > Create > New Lua Script vagy New AngelScript.
2. Nyisd meg a generált sablont; az OnStart, OnUpdate és OnDestroy életciklus-hookokat ebből kiindulva szerkeszd.
3. Húzd a scriptassetet a mesh Hierarchy-sorára, vagy Add Component > Script alatt válassz Backend és scriptet. AngelScriptnél a Class mezőt is ellenőrizd.
4. A megjelenített scriptmezőket az Inspectorban állíthatod.
5. Play alatt ments egy módosítást: a Lua/AngelScript hot reload újraépíti az érintett példányt. A Console mutatja a hibát.

#### Eredmény és gyakori hiba
A sablon valódi backendhez illő kiindulópont. Újratöltéskor az instance állapota újraindulhat; hibás forrás nem fut tovább érvényes új példányként. A script fájlmentése és a jelenetmentés külön lépés.

<!-- topic: script-api -->
### Script API: objektum, input, animáció és effektek

#### Mire jó?
Az API-n keresztül kereshetsz objektumot, transzformálhatsz, példányosíthatsz és játékviselkedést állíthatsz. A backend nevek és visszatérési formák eltérhetnek; a létrehozott sablont és a backend felületét kövesd.

- Entity: Find / EntityExists, Get/Set Position, Rotation, Scale; forgatás fokban.
- Input/idő: IsKeyDown, MouseDelta, DeltaTime, ElapsedTime.
- Világ: SpawnMesh, SpawnPrefab, DestroyEntity és SetMaterial; a létrehozás/törlés/materialváltás halasztva érvényesül.
- Animator: Float, Bool és Trigger állítás név alapján.
- Character: állapot lekérdezése, futás/ugrás engedélyezése.
- Audio/log: PlayOneShot, Log és LogError.
- ParticlePlay/Stop/Restart/Emit: emittervezérlés; Stop után a már élő részecskék befejezik életüket.

#### Rövid tutorial
1. Készíts egy scriptet a generált sablonból és rendeld egy mesh-hez.
2. Az OnStart-ban keress meg egy ismert nevű objektumot, és ellenőrizd, hogy az azonosító létezik.
3. OnUpdate-ban a delta idővel arányos mozgást vagy név szerinti Animator-paramétert állíts.
4. Play-ben figyeld a Console-t és a célt; Stop után az editorállapot tér vissza.

#### Eredmény és gyakori hiba
A sebesség nem függ közvetlenül az FPS-től. Hiányzó objektumra vagy komponensre küldött parancs nem épít automatikusan új komponenst.

<!-- topic: script-queries -->
### Raycast, OverlapSphere és Enabled állapot

#### Mire jó?
A Raycast sugarat, az OverlapSphere gömbtartományt vizsgál. A RaycastFiltered rétegmaszkot, triggerkezelést és figyelmen kívül hagyott entitást is használhat; a régi Raycast elérhető maradt.

#### Rövid tutorial
1. Készíts két collideres tárgyat külön Physics Layer-en.
2. Egy scriptből indíts szűrt sugarat a kamera vagy játékos helyétől; állíts véges irányt és pozitív távolságot.
3. A layerMask bitjeivel válaszd a keresett rétegeket, és ignoreEntityId-val zárd ki a saját testet.
4. Gömbös közelségvizsgálathoz OverlapSphere-t hívj, és ellenőrizd az eredményt illetve a truncated jelzést.
5. SetEntityEnabled segítségével ideiglenesen tilts egy objektumot, majd kapcsold vissza.

#### Eredmény és gyakori hiba
Az entity engedélyezése együtt érinti a render/fizika/script/audio/particle működést. Az overlap legfeljebb 1024 eredményt ad; a terrain/non-entity találat azonosítója lehet 0. A rétegmaszk nem azonos a réteg sorszámával.

<!-- topic: game-ui -->
### Játékfelület: RmlUi és szöveges kérés

#### Mire jó?
A játék saját HUD-ja és menüje RmlUi dokumentumokból, .rml és .rcss fájlokból készül. Ez a Game View rétege, külön az editor ImGui-paneljeitől.

#### Rövid tutorial
1. A projekt assetmappájában készíts például ui/hud.rml dokumentumot, az elemeknek adj id attribútumot.
2. Scriptből UiOpen-nel nyisd meg az assetmappához viszonyított útvonalat; ellenőrizd a nem nulla handle-t.
3. UiSetText, UiSetProperty és UiSetClass segítségével frissíts szöveget, RCSS tulajdonságot vagy osztályt.
4. UiConsumeClick segítségével kezeld a gombot; UiSetVisible/UiClose rejti vagy bezárja a dokumentumot.
5. Egy sor szöveg bekéréséhez a PromptText API és kapcsolódó lekérdezések használhatók; secret maszkolja a mezőt.

#### Eredmény és gyakori hiba
A HUD Play-ben és a runtime-ban jelenik meg. Stop bezárja a nyitott dokumentumokat és a futás kérésállapotát. Hibás elem-id vagy assetútvonal nem módosít másik elemet.

<!-- topic: network-native -->
### Hálózat és legacy natív C++ scriptek

#### Mire jó?
A script TCP API bytefolyamot ad: NetConnect, NetState, NetSend, NetReceive, NetClose. A játék protokollja és üzenethatárai a script feladata. A natív C++ script külön, legacy modulút, nem a Lua/AngelScript asset ráhúzása.

#### Rövid tutorial
1. Hálózatos scriptben NetConnect után állapotlekérdezéssel várj a kapcsolatra, ne blokkoló várakozással.
2. Az alkalmazás saját keretezésével küldj üzenetet; a fogadást részleges darabokból állítsd össze.
3. Hibás/lezárt állapotnál kezeld a kapcsolat végét, és NetClose-zal takaríts.
4. Meglévő natív modul forrásait a Scripts/Build felületen fordítsd; a Build Output adja a fordítóhibákat.

#### Eredmény és gyakori hiba
TCP nem garantál egy Receive = egy üzenet megfelelést. A streamek Stop után lezáródnak. A C++ modul nem hot reloadolt Lua; az SDK/ABI illeszkedése és a külön modulbuild szükséges.

## Világépítés és nagy jelenetek

<!-- topic: tree-generator -->
### Tree Generator: fa mentése assetként

#### Mire jó?
A Tools > Tree Generator procedurális fát készít. General/Seed, Trunk and Branches, Bark, Leaves és Output szakaszok állítják az alakot és felületet.

#### Rövid tutorial
1. Nyisd a Tree Generatort, és indulj egy presetből.
2. Állítsd a Seed-et; New Seed másik változatot ad azonos típusból.
3. A Branch levels és ágbeállítások után a Leaves alatt állítsd a lomb sűrűségét és kártyáit.
4. Bark/Leaves textúrákat válassz, majd az Output geometriai számait ellenőrizd.
5. Bake distant-tree impostor, Impostor distance és Save as Asset... segítségével ments projektassetet, majd húzd a jelenetbe.

#### Eredmény és gyakori hiba
Újrahasználható fa és választás szerint távoli atlasz készül. Sok egyedi levél helyett játékhoz levélcsoport-kártyákkal és mérsékelt geometriával dolgozz. Egy fa FPS-ét is mérd meg, mielőtt erdőt másolsz belőle.

<!-- topic: lod-occlusion -->
### LOD, levél-LOD és takarási szűrés

#### Mire jó?
A LOD távolabb egyszerűsíti a geometriát; a főnézeti levél-LOD ritkítja a lombkártyákat. A GPU takarási szűrés domb/nagy opaque tárgy mögött kihagyhat statikus csoportokat.

#### Rövid tutorial
1. Statikus modellhez add a LOD Group komponenst. Az Enabled kapcsolja a csoport használatát; az Override on this instance a modell alapbeállításának helyi felülírását engedi.
2. A színes sáv métertávolságokat mutat. Húzd a határokat, vagy nyiss le egy LOD-sort a Transition (m) és Target geometry szerkesztéséhez. A sorok a ténylegesen elkészült háromszögszámot és a LOD 0-hoz viszonyított százalékot mutatják; a Not built jelzi, ha még nincs eredmény.
3. Az Add LOD / Last LOD a szintek számát módosítja (1–4). A bekapcsolt csoport geometriája a háttérben elkészül közeli nézetben és kijelölés mellett is. A Building a CPU-generálást jelzi, a GPU-feltöltés külön állapotot kap. A Generate / Rebuild LODs szintén generálást kér; a Set as model asset default a modell összes használójával megosztja a beállítást.
   A Scene View és a Game View saját kamerájának távolságát és választott szintjét mutatja. A távolságot az objektum világban számolt határainak középpontjától mérjük; egy nagy befoglaló dobozon belül állva sem lesz automatikusan nulla. A kijelölés és a takarási passz nem kényszerít LOD 0-ra. Távolodj el a váltások ellenőrzéséhez. A határok vagy a hiszterézis módosítása újrahasználja a geometriát; a célarányok vagy a szintek számának módosítása újraépíti azt.
   Az alacsonyabb háromszögcélarány erősebb egyszerűsítést enged. A generátor először megőrzi a textúravarratokat, majd növeli a geometriai hibatűrést, és szükség esetén submesh-enként csoportosítja a csúcsokat. Nagyon alacsony cél módosíthatja a körvonalat vagy a textúravarratokat; a tényleges szám eltérhet a céltól, és minden submesh megtart geometriát. A LOD a részletességet csökkenti; az utolsó szint látható marad, nem tűnik el.
4. Az Advanced tartalmazza a hiszterézist, amely a távolsághatár közelében csökkenti az oda-vissza váltást. A legutolsó LOD nagyobb távolságban is látható marad; a panel nem vezet be külön távolsági elrejtést.
5. Tegyél fákat különböző távolságokra, készíts dombot vagy nagy tömör takarót, majd azonos kamerával/felbontással hasonlítsd össze az FPS/GPU időt.

#### Eredmény és gyakori hiba
A közeli objektum megőrzi a részletességét. Az automatikus geometriai LOD jelenleg statikus modellekhez használható; az alpha-blended materialok rajzolása az eredeti geometriát használja. Az alapból aktív takarás csak teljesen takart csoportot rejt; karakter, transparent, terrain-chunk és árnyék saját útja megmarad. Nyitott erdőben nincs feltétlenül nyereség. Indítás előtt IX_OCCLUSION_CULLING=0 kikapcsolja az új szűrést; ez nem editorcsúszka.

<!-- topic: impostors -->
### Impostor: modell, terrain és karakter

#### Mire jó?
Két külön megoldás van: előre bake-elt többirányú modellatlasz és automatikus GPU-s nézeti cache. Az utóbbi távoli terrain-chunkot, statikus tárgyat és az aktuális animált pózt is kezelhet.

#### Rövid tutorial
1. Statikus modellnél Asset Browser > jobb kattintás > Bake model impostor. Fánál a Tree Generator Output kapcsolója is bake-el.
2. Távolodj el a modelltől, és ellenőrizd az átmenetet. Disable model impostor a modellatlaszt letiltja.
3. Terrainhez vagy karakterhez nem kell ilyen fájlt kézzel készíteni: a GPU-s nézeti út automatikusan választ a megfelelő távoli részeknél.
4. Figyeld a Statistics/Performance Runtime impostors és Impostor image cache sorait.

#### Eredmény és gyakori hiba
Közeli, kijelölt, túl nagy vagy nem alkalmas objektum rendes geometriát használ. A modell-bake nem támogat minden materialt, például Blend/unlit/emissive megjelenést. A nézeti cache kis helyben fordulást kezelhet, sétáló kameránál visszavált. IX_TREE_IMPOSTORS=0 és IX_SCENE_IMPOSTORS=0 indítási kapcsoló az összehasonlításhoz.

<!-- topic: layered-world -->
### Layered World és többszintes járható világ

#### Mire jó?
A Tools > Layered World ütközési felületekből rétegadatokat készít; híd, emelet és egymás fölötti járható tér is leírható. Nem a terrain textúrafestés nyolc rétegéről van szó.

#### Rövid tutorial
1. Készíts collideres terrain-t és fölötte járható hídmodellt.
2. A modell Inspector > Layer generation alatt engedélyezd az Include collision surfaces beállítást és a megfelelő felszíni jelöléseket.
3. Tools > Layered World > Generate layers from collision; Show layer volumes segít az ellenőrzésben.
4. A Support probe (offline) alatt válassz volume-ot és X/Z pontot, majd Place support probe.
5. Export layer metadata menti a rétegadatot.

#### Eredmény és gyakori hiba
Külön járható térfogatok készülnek. Collider vagy helyes authoring jelölés nélkül a modell nem biztosít megfelelő felületet. Geometriaváltozás után generáld újra és ellenőrizd a rétegeket.

<!-- topic: server-export -->
### Strict server world export és spawn

#### Mire jó?
A szervernek szánt világexport terrain-, collision-, layer- és spawnadatokat ellenőriz. A szerver világcsomag különbözik a File > Build Game runtime-csomagtól.

#### Rövid tutorial
1. Mentett jelenetből generálj rétegeket, és ellenőrizd a support probe-ot.
2. Tools > Layered World > Server world export alatt adj World ID-t.
3. Player spawn X/Z és Spawn volume: 0 a terrain; más érték a generált volume.
4. Export strict server world indítja az ellenőrzött exportot; a státuszt és Console-t olvasd el.

#### Eredmény és gyakori hiba
A spawn csak alkalmas, nem blokkolt és befoglalható helyen fogadható el. Hibát ne puszta koordináta-átírással kerülj meg: ellenőrizd a kapszula helyigényét, collisiont és rétegazonosítót. Ez nem indít automatikusan szervert.

## Környezet és képminőség

<!-- topic: environment -->
### Environment, ambient és napszak

#### Mire jó?
A Scene Settings > Environment a Sun és Ambient közös beállítása. A nap iránya a megvilágításra, égboltra és naphoz kötött effektekre hat.

#### Rövid tutorial
1. Állítsd a Sun Height in sky és Direction értékét.
2. Intensity és szín után engedélyezd a Casts shadows opciót, ha árnyékot szeretnél.
3. Ambient szín/intenzitás segítségével szabályozd a sötét felületek alapvilágítását.
4. Time of day preset-tel próbálj reggelt és estét, majd ments.

#### Eredmény és gyakori hiba
A jelenet más fényhangulatot kap. A napszak itt presetválasztó, önmagában nem automatikus nappal-éjszaka óra. Teljesen fekete tárgynál a material és normal mellett az ambientet is ellenőrizd.

<!-- topic: sky -->
### Sky: szín, procedurális ég, cube map és panoráma

#### Mire jó?
A Scene Settings > Sky négy módot kínál: Color, Procedural, Cube map (6 images) és Panorama (360 image). A procedurális ég Zenith, Horizon és Ground színeket, valamint Sun size/glow értékeket használ. A képalapú ég HDR és EXR képet is támogat.

#### Rövid tutorial
1. Sky > Panorama (360 image): importálj 2:1 oldalarányú equirectangular képet, és húzd a Panorama mezőre.
2. Cube map esetén töltsd ki a Right/Left/Up/Down/Front/Back oldalakat. A right/left vagy px/nx jellegű egyező fájlnevek alapján az üres társoldalak automatikusan kitölthetők.
3. Állítsd az Exposure, Rotation és Tint értékeket. Ambient from sky esetén az ég átlagos színe adja a környezeti fény színét.
4. Próbáld a Procedural mód Zenith/Horizon/Ground, Sun size és Sun glow mezőit, majd ments.

#### Eredmény és gyakori hiba
Az égbolt háttérként jelenik meg a geometria mögött. A nap helyét és színét az Environment > Sun adja, nem külön gizmo. Más égbolttípushoz eltérő mezők tartoznak; hiányzó kép vagy rossz útvonal esetén a státusz segít.

<!-- topic: post-processing -->
### God rays és Tone mapping

#### Mire jó?
A God rays a naphoz kötött fénysugarakat ad, a Tone mapping a HDR jelenet megjelenített fényességét/színeit alakítja. Az exposure nem változtatja meg a lámpák fizikai intenzitását.

#### Rövid tutorial
1. Scene Settings > Environment alatt állíts jól látható napot.
2. God rays > Enabled: a megjelenő erősség, sűrűség és volumetric mezőket fokozatosan hangold.
3. Árnyékból képzett sugarakhoz legyen Sun > Casts shadows; figyeld a panel megjegyzését.
4. Tone mapping alatt állítsd az exposure-t és a választott leképezést, majd ugyanabból a kamerából hasonlíts.

#### Eredmény és gyakori hiba
Filmesebb kép készül. A nappal szembe néző sugarak és volumetric hatások drágábbak lehetnek. Túl világos képnél előbb az exposure-t és a fényeket mérsékeld, ne a textúrát sötétítsd véletlenszerűen.

## Beállítások, mérés és kiadás

<!-- topic: project-settings -->
### Project Settings és automatikus mentés

#### Mire jó?
Az Edit > Project Settings a projektszintű physics rétegmátrixot, audio buszokat és renderfelbontást kezeli. A szerkesztő automatikus mentése mellett a kézi mentés továbbra is használható.

Az automatikus mentés ötpercenként fut, ha nyitott projekt van Edit módban; Play alatt megvárja a szerkesztés folytatását. A státuszsáv mutatja a visszaszámlálást.

#### Rövid tutorial
1. Physics layers alatt nézd meg, mely rétegpárok ütköznek; csak a szándékos kapcsolatot módosítsd.
2. Audio alatt hangold a Master/Music/SFX hangerőt.
3. Rendering > Render resolution: View size a panel méretét követi; fix módok adott pixelszámot renderelnek. Custom után Apply Resolution kell.
4. Ctrl+S-sel mentsd a projektet és az aktuális jelenetet.

#### Eredmény és gyakori hiba
Az eltérő panelméret és fix renderfelbontás különböző terhelést okozhat. Az autosave nem helyettesíti a tudatos mentést a nagy műveletek vagy build előtt. A gravitáció és fixed timestep a Scene Settings > Physics alatt található.

<!-- topic: performance -->
### FPS, CPU/GPU és memória mérése

#### Mire jó?
A Scene View saját render-FPS-t mutat; a Game View külön FPS, CPU/GPU kihasználtság, RAM és renderer adatokat ad. A View > Statistics részletes frame- és jelenetadatokat mutat.

- A Statistics renderer-, jobs-, jelenetgeometria-, impostor- és képcache-számlálókat is mutat.
- Math library > Run Math Benchmark szintetikus számítási mérés; az átviteli sebessége eltér a jelenet FPS-étől.

#### Rövid tutorial
1. Állíts fix kamerát és renderfelbontást; egyszerre csak a szükséges nézet legyen aktív.
2. Jegyezd fel az FPS-t és a frame-időt. 60 FPS körül 16.7 ms, 120 FPS körül 8.3 ms jut frame-enként.
3. Egyetlen dolgot változtass: lombsűrűség, részecskeszám, árnyék vagy felbontás.
4. Várj a betöltés/előnézet/bake befejezésére, majd hasonlíts több másodperces eredményt.

#### Eredmény és gyakori hiba
Megtalálható a drága rendszer. A CPU/GPU kihasználtság nem azonos a frame-szakaszok idejével; a RAM kijelzésnél figyeld a százalék és MiB jelentését. A beküldött háromszögszám nem feltétlenül azonos a GPU-n ténylegesen rajzolt számmal takarási szűrésnél.

<!-- topic: physics-debugger -->
### Physics Debugger: alakok, sugarak és események

#### Mire jó?
A Tools > Physics Debugger a fizikai testeket és lekérdezéseket segít ellenőrizni, külön a látható mesh geometriájától.

#### Rövid tutorial
1. Play-ben nyisd a Physics Debuggert, és kapcsold be a kívánt Scene View debugrajzolást.
2. A Raycast from the camera szakasznál válassz szűrőt/távolságot és nyomj Raycast-ot.
3. Overlap in front of the camera alatt a Sphere, Box vagy Capsule próbával vizsgálj közeli testeket.
4. Collision and trigger events alatt figyeld az eseményeket, miközben két test találkozik.

#### Eredmény és gyakori hiba
Látható, mit érzékel a fizika. Ha nincs találat, ellenőrizd a layer/trigger szűrőt, aktív collider-t és tesztpontot. A debugrajzolás és folyamatos eseménylista többletmunkát jelent; a végleges FPS-mérésnél kapcsold ki.

<!-- topic: console-diagnostics -->
### Console, Build Output és diagnosztika

#### Mire jó?
A Console működési státuszokat és scriptüzeneteket, a Build Output fordító/csomagoló eredményeket gyűjt. Nem ugyanazt a hibaforrást mutatják.

#### Rövid tutorial
1. View > Console; a súlyossági szűrővel keresd az Error/Warning üzeneteket.
2. Hiba reprodukálása előtt Clear-rel tisztítsd a listát, majd egyetlen műveletet próbálj újra.
3. Script- vagy játékbuild hiba esetén nyisd a Build Outputot, és az első érdemi hibát keresd.
4. Tools > Engine Diagnostics alatt GPU frame capture (F11), frame profile és material dump (F12) érhető el.
5. A diagnosztikai kapcsolók ideiglenesen letilthatnak árnyék/reflection/asset discovery/watcher/hierarchy munkát.

#### Eredmény és gyakori hiba
Pontosabb hibaleírást kapsz. A GPU capture eszköztámogatástól függ. A passok letiltása nem a jelenet végleges javítása; a teszt után állítsd vissza a kapcsolókat. Az ImGui Demo Window fejlesztői példafelület.

<!-- topic: build-game -->
### Build Game: editor nélküli játék

#### Mire jó?
A File > Build Game... futtatható runtime-ot, szükséges engine fájlokat és projektmásolatot készít.

#### Rövid tutorial
1. Állítsd le a Play-t, és mentsd a projektet/jelenetet.
2. File > Build Game...: Game name, Output folder és Startup scene megadása.
3. Legacy C++ források esetén kérhető a scriptmodul fordítása; Lua/AngelScript nem igényel külön ilyen buildet.
4. Run the game when it is built jelöléssel az elkészült játék indítható. Nyomj Build-et.
5. A Build Output eredményét ellenőrizd, majd Run Game / Open Folder segítségével próbáld ki a csomagot.

#### Eredmény és gyakori hiba
A célmappában Game name.exe és Game/ projektmásolat készül. Az első runtime-fordítás több perc is lehet. A startup scene-nek a projekthez kell tartoznia. A közös renderer/fizika/script optimalizálás a buildelt játékban is megmarad; a Help editorablaka nem fut a runtime-ban.

<!-- topic: shortcuts -->
### Gyorsbillentyűk és fókusz

#### Mire jó?
A Help > Keyboard Shortcuts rövid listája mellett ez a téma a fontos kontextusokat is leírja.

- Ctrl+N: új jelenet; Ctrl+S: mentés; Ctrl+Shift+S: mentés másként.
- F5: Play / Stop; Shift+F5: Stop; F6: Pause / Resume.
- Jobb egér + WASD: kamera; W/E/R: Move/Rotate/Scale.
- F: kijelölés keretezése; Delete: kijelölt jelenetobjektum törlése.
- F11: GPU capture; F12: materialnapló; Asset Browserben F2 átnevezés.

#### Rövid tutorial
1. Jelölj ki modellt a Hierarchy-ban, nyomj F-et, majd W-t.
2. Egy szövegmezőbe kattintva írj nevet; ilyenkor a billentyűt a mező használhatja.
3. Fejezd be a szerkesztést, fókuszáld a Scene View-t, és próbáld újra a gizmo billentyűt.

#### Eredmény és gyakori hiba
A fókusz, aktív drag, ecset és Play mód befolyásolja a billentyű működését. Kamera mozgatása közben a W nem egyidejű gizmo-parancs.

<!-- topic: troubleshooting -->
### Gyakori hibák: gyors ellenőrző sor

#### Mire jó?
Segít eldönteni, melyik rendszer beállítását érdemes ellenőrizni.

- Nem tudsz létrehozni/szerkeszteni: nyitott projekt/jelenet, Stop Play, megfelelő kijelölés.
- Modell nem látszik: Enabled, editor láthatóság, Scale, kamera Near/Far, betöltési/importhiba.
- Material sötét vagy hiányos: texture slot, fény/ambient, normal map, alpha mód és override.
- Festés alig fed: Overall coverage; a lágy szélt Edge smoothing szabályozza.
- Karakter átesik: collideres talaj, kapszula, physics layer és aktív Play.
- Script nem indul: Enabled, Backend, asset/class, fordítás/hot reload hiba a Console-ban.
- Hang néma: Source, Listener, klip, buszhangerő és Play On Start.
- Alacsony FPS: azonos felbontás/kamera, csak szükséges nézet, lomb/árnyék/részecske/reflection ellenőrzése.

#### Rövid tutorial
1. Ments egy másolatot és egyszerűsíts egyetlen problémás objektumra.
2. Nyisd a megfelelő panelt és a Console-t.
3. Egy beállítást változtass, majd Edit/Play és runtime között hasonlíts.
4. Jegyezd fel a pontos lépéseket, üzenetet és mérési körülményeket.

#### Eredmény és gyakori hiba
Reprodukálható probléma marad találgatás helyett. Nincs minden szerkesztési műveletre általános Ctrl+Z ígéret; csak az adott eszközben ténylegesen elérhető visszavonást használd.
