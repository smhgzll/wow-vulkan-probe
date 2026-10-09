# Semantik draw sınıflandırması (yol haritası aşama 1, 27 Eylül 2026)

Her draw'a bir anlam atanıyor: gök, opak dünya, alfa testli dünya, su, transparan, additive, UI ya da bilinmeyen. Kaynak, katmanın zaten izlediği Vulkan durumu. Sistem **yalnızca gözlem yapar**, hiçbir şey farklı çizilmez. Amaç 2. aşamaya (opak / su / transparan sınırlarının bulunması) ve oradan fog'un transparanlarla doğru birleşmesine (3. aşama) sağlam bir zemin hazırlamak.

## Nerede ne var

| Dosya | Görev |
|---|---|
| `src/draw_semantics.{hpp,cpp}` | Saf kurallar. Girdi düz bir `DrawFacts`, çıktı sınıf + güven + kanıt bitleri + tek satırlık gerekçe. Vulkan'a bağımlı değil. |
| `src/draw_classifier.{hpp,cpp}` | Katman tarafı: izlenen durumdan `DrawFacts` (`effective_state` ile pipeline + dinamik durum), kayıt anında sınıflandırma, sayaçlar, `[DRAW-CLASS]` logu. |
| `src/discovery_spirv.cpp` | `find_dxvk_alpha_test`: DXVK'nın alfa testi kalıbı. `dxvk_alpha_test_state`: pipeline'ın specialization verisinden alfa testi. |
| `src/pipeline_discovery.cpp` | FS aşamasının alfa testini `Pipeline::alphaTest`'e yazar (GPL fragment library'sinden de taşınır). Pipeline hash'i değişmedi. |
| `src/discovery_commands.cpp` | `observe()` her draw'da `classify_recorded` çağırır. Anlık görüntü alınan draw'lara `Draw::semantic` yazılır. |
| `tests/draw_semantics_test.cpp` | Kural testleri; gerçek WoW draw'larının durumları ve muhafazakâr köşeler. |
| `tests/dxvk_alpha_test.frag` | DXVK alfa testi yapısının kopyası (fixture, `glslc -O` ile derlendi). |

Kapatmak için: `DEPTH_PROBE_DRAW_CLASSES=0`. Maliyet, `hook_bench` ile draw başına yaklaşık 50 ns (97 → ~150 ns); karede 2000 draw'da yaklaşık 0.1 ms CPU. Pipeline'ın dinamik durum maskesi artık oluşturulurken bir kez hesaplanıyor (`dynamic_state_mask`), `effective_state` her çağrıda set araması yapmıyor.

## Sınıflar ve kurallar

Sırayla uygulanır. İlk tutan kural kazanır, hiçbiri tutmazsa sonuç UNKNOWN ve gerekçesi yazılır.

1. **UNKNOWN:** rasterizer discard ya da viewport bilinmiyor.
   **DEPTH_ONLY:** renk yazma maskesi 0, depth test ve depth write açık. Sahne dışındaysa yüksek, sahne hedefindeyse (depth prepass) orta güven. Depth yazmayan renksiz draw'lar (stencil, occlusion) UNKNOWN kalır. WoW'da bunlar oyunun güneş gölge kademeleri (aşağıda); sınıf adı "gölge" iddia etmiyor, çünkü kayıt anında o depth'in sonradan nasıl örnekleneceği bilinmiyor.
2. **WATER (yüksek):** WoW'un su FS hash'i (`wowWaterFS`), dünya aralığında ya da sahne hedefinde.
3. **UI (orta):** tam 0..1 derinlik aralığı ve depth testi yok. Tam aralıkta depth test varsa UNKNOWN (UI içindeki 3D model olabilir, sahiplenilmez).
4. **SKY:** viewport minDepth ≥ 0.99 (WoW göğü 0.998/0.999..1). Depth testi açıksa yüksek, değilse orta güven. Sahne dışındaki bir depth hedefinde UNKNOWN.
5. **Dünya aralığı (0..<1, WoW'da 0..0.94)** dışında kalanlar UNKNOWN. Dünya aralığında depth test yoksa ya da blend veya depth test bilinmiyorsa da UNKNOWN.
6. **Blend yok + depth write:**
   - Alfa testi açık → **ALPHA_TEST (yüksek)**.
   - Alfa testi kapalı → **OPAQUE (yüksek)**.
   - Bilinmiyor → **OPAQUE (orta)**, `ALPHA-TEST-UNKNOWN` kanıtıyla. Katı geometri olduğu kesin, yalnızca opak/cutout ayrımı açık. İkisi de derinlik yazar ve transparanlardan önce gelir; 2. aşamanın ayrımı için bu yeterli.
   - Blend yok, depth write yok → UNKNOWN.
7. **Blend var:**
   - Faktörler bilinmiyor → UNKNOWN.
   - **Replace blend:** hedef ZERO ve kaynak hedefi okumuyor (SRC_ALPHA/ZERO gibi). Depth yazıyorsa **OPAQUE (orta)**, alfa testi açıksa ALPHA_TEST (orta), yazmıyorsa UNKNOWN. WoW'un WMO iki katmanlı malzemeleri böyle çiziliyor (`frame-scan-42775-2`, `-9`): aynı geometri önce SRC_ALPHA/ZERO + depth write ile, hemen ardından ONE_MINUS_SRC_ALPHA/ONE + depth write ile (additive ikinci katman) çiziliyor. Kural eklenmeden önce ilk katmanlar UNKNOWN çıkıyordu (7–27 draw/kare).
   - Hedef ONE → **ADDITIVE**.
   - SRC_ALPHA/ONE_MINUS_SRC_ALPHA, premultiplied (ONE/ONE_MINUS_SRC_ALPHA) ya da modulate (DST_COLOR·ZERO vb.) → **TRANSPARENT**. Depth yazmıyorsa yüksek, yazıyorsa orta güven.
   - Diğer faktörler → UNKNOWN.
8. **PARTICLE:** henüz hiçbir kural atamıyor. WoW 3.3.5 parçacıkları DXVK'nın sabit fonksiyon shader'larıyla çiziliyor. Onları diğer blend'li dünya draw'larından ayıran doğrulanmış bir imza yok; o zamana kadar TRANSPARENT/ADDITIVE sayılıyorlar. Log, blend'li sahne draw'larının imzalarını sıralayarak bu kuralın verisini topluyor.

Yalnızca viewport aralığına güvenilmiyor. Fog'un eski ayrımı (`fog_observe_draw`) yalnızca buna bakıyordu; o davranış değişmedi, 2. aşama doğrulandıktan sonra yeni sınıflara bağlanacak.

## DXVK'nın alfa testi: discard kanıt değil

Gerçek WoW verisi (Inspector seçimleri, `exports/selection-*`, 46 draw, 25 ayrı FS) şunu gösterdi: **her fragment shader'da discard (OpDemoteToHelperInvocation) var.** DXVK, D3D9'un sabit fonksiyonlu alfa testini her çevrilmiş shader'a gömüyor:

```
op = BitFieldUExtract( SpecConst[20] != 0 ? SpecConst[1] : spec_state_UBO.word[1], 21, 3 )
switch (op) { 0 NEVER … 6 GREATER_EQUAL; default (7 = ALWAYS): geçer }  →  geçmezse demote
```

- `find_dxvk_alpha_test`, bu yapıyı SpecId'leri, bit konumunu ve genişliği sabit kodlamadan yapısal olarak arar: `OpSwitch(≥6 durum) ← OpBitFieldUExtract(OpSelect(koşul, spec, diğer), sabit, sabit)`. Koşul, bir spec constant ile 0'ın `OpINotEqual`/`OpIEqual` karşılaştırmasıdır; `OpSpecConstantOp` ile katlanmış hâli de kabul edilir.
- Programlanabilir shader çevirilerinin tamamında (467/467 dosya) bulundu: SpecId 20, word 1, bit 21, 3 bit.
- Pipeline oluşturulurken FS'nin `pSpecializationInfo`'su okunur:
  - SpecId 20 ≠ 0 ise karşılaştırma işlemi kesin bilinir. ALWAYS ise alfa testi kapalı, değilse açıktır.
  - SpecId 20 yoksa ya da 0 ise değer DXVK'nın set 2 / binding 5'teki `spec_state` UBO'sundan draw başına okunur. Bu, GPL'nin hızlı bağlanmış pipeline'ları için geçerli; sonuç "bilinmiyor".
- DXVK'nın **sabit fonksiyon** shader'ları (gök kubbesi FS'i `429850c1…` gibi; 24 dosya) aynı düzeni `specUint(2)` yardımcı fonksiyonu ve bir düzen tablosu üzerinden okuyor. Yapısal arama bunları tanımıyor; "no DXVK alpha-test pattern" olarak raporlanıyorlar. Bunlar gördüğümüz örneklerde blend'li çizimler olduğundan 1. aşamada gerekmedi. 4. aşamada eklenecek.

Test: `pipeline_discovery` ctest'i şunları sınıyor:
- fixture'ı ALWAYS ile kapalı, GREATER ile açık okuması,
- SpecId 20 = 0 ve spec verisi olmayan durumlarda "bilinmiyor" demesi,
- GPL fragment library'sinden bağlanmış pipeline'a taşınması,
- düz bir shader'da kalıp bulmaması.

## Gösterim

- **Inspector → Selected Surface:** 3. satır `CLASS: OPAQUE (MEDIUM)`, 4. satır gerekçe.
- **`draw-state.json`** (seçim dışa aktarımı): `semantic` alanında sınıf, güven, gerekçe, kanıt ve alfa testi durumuyla kaynağı.
- **Discovery capture JSON'u:** her draw'da `semantic`.
- **Log:** her 900 karede, kare başına ortalama olarak dört satır:

```
[DRAW-CLASS] per frame over 900 frames | scene target: SKY 17.0 OPAQUE 280.0 … | other targets: UI 103.0 …
[DRAW-CLASS] unknown because: 12.0 FULL RANGE WITH DEPTH TEST (UI MODEL?); …
[DRAW-CLASS] solid world draws, alpha test from: 250.0 specialization; 30.0 fast-link (DXVK spec UBO); …
[DRAW-CLASS] top blended/unknown scene signatures: 9.0 ADDITIVE src6 dst1 dw0 topo3 fs …; …
```

## Kare taraması (`tools/frame-scan.py`) — oyunu kapatıp açmadan, tuşsuz

Kullanıcıdan kayıt almak yerine araç, çalışan oyundan kendisi tam kare ister:

```
tools/frame-scan.py --request            # istek at, bekle, raporla (varsayılan 3 kare; ilki kısmi olabilir, atlanır)
tools/frame-scan.py --watch 6 --every 20 # sen normal oynarken 20 s arayla 6 tarama, sonra özet + son taramanın raporu
tools/frame-scan.py [KLASÖR]             # var olan bir taramayı raporla
```

- **Katman tarafı** (`finish_present`, `discovery_capture.cpp`):
  - 30 present'te bir `$DEPTH_PROBE_EXPORT_DIR/frame-scan-request` dosyasına bakar. İçerik isteğe bağlı kare sayısıdır.
  - Dosyayı siler ve bir sonraki present'ten itibaren o kadar kareyi mevcut discovery capture ile kaydeder: her draw'ın ham ve birleştirilmiş (`effective`) durumu, sınıfı, hedefi, pipeline'ı (blend faktörleri, renk maskesi, alfa testi) ve shader'ları (`.spv`, discard, DXVK kalıbı).
  - Çıktı `frame-scan-<pid>-<n>/` klasörüne gider. Yazım bitince `DONE` dosyası bırakılır. Log satırı: `[FRAME-SCAN] request: …`.
- **Rapor** (klasördeki `report.md` ve ekran) sıralı sonuçlardan oluşur:
  1. Karenin pass'leri sırayla.
  2. Sahne hedefindeki sınıf dizisi ve her sınıfın ilk/son indeksi. Katı draw'ların tamamı blend'lilerden önce mi (**CLEAN BOUNDARY**) yoksa iç içe mi (**INTERLEAVED**)? 2. aşamanın asıl sorusu bu.
  3. UNKNOWN nedenleri ve arkalarındaki FS'ler.
  4. Opak draw'ların alfa testinin kaynağı ve açık olanlar.
  5. Blend'li imzalar (faktörler, depth write, topology, FS, programlanabilir/sabit fonksiyon, köşe sayısı). PARTICLE kuralının verisi burası.
  6. Tam aralıklı draw'lar (UI adayları), depth test açık/kapalı.
- **Oyun kontrolü:** Araç önce oyunun çalıştığına bakar. İstek 10 saniyede alınmazsa oyundaki katmanın eski bir derleme olduğunu söyler (bir kez yeniden başlatmak gerekir).
- **Test:** `pipeline_discovery` ctest'i istek dosyasını, tüketilmesini, `DONE`'u ve JSON'u sınıyor. `DEPTH_PROBE_KEEP_FRAME_SCAN=1` klasörü bırakır; araç onun üzerinde uçtan uca çalıştırıldı.

## Oyunda doğrulama (27 Eylül 2026, Booty Bay, 7 tarama, `frame-scan-36309-1..7`)

Kare başına 1700–1820 draw. Taramalar birbiriyle tutarlı (oyuncu yerinde duruyordu).

- **Sahne hedefindeki sıra:**

  ```
  OPAQUE×152 SKY×31 OPAQUE×314 TRANSPARENT×32 WATER×93 TRANSPARENT×3 ADDITIVE×2 TRANSPARENT×3 ADDITIVE×1 | UI×106
  ```

  Yedi taramanın hepsinde **temiz sınır** var: katı draw'ların tamamı ilk blend'li draw'dan (indeks ~500) önce. Gök, opak dünyanın ortasında çiziliyor. UI aynı sahne hedeflerine en sonda, ayrı bir pass'te geliyor.
- **UI:** sahne hedefindeki 106 draw'ın hepsi tam aralık ve depth testsiz. UI kuralı oyunda tutuyor.
- **Alfa testi:** opak dünya draw'larının %100'ünde pipeline'ın specialization verisinden biliniyor (fast-link hiç görülmedi). Hepsi ALWAYS, yani kapalı; bu sahnede alfa testli opak draw yok.
- **Blend'li draw'larda alfa testi:** su, UI, transparan ve additive draw'larda alfa testi **açık** (GREATER_EQUAL). WoW, blend'lilerde de alfa referansıyla sıfır alfayı atıyor. 32 draw'lık `fs 2ef17a4e` grubu (medyan 1539 köşe) blend'li, depth yazan ve alfa testli; ağaç ya da yaprak benzeri geometri. 4. aşama için önemli: WoW'da "cutout" bu tür draw'lar olabilir, opak fazda değil.
- **Gölge kademeleri:** karenin başında dört 2048×2048 D24S8 depth görüntüsüne toplam yaklaşık 950 yalnızca-depth draw (52 / 32 / 182 / 683). Opak dünya shader'ları (arazi `44c36902`, modeller `2212a63c`, `ba3c7f84`) bu dördünü set 1 / binding 1–4 (arazide 5–8) üzerinden karşılaştırmalı örnekleyiciyle okuyor. WoW gerçek güneş gölge haritaları çiziyor. 10. aşama (güneş gölge haritası) için büyük fırsat: sahneyi yeniden çizmek yerine oyunun kademeleri ve matrisleri kullanılabilir. Matrisler henüz bulunmadı.
- **UNKNOWN:** eskiden %56–57'ydi, neredeyse tamamı bu gölge draw'larıydı. DEPTH_ONLY sınıfıyla kare başına yaklaşık 1 draw kalıyor (swapchain'de tuhaf aralıklı bir draw).
- **Taramaya eklenenler:**
  - Draw başına render pass sırası (`pass`). Rapor, katı→blend'li sınırının bir DXVK pass sınırına denk gelip gelmediğini söylüyor; 3. aşamada fog'un nereye ekleneceği buna bağlı.
  - Gölge haritası bölümü.
  - `DONE` artık atomik yazılıyor. Eski katman içeriği yerinde yazdığı için bir tarama boş `DONE` okuyup çöküyordu; araç artık bekliyor.

## Pass segmentasyonu (yol haritası aşama 2, gözlem, 27 Eylül 2026)

**Soru:** Fog'un opak dünya ile blend'li dünya arasına girmesi için tek bir bölme noktası var mı, varsa nerede?

**Oyundan gelen yapı** (Booty Bay taramaları):
- Arazi, gök, modeller, ardından ağaç benzeri blend'liler, su ve efektler: dünyanın **tamamı tek bir DXVK render pass'inde**.
- UI aynı hedeflere sonra, ayrı bir pass'te çiziliyor.
- Katılar çoğu karede blend'lilerden önce bitiyor. Ama bir modelin kendi additive parçası opakların arasına düşebiliyor: `frame-scan-42775-1`'de bir fener parıltısı `fs 2212a63c`, ardından 6 opak draw geliyor.

**Üç aday kural** (`src/draw_segments.*`, saf mantık, gerçek dizilerle unit test):
- **A:** ilk blend'li draw'dan önce.
- **B:** ilk alpha-blend'li dünya draw'ından (TRANSPARENT/WATER) önce. Opak fazdaki additive parıltılar bölmenin önünde kalır.
- **C (önerilen):** ilk **depth yazan** TRANSPARENT (yapraklar: alpha blend + alfa testi + depth write) ya da ilk WATER draw'ından önce.

WoW, opak fazda her nesnenin kendi blend'li parçalarını nesneyle birlikte çiziyor: WMO'ların ikinci malzeme katmanları, fener parıltıları, bir M2'nin depth yazmayan alfa parçası. Asıl transparan faz yapraklar ve suyla başlıyor. C bu yapıya uyuyor.

Her kural için ölçülenler:
- bölmeden sonra kalan renk yazan katılar: fog'dan sonra çizilir, fog almaz, **görünür hata**,
- bölmeden önceki **depth yazmayan** blend'liler: bugünkü gibi arkalarındaki derinlikle fog alırlar. Depth yazan blend'liler (WMO'ların additive ikinci katmanı, yapraklar) kendi derinliklerini bıraktıkları için doğru fog alır; ayrıca "zararsız" olarak sayılır,
- bölmenin bir render pass başlangıcında olup olmadığı. Değilse fog'u eklemek o pass'i bölmek demek.

**Ölçüm:**
- **Katman:** `[DRAW-SEGMENTS]` logu, 900 karede bir. Kural başına bulunma oranı, temiz kare oranı, sonradaki katıların ortalaması ve en çoğu, öncesindeki blend'liler, pass başlangıcı oranı, ortalama indeks.
- **Tarama raporu:** 2. bölümde her iki kuralın o karedeki sonucu.

| Kare | Kural B | Kural A |
|---|---|---|
| `36309-*` | indeks 497, sonrasında katı 0, öncesinde blend'li 0 | aynı |
| `42775-1` | indeks 308, sonrasında katı 0, öncesinde 1 additive | indeks 301, sonrasında 6 opak |
| `42775-2..8` | indeks ~674, sonrasında katı 0, öncesinde depth yazmayan 1 (+7 WMO katmanı) | indeks 126, sonrasında 534 katı |
| `42775-9` | indeks 555, sonrasında katı 0, öncesinde depth yazmayan 5 (+27 WMO katmanı) | indeks 86, sonrasında 411 katı |

| `42775-11` | **indeks 476, sonrasında 29 katı** (bir M2'nin opak fazdaki alfa parçası) | indeks 126, sonrasında 348 katı |

**Kural C, iki oturumdaki 26 karenin hepsinde bölmeden sonra 0 katı bırakıyor** (11. tarama dahil: indeks 512). Bölmeden önce kalan depth yazmayan blend'li sayısı 0–7. Kural A pratikte kullanılamaz; B bir karede başarısız oldu. Log ve rapor C'yi önce yazıyor.

**Bölgeler arası (27 Eylül 2026, 27 dünya karesi):**

| Bölge | Tarama | Draw/kare | Yaprak | Su | Kural C (sonrasında katı / öncesinde depth yazmayan) |
|---|---|---|---|---|---|
| Booty Bay | 18 | 200–2700 | 27–41 | 77–115 | 0 / 0–7 |
| Teldrassil (iç mekân) | 1 | 175 | 0 | 0 | 0 / 4 (bölmeyi bir WMO'nun depth yazan yarı saydam parçası başlatıyor) |
| Teldrassil | 2 | ~450 | 12 | 0 | 0 / 0 |
| Mulgore | 4 | ~880 | 52–58 | 0 | 0 / 0 |

İki tarama, UI içinde çizilen bir 3D modeli yakaladı (tam aralık + depth test, 72 draw). Muhafazakâr kural gereği UNKNOWN kaldılar; doğru olan bu.

**Yeni desenler:**
- WMO'ların iki geçişli yarı saydamlığı: aynı geometri önce DEPTH_ONLY, sonra depth yazan alpha blend. Transparan fazda çiziliyor; kendi derinliğini bıraktığı için fog'u doğru alır.
- Parçacık adayları: `fs c22a1e2e`, depth yazmayan, 6'nın katı küçük köşe sayıları (tek tek dörtgenler).

**PARTICLE için temiz bir imza yok.** Aynı vertex shader (`75c42f7a`) ve FS parçacık benzeri dörtgenlerde, M2 parıltılarında, gök parçalarında ve alfa testli M2 gövdelerinde kullanılıyor; hepsi indeksli. Fog açısından belirleyici olan parçacık olup olmamak değil, **depth yazmamak**. 3. aşama "blend'li ve depth yazmayan" draw'ları hedef alacak. PARTICLE sınıfı atanmamaya devam ediyor.

Henüz görülmeyenler: yaprak ya da su olmayan dış mekân sahneleri (şehir içi, zindan). Orada C hiç bulunmaz ve bölme bugünkü gibi pass sonunda kalır, yani gerileme yok; ama o sahnelerde transparanlar yine arkalarındaki derinlikle fog alır.

İkisinde de bölme **render pass'in içinde**. Yani 3. aşamada fog'u oraya eklemek, dünya pass'ini bölmek (bitir, fog composite, aynı hedeflerle LOAD ile yeniden başlat, durumu geri yükle) ya da pass içinde çalışan başka bir yol gerektirecek.

**3. aşama bu veriyle tasarlandı** ([fog.md](fog.md), "Transparanlara duyarlı fog"). Tek bir bölme noktası yerine yalnızca yanlış fog alan grup (depth yazmayan, su olmayan blend'liler) composite'ten sonraya bırakılıyor. Kural C'nin ölçüsü, o grubun ne kadarının opak fazda kaldığını izlemek için log'da duruyor.

## Doğrulama durumu

- **Doğrulanan:**
  - Kurallar gerçek WoW draw durumlarıyla test edildi: arazi, su karosu, gök kubbesi.
  - DXVK alfa testi kalıbı gerçek shader'ların tamamında bulundu.
  - Fog smoke'unda gerçek GPU'da bir karede 3 OPAQUE + 1 UI, gök bölünmesinde 1 SKY sayılıyor. Kurallar bozulunca bu testler başarısız oluyor.
- **Oyunda doğrulanan (Booty Bay):** sınıf dağılımı, UI'nin depth testsiz olması, alfa testinin %100 specialization'dan bilinmesi, blend'li imzalar, temiz katı→blend'li sınırı. Bkz. yukarıdaki bölüm.
- **Henüz görülmedi:** orman (alfa testli yapraklar), bina içi, yoğun büyü efektleri ve parçacıklar, başka su türleri. Bir de pass sınırının yeni taramalardaki durumu (oyunun yeni derlemeyle bir kez yeniden başlatılması gerekiyor).

## Bilinen sınırlar

- PARTICLE atanmıyor (yukarıda açıklandı).
- DXVK sabit fonksiyon shader'larının alfa testi okunmuyor (4. aşama).
- Hızlı bağlanmış pipeline'larda alfa testi için draw başına UBO okuması gerekir. Maliyetli; Inspector'ın uniform okuma altyapısıyla 4. aşamada yapılabilir.
- Sahne hedefi, Scene Debugger'ın her present'te seçtiği sahne derinliği. İlk karelerde bilinmeyebilir; o zaman gök kuralı sahne dışındaki bir depth hedefi için UNKNOWN verir.
- Sınıf, kayıt anındaki komut durumundan hesaplanıyor. Secondary command buffer'lardaki draw'lar da kendi durumlarıyla sayılıyor; kalıtılan hedef bilgisi eksikse sahne hedefi "hayır" çıkar.

## 4. aşama: alpha test / cutout (27 Eylül 2026, Shadowglen)

**Tarama** (`frame-scan-54178-2`, kare başına):
- 238 katı dünya draw'ının hepsinde alpha test pipeline'ın specialization'ından biliniyor ve hepsi kapalı (ALWAYS).
- Asıl cutout'lar yapraklar: alpha blend + alpha test (GEQUAL) + derinlik yazan 70 draw (ağırlıklı fs `2ef17a4e`, medyan 3168 vertex). Bunlar TRANSPARENT sayılıyordu.
- Tanınmayan alpha test sadece sabit fonksiyon gök/UI shader'larında (`429850c1`), dünyada yok.

**Kural:** Alpha blend (veya modulate) + alpha test açık + derinlik yazan draw **ALPHA_TEST** sınıfına girer, `CUTOUT` kanıtıyla. Alpha test derinliğin nereye yazılacağına karar veriyor, blend sadece kenarları yumuşatıyor; yani derinlik, sis ve ışık için "delikli katı". Güven: alpha eşiği biliniyorsa yüksek, değilse orta. Kalan durumlar değişmedi: derinlik yazmayan alpha testli karışım TRANSPARENT, additive ADDITIVE, alpha testsiz karışım TRANSPARENT.

**Alpha eşiği:** DXVK'nın D3D9 `render_state` push bloğunda (fragment) `alpha_ref`, 24. bayt / word 6 (ofsetler shader'ın SPIR-V'sinden okundu). Frame-scan JSON'da `semantic.alphaRef`, raporda 4. bölümde dağılımı.

**Segmentler (2. aşama):** Karışımlı cutout karışımlı faz sayılır. C kuralı "derinlik yazan ilk TRANSPARENT/karışımlı cutout ya da WATER" olarak kaldı. Cutout'lar kuraldan sonra gelen "katı draw" sayılmaz; karışımsız alpha testli draw katı sayılmaya devam eder.

**Fast-link:** DXVK'nın fast-link pipeline'larında alpha test specialization'da değil, `spec_state` UBO'sunda. `find_dxvk_alpha_test` artık SELECT'in öbür kolundaki UBO okumasının set/binding/bayt ofsetini de buluyor (WoW: set 2 binding 5, üye 1, bayt 4; test fixture'ında std140 dizi, bayt 16). `classify_recorded` her fast-link draw'ında o word'ü host-coherent ring'den okuyor (`read_bound_uniform_word`: snapshot yok, descriptor yürüyüşü yok). Log kaynağı: "fast-link, read from the spec UBO".

**Log:** `[DRAW-CLASS] blended cutouts (alpha test + depth write): N per frame - x ref R, …`.

**Testler:** Semantik: yaprak (eşikli/eşiksiz), cam, partikül, glow; segmentler: opak → cutout → su, C kuralı cutout'ta başlıyor, sonrası 0 katı. Discovery: fixture'da fast-link word'ünün yeri (set 2, binding 5, bayt 16).

**Oyunda doğrulandı (Shadowglen, `frame-scan-62384-1`):** kare başına 44 yaprak draw'ı yüksek güvenle ALPHA_TEST (fs `2ef17a4e`), alpha eşiği hepsinde 128/255 (0.5, gerçek kesme). Log: `[DRAW-CLASS] blended cutouts (alpha test + depth write): 44.0 per frame - 44.0 ref 128`. C kuralı ilk cutout'ta başlıyor (%100), sonrasında 1 katı draw kalıyor: yapraklardan sonra çizilen bir gök parçası. `tools/frame-scan.py` bölüm 2'deki kurallar da cutout'u karışımlı faz sayacak şekilde güncellendi. Fast-link okuması burada denenemedi (bu sahnede hepsi specialization); Booty Bay'de bakılacak. Sabit fonksiyon shader'larının alpha testi (`specUint(2)`) hâlâ tanınmıyor; sadece gök/UI'ı etkiliyor.
