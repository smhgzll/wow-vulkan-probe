# Su yansımaları ve su görünümü

Bilinen WoW su shader'ı için oyunun çizdiği suyu, aynı kare içinde SSR veya planar yansıma + dalga, köpük, kostik, parıltı katmanlarıyla yeniden birleştirir (composite). Oyuna veya Wine prefix'ine dosya kurulmaz; her şey Vulkan layer içinde, oyunun render pass'inin sonunda çalışır. Son güncelleme: 21 Eylül 2026.

**Durum.** SSR ve planar gerçek WoW'da çalıştı (yansıma, gökyüzü belleği, köpük, ayar kalıcılığı, capture araçları). Son iki tur (köpük yeniden yazımı, taban netliği/kostik/parıltı/whitecap, FPS çalışması) **gerçek WoW'da henüz denenmedi**; yalnız sentetik GPU testleri, yakalanmış gerçek karelerin GPU'da yeniden oynatılması (`tools/water-preview.py`) ve birim testleri var. **Açık sorun:** kullanıcı FPS'in ~180'den ~40'a düştüğünü bildirdi; ölçülen composite maliyeti (1.3–2.9 ms @1080p) bunu açıklamıyor. Şüpheliler: mask pass (orijinal su shader'ı tüm karolar için), planar mod, overlay maliyeti. Log satırları aşağıda.

## Kullanım

1. Steam launch options: `/mnt/Games/Projects/vulkan-depth-probe/tools/run-inspector.sh %command%`; oyunu tamamen kapatıp aç. WoW multisampling **1** olmalı.
2. **F10 → Inspect Surface**, suya tıkla (gerekirse *Selected Surface → Next GPU Hit* ile su adayına geç). Desteklenen profil: VS `2fce10e0177c7507`, FS `6e0413111ae30ee9`.
3. **Back to Home → Water Reflections → Use Selected Water Candidate**. Seçilen profil `water.ini`'ye yazılır; sonraki açılışta otomatik gelir.
4. Sayfa 7 (Water Reflections): **Mode** `Off → SSR → Planar`, **View** (Final, Mask, Normal, Water Depth, Reflection, Base Surface, Displacement, Large Waves ve MODERN bileşenleri: Medium/Micro/Final Normal, Fresnel, Roughness, SSR Confidence, Planar Weight, Environment Weight, Sun Glints, Water Thickness, Absorption, Refraction, Caustics, Water Tint), **Style** (satır 21: MODERN ↔ CLASSIC, A/B), **Strength**, **Capture Debug Data**, **Layers** (hazır kombinasyonlar), **Tone**, **Water Details**, **Performance Test**.
5. Sayfa 8 (Water Details): satıra tıklayınca sıradaki değere geçer, ayarlar otomatik kaydedilir; alttaki satır 30 geri döner.

Durum satırı engeli gösterir (`Water replay blocked: ...`); `recorded` yalnız komutların kaydedildiği anlamına gelir. Sahne/depth değişince kayıtlı profil korunur (`profileSaved`).

### Ayarlar (`~/.config/vulkan-depth-probe/water.ini`, `DEPTH_PROBE_WATER_SETTINGS` ile değiştirilebilir)

Geçersiz/aralık dışı değer yalnız kendi anahtarını atlar (önceki değer kalır); bilinmeyen anahtarlar yok sayılır. Menü her değişiklikte kaydeder.

| Anahtar | Varsayılan | Adımlar / anlam |
|---|---:|---|
| `profile` | 0 | 1 = bilinen WoW su shader çifti otomatik seçilir |
| `mode` | 0 | 0 kapalı, 1 SSR, 2 planar |
| `strength` | 0.70 | yansıma yoğunluğu (0.05–1.5) |
| `layers` | 15 | bit maskesi: 1 yansıma, 2 kırılma dalgalanması, 4 derinlik rengi + ton + **taban/kostik**, 8 kıyı/açık deniz **köpüğü**; parıltı yansıma katmanına bağlıdır |
| `tone` | 1 | 0 doğal, 1 tropikal, 2 derin mavi, 3 zümrüt, 4 altın |
| `quality` | 1 | SSR: 0 düşük (6 px, ≤32 adım), 1 orta (4 px, ≤64), 2 yüksek (3 px, ≤128) |
| `foam_amount` | 1 | 0, .5, 1, 1.5, 2 (0 = köpük yok) |
| `foam_size` | 1 | .5, .75, 1, 1.5, 2.5: yama, dantel ve dalga cephesi ölçeği |
| `foam_speed` | 1 | 0, .5, 1, 1.5, 2.5: köpük saatinin hızı (saat birikimlidir, hız değişince desen sıçramaz) |
| `foam_softness` | .5 | 0–1: kenar yumuşaklığı |
| `foam_pulse` | .7 | 0, .35, .7, 1: yamaların gelip gitme şiddeti |
| `foam_reach` | 1 | .4–2.5: köpüğün kıyıdan uzanma mesafesi (su derinliği) |
| `detail` | .6 | 0–1.5: dalga dokusu (normal) detayı |
| `game_opacity` | 1 | 0–1: oyunun kendi su dokusunun opaklığı; altında komşu su piksellerinin ortalamasına yumuşatılır (kendi köpük çizgisi dahil) |
| `clarity` | .6 | 0–1: sığ suda deniz tabanının görünürlüğü (derinlik katmanı gerekir) |
| `caustics` | .8 | 0–1.6: taban kostiği |
| `glitter` | 1 | 0–2: güneş parıltısı (yansıma katmanı gerekir) |
| `whitecaps` | .5 | 0–1: açık deniz dalga tepesi köpüğü (köpük katmanı gerekir); tepeler Gerstner yüzeyinden gelir, dalga yoksa whitecap da yok |
| `style` | 1 | 1 MODERN (katmanlı yüzey, Fresnel, pürüzlülük, güneş, soğurma), 0 CLASSIC (29 Eylül öncesi görünüm, bit bit aynı) — menü sayfa 7 satır 21 |
| `roughness` | .06 | .03–.25: GGX pürüzlülüğü (MODERN, ayar değeri) |
| `medium_normals`, `micro_normals` | 1, 1 | 0–2: orta / mikro dalgacık ölçeği (1 ≈ rms eğim .07 / .08; ikisi birlikte sakin rüzgârın Cox–Munk değeri ~.12'ye yakın: ÖLÇÜLMÜŞ referans, ayar ölçeği) |
| `subsurface` | 1 | 0–2: güneşe karşı dalga tepelerinden geçen ışık (MODERN) |
| `absorption` | .31,.055,.008 | yard başına r,g,b soğurma (MODERN; varsayılan saf su, Pope & Fry 1997: ÖLÇÜLMÜŞ), `clarity` ile ölçeklenir: 1 = bu değer, 0 = 8× bulanık |
| `sun_fallback` | off | `yükseklik,azimut` (derece): oyunun güneşi okunamazsa kullanılacak güneş; `off` = güneş terimi yok |
| `wave_height` | 1 | 0, .5, 1, 2, 4: Gerstner dalgalarının genlik çarpanı (0 = düz taban düzlemi) — menü **WAVE HEIGHT** |
| `wave_steepness` | 1 | 0, .5, 1: yörünge şekli çarpanı (0 = parçacıklar yalnız iner/çıkar) — menü **WAVE STEEPNESS** |
| `wind_direction` | 25 | derece (−360..360): bütün dalgaları döndürür (ayar değeri) |
| `wave_speed` | .75 | 0–4: fiziksel faz hızlarının genel çarpanı (ayar değeri) |
| `wave1`…`wave5` | aşağıda | `yön(°),dalga boyu(yd),genlik(yd),dikiklik(0..1),hız(× fiziksel)`; dalga boyu veya genlik 0 = boş yuva. Beş sayıdan biri bozuksa bütün yuva eski değerinde kalır |

## Nasıl çalışır

Hepsi render-pass-end hook'unda (`record_water_reflections`) aynı command buffer'a kaydedilir; ek queue submit, semaphore veya kare başına queue/device idle yoktur. Yalnız tek graphics queue, primary/one-time-submit command, tek color + depth attachment, MSAA yok, secondary/indirect/instanced/query/transform-feedback/conditional draw yok. Başarısız veya desteklenmeyen durumda oyunun görüntüsü değişmez; nedenler tek satırda toplanıp durum satırına ve loga yazılır.

Sırasıyla:

1. **Kamera pass'i** (20×1 RGBA32F): WoW su VS'nin UBO'sundan projection (`c0..c3`) ve görünüm (`c5..c8`) matrisleri okunur; GPU matrislerin sonlu/simetrik/+Z ileri/0..1 olduğunu, seçilen draw'ın tüm vertex'lerinin okunabilir/sonlu olduğunu doğrular (texel 13.x: kamera geçerli; maske, seviyeler, gökyüzü ve sisin su yüzeyi durağı yalnız buna bağlı). Ayrıca referans seviyeyi arar: kameranın **altındaki** ilk yatay 3-vertex dizisi, tolerans 0.005 (texel 18: düzlem geçerli, yükseklik, kamera dünya z'si; yalnız planar ayna bunu ister) . Texel düzeni: 0..3 yansıma matrisi, 4..7 ters projection, 8 clip-uzayı düzlem, 9 view-uzayı düzlem, 10 (near, far, P00, P11), 11 viewport, 12 depth aralığı, 13 (kamera geçerli, yükseklik, P22, P23), 14..17 view matrisi kolonları, 18 (düzlem geçerli, düzlem yüksekliği, kamera z, 0).
2. **Mask pass:** deniz ~170 karodur (her biri 640 indeksli tek triangle-strip draw). İlk karo kamerayı ve (varsa) referans düzlemi doğrular, uygun **tüm** karolar özel hedefe orijinal VS/FS ile çizilir; maske alfası yalnız kamera geçerliliğiyle çarpılır (texel 13.x), düzlemle değil. Fragment SPIR-V'si (`water_spirv.cpp`) orijinal sampling/discard/alpha-test'i korur; çıktı 0 = view-space konum + orijinal alfa, çıktı 1 = orijinal renk (`waterColor`, RGBA16F). Referans düzlemde olmayan pikseller composite'te kendi seviyeleriyle işlenir (yatay, kameranın altında), değilse oyunun görünümünde kalır. Özel derinlik tamponu game'in derinliğinden bağımsızdır.
3. **Planar pass** (yalnız mod 2): aynı pass'in opak draw'ları yansıtılmış VS (clip = `P·M·mirror·M⁻¹·P⁻¹`, ters winding, ek clip distance) ile özel hedefe yeniden çizilir.
4. **Sahne kopyası** ve derinliğin GENERAL'a geçişi, ardından **gökyüzü belleği** (128×64 RGBA16F, dünya azimut/yükseliş; ekranın gösterdiği gökyüzü %12 ile harmanlanır, hiç görülmeyen yönler varsayılan gradyanda kalır — kamera aşağı bakınca yansıma bozulmasın diye).
5. **Composite** (tam ekran, `water_composite.frag`): yalnız su pikselleri için oyunun rengini yeniden yazar. Önce pikselin görüş ışını **Gerstner yüzeyiyle** kesilir (aşağıda "Yüzey"); sonra katmanlar: dalga normali (Gerstner + 3 kısa sinüs + doku eğimi), derinlik (kırılma dalgalanması, derinlik rengi, ton), **taban netliği ve kostik**, SSR (adımlar uzağa doğru seyrekleşir) veya planar arama + gökyüzü belleği, Fresnel, **parıltı**, **köpük** (aşağıda). Sonra oyunun state'i geri yüklenir ve orijinal pipeline yeniden bağlanır.

Ömür: ağır kaynaklar (`WaterBatch`, 1080p'de ~100 MB) oyunun color/depth image çifti başına bir kez yaratılır ve o çifte çizen tüm command buffer'lar paylaşır (üst sınır: 3 batch, 64 slot); her komut kendi `WaterSlot`'unu (descriptor set + parametre buffer'ı + zaman damgası sorguları) taşır. Her çalıştırma ALL_COMMANDS bariyeriyle başlar ve kendi kendine yeterlidir, tek queue'da iki çalıştırma çakışmaz.

**State geri yükleme.** Özel pipeline'lar statik state taşıdığından, uygulamanın son dinamik state'i (viewport/scissor, depth, cull, topology, bias, stencil, blend sabitleri, line width, depth bounds, logic op, **blend enable/equation/write mask**, EXT/core varyantı dahil) düz veri olarak (`WaterRestore`) tutulur ve replay'den sonra geri yazılır; ardından descriptor set'leri, push constant'lar, vertex/index buffer'ları ve orijinal pipeline.

### Köpük

Yerel köpük miktarı (0..1) bir **kapsama oranıdır**; değerleri düzgün dağılımlı ince bir dantel dokusu `1-miktar` eşiğinde dar bir kenarla kesilir (toon/kıyı su shader'larındaki "gürültü eşiği × derinlik gradyanı" tekniği). Miktar düşükken yalnız ince kabarcık ağı, yüksekken içinde delikler olan yoğun köpük kalır; kenarlar dantelli olur. Miktar şunlardan oluşur: kıyı zarfı (su derinliği) × kıyıya koşan swash cepheleri (kıyı tarafı keskin, deniz tarafına dantel kuyruğu, kenar dantel ölçeğinde koparılır) × sürüklenen/warp edilen yama alanı × yama başına ritim (köpük gelir gider) + yalnız yakında su çizgisi köpüğü (uzaktan düz beyaz çizgi olmasın diye 12–45 yd arası söner). Tüm katmanlar tek hızla birlikte sürüklenir. **Açık deniz whitecap'leri:** uzun dalgaların (2.6–23 yd) tepe yüksekliği eşik üstündeyse ve tepe geçtikten sonra kısa süre (zamanda geriye örneklenmiş aynı nokta; kalıcı tampon yok), aynı dantel eşiğiyle, hiç dolu olmadan (üst sınır .62). Derin suda hiçbir şey hesaplanmaz.

### Taban netliği ve kostik

Oyun sahneye `a·W + (1−a)·S` karıştırır (W su shader çıktısı, S deniz tabanı). Mask pass W ve `a`'yı ayrı kaydettiği için composite `S = (sahne − a·W)/(1−a)` ile tabanı geri kazanır, kostiği tabana uygular ve sığ suda alfayı `exp(-0.42·derinlik)` ile azaltıp yeniden karıştırır. Karışım denklemi statik pipeline faktörlerinden veya `vkCmdSetColorBlendEquationEXT` hook'undan okunur: SRC_ALPHA/ONE_MINUS_SRC_ALPHA → mod 1, ONE/ONE_MINUS_SRC_ALPHA → mod 2 (premultiplied), bilinmiyorsa mod 1 varsayılır (`[WATER-BLEND] ... (assumed alpha blending)`), başka denklemde mod 0 (taban netliği kapanır, kostik suyun kendisini aydınlatır). **Bu denklemin gerçek WoW'da doğru olduğu henüz doğrulanmadı;** sığ su tuhaf görünürse `clarity` 0 yapılır. Sentetik sahnede matematik doğrulandı.

### Dokular

`shaders/water_detail.bin` (`tools/generate-water-textures.py`, sabit tohum, telifsiz — kullanılabilir lisanslı döşenebilir su yüzeyi dokusu bulunamadı): 256² RGBA8, **3 katmanlı dizi × 9 mip**. Katman 0 rüzgâr yönlü dalga eğimi (RG, ±1 = 3σ) + yükseklik; katman 1 köpük (R dantel, G ikinci dantel, B kabarcık, A çizgiler); katman 2 kostik (R, G iki kabarcık ağı; çarpılarak kullanılır). Batch'in ilk 8 çalıştırması bunu tutulan staging buffer'dan yükler (her çalıştırma kendi başına yeterli). Dosya yok/bozuksa düz 1×1 dokuyla çalışır (`[WATER-TEXTURE]`). Launcher dosya yoksa üretir (numpy gerekir).

## Yüzey: Gerstner dalgaları (yenileme aşama 1–3, 29 Eylül 2026)

Aşamalı su yenilemesinin ilk üç adımı: mesh yoğunluğu ölçümü, büyük dalgaların yer değiştirmesi ve normallerin aynı dalga fonksiyonundan hesaplanması. SSR/planar yolu değişmedi; yalnız yansımanın başlangıç noktası ve normali artık hareketli yüzeyden gelir. **Oyunda henüz görülmedi** (yeniden başlatma gerekir: `WaterParams` 176 → 352 bayt).

**Aşama 1 — WoW su mesh'i (ÖLÇÜLDÜ, 115 gerçek capture, 29 Eylül):** her karo düzenli bir ızgarada triangle strip; ızgara adımı **4.167 yd** (33.33 yd'lik harita parçası / 8), tam karo 66.7 × 66.7 yd ve en çok 17 × 17 vertex. Üçgenler 4.17 × 4.17 × 5.89 yd, alanı 8.68 yd². En sık draw 640 indeks (5.280 kez görüldü); kıyıdaki karolar yalnız çizilen kareleri içerir (20–630 indeks). Karo başına tek yükseklik vardır (iki seviyeli karolar hariç). **Sonuç:** 6.5 yd'lik bir dalgaya dalga boyu başına 1.6 vertex düşer (aliasing), 11 yd'liğe 2.8 (belirgin köşelenme); yalnız 30 yd üstü ölü dalgalar pürüzsüz deforme olurdu. 3–5 Gerstner dalgası orijinal mesh'te **kullanılamaz**.

**Tasarım: yoğun ızgara yerine piksel başına kesişim.** Composite her su pikselinin görüş ışınını yer değiştirmiş yüzeyle keser (Newton yöntemi, taban düzlemi kesişiminden başlayarak 3 adım; türevler normalle aynı kısmi türevlerden gelir). Oyunun düz mesh'i **kapsamayı** (maske) ve **taban düzlemini** tanımlamaya devam eder. Yoğun ızgara ya da tessellation gerekmedi. Kalan tek fark siluetler: tepe, düz suyun kapsamadığı bir yere (kıyının ötesi, ufuk çizgisinin üstü) taşamaz. Ilımlı genliklerde (toplam ≤ 0.24 yd) bu görülmez; fırtınalı genlikler istenirse yoğun ızgaralı bir replay o zaman tasarlanmalı.

**Tek yüzey tanımı:** `src/water_surface.*` (CPU referansı, testler) ve `shaders/water_surface.glsl` (GPU; birebir aynı). Parçacık `x0`'dan `P(x0) = (x0 + Σ Q·A·D·cos θ, taban + Σ A·sin θ)`, θ = k·D·x0 + faz(t) konumuna gider. Normal ve yatay Jacobian aynı türevlerden gelir. Faz host'ta double ile, sarılmayan zamandan hesaplanır: eski float saat 3600 s'de sarılıyordu, bu dalgalarda saatlik bir sıçrama olurdu. Katlanma olmaz: Σ Q·k·A > 0.95 ise Q'lar ölçeklenir. Dalga LOD'u: dalga boyu 3–8 piksel izinden kısa dalgalar söner (aliasing olmasın ve yatık bakışta kesişim yakınsasın).
Composite'te bu tek yüzeyi kullananlar:
- **Yansıma:** SSR başlangıç noktası, yansıma yönü ve Fresnel.
- **Kırılma:** kırılma dalgalanması, büyük dalgaların eğimiyle.
- **Su derinliği:** hareketli yüzeyin yüksekliği eksi tabanın yüksekliği.
- **Parıltı:** yüzey eğimi artı mikro eğim.
- **Doku, köpük ve kostik:** dinlenme konumu `x0` ile; dalgayla birlikte yörüngede gider.
- **Whitecap:** Gerstner yüksekliği / Σ A.
- **Orta ayrıntı:** 3 kısa sinüs (0.7 / 1.3 / 2.6 yd) ve doku eğimi normale eklenir; eskiden 6 sinüs vardı, uzun üçü Gerstner'le değişti.

**Değerlerin kaynağı:**
- **Ölçülen:** mesh ızgarası ve üçgen boyutları, yerçekimi 10.7283 yd/s² (birim dönüşümü).
- **Çıkarım:** "30 yd altı dalgalar mesh'te görünmez", köşelenme eşikleri (vertex/dalga boyu).
- **Sezgisel:** LOD eşiği (3–8 iz), Newton adım sayısı (3), katlanma sınırı .95, orta sinüslerin uzaklıkla sönmesi.
- **Ayar değerleri:** varsayılan dalgalar (31/19/11.5/6.5 yd; genlikler .10/.07/.045/.025 yd; yönler 0/+28/−22/+47° + rüzgâr 25°), hız çarpanı .75, dikiklik 1 (dairesel yörünge). WoW'da dalga verisi yok; bunlar sakin bir liman için seçildi, bölgeye göre değiştirilmeli. Tek bir Booty Bay görüntüsüne göre genelleştirilmedi.
- **Dikiklik tanımı:** GPU Gems'in "QkA oranı" normalizasyonu alçak dalgalarda fiziksel olmayan büyük yatay kayma üretir (31 yd'lik dalgada 0.06 yd yükseklik, 0.6 yd yatay). Burada Q = yörünge şekli (1 = dairesel trokoid); tepe keskinliği fizikteki gibi k·A'dan gelir.

**Hâlâ düz yüzey varsayan yerler (bilinçli):**
- **Kapsama:** maske ve örtülme testi taban düzleminden yapılır. Dalga çukuru kıyıda zeminin altına düşse de piksel su sayılır, derinlik 0'a kıstırılır. Hareketli kıyı çizgisi (swash) daha sonra, taban geri kazanımıyla gelecek.
- **Sis:** mask'teki taban düzleminde durur. Fark ≤ toplam genlik (0.24 yd); sis yolunda 1/255'in altında.
- **Planar ayna:** taban düzleminin aynasıdır. Hareketli yüzey yalnız normal bozulmasıyla (uv kayması) yansır.
- **Düzlük/seviye testleri:** mask'in düz konumlarıyla yapılır.
- **Parça boyutu (LOD):** düz kesişimden hesaplanır; böylece yüzey tanımı dalgalardan bağımsız kalır.

**Debug görünümleri:**
- **5 BASE SURFACE:** dünyaya sabit 1 yd ızgara; yeşil = doğrulanmış düzlem, mavi = başka seviye.
- **6 DISPLACEMENT:** r = yükseklik, g/b = yatay yörünge (.5 + 2·yd, ±0.25 yd tam ölçek).
- **7 LARGE WAVES:** rg = Gerstner eğimi × 5, b = Jacobian.

**Maliyet (ÖLÇÜLDÜ, `tools/water-preview.py`, GTX 1660 Ti, 1080p, capture `18039-1`, karenin %63'ü su):**
- Yüzey (kesişim + normal): 0.236 → 0.339 ms (debug 7 erken dönüşü), yani **≈ 0.10 ms**.
- Composite'in tamamı, whitecap kapalı: 0.868 → 0.818 ms. Fark ölçüm gürültüsü; SSR ışınları değiştiği için yön belirsiz.
- Whitecap açık: +0.13 ms, çünkü gecikmeli tepe örnekleri 3 × 4 dalga.

**Testler:**
- `water_surface` (ctest):
  - dalga yokken tam düz;
  - analitik normal ve Jacobian = sonlu farklar (1−cos 3e-8);
  - dalga ayarlanan hızda ilerler;
  - 3600 s'de faz sıçraması yok;
  - ×10 genlikte katlanma yok (en düşük Jacobian .23);
  - LOD;
  - 90°'den 1.5°'ye kadar kesişim hatası < 0.2 mm (eski sabit nokta iterasyonu 6°'de 3.4 cm kalıyordu);
  - dünyaya sabitlik: üç kamera aynı yüzey noktasını görür.
- `water_vulkan_smoke`:
  - GPU'nun DISPLACEMENT görünümü her su pikselinde CPU referansıyla aynı (5.426 piksel, en çok 1/255 fark; faz işareti mutasyonu 127/255, Newton'suz 7/255 ile başarısız);
  - `wave_height 0` = tam düz.
  Mevcut piksel-kesin yansıma değişmezleri dalgalar açıkken de geçer; 64-bit Khronos validation temiz.
- `inspector_test`: dalga ayarlarının gidiş-dönüşü, bozuk satırın bütün yuvayı koruması.

## MODERN stil (29 Eylül 2026)

Referans analizi: [water-references.md](water-references.md). CLASSIC yol aynen korunur (SSR yürüyüşü ve köpük ortak fonksiyonlara taşındı; üç gerçek capture'da önce/sonra bit bit aynı). MODERN yol, aynı yüzey kesişiminden sonra şu katmanları kurar:

1. **Çok ölçekli normal.** Büyük = Gerstner yüzeyi. Orta = dalgacık dokusunun zıt yönde kayan iki örneği (Crest); ikincisi 67° döndürülür (dokunun dalgacıkları rüzgâr yönlü, aynı yönde iki kopya uzakta paralel bant yaptı), dünya gürültüsüyle detile edilir (LesusX). Orta dalgacıkların karo boyu piksel iziyle oktav oktav büyür (7, 14, 28… yd; piksel ~6 texel kaplar): mip filtresi uzak dalgacıkları düzleştirince uzak deniz mat görünüyordu (capture 35169-1), Crest de normal haritasını LOD ile ölçekler. Mikro = 1.6 / .9 yd karolar; mesafede mip'in ortaladığı mikro eğim varyansının yarısı pürüzlülüğe eklenir (Toksvig benzeri; tamamı güneş yolunu tek bir düz lekeye çevirdi: SEZGİSEL).
2. **Fresnel.** `R0 + (max(1−α, R0) − R0)(1 − N·V)^5`, R0 = .02 (n ≈ 1.33, ÖLÇÜLMÜŞ), pürüzlülüğe duyarlı tavan (Lagarde). CLASSIC'teki %30 yansıma tabanı, doygunluk ×1.25 ve parlaklık artırması yok. Aşağı bakınca su gövdesi, yatık bakışta yansıma.
3. **Yansıma hiyerarşisi.** Planar (mod 2, doğrulanmış seviye: tam ayna) → SSR (planar'ın eksik kaldığı yer; mod 1'de her yer) → pürüzlülükle yayılmış gökyüzü belleği. SSR bulanıklığı ve güveni pürüzlülüğe bağlı. Şartnamenin "SSR → planar" sırasından bilinçli sapma: planar görüntü doğrulanmış düzlemin tam aynasıdır, SSR yaklaşık (kalınlık tahmini, ekran kenarı); SSR yalnız planar'ın boş kaldığı pikselleri doldurur. Yansıyan renk LDR'dir: parlak uç yaklaşık HDR'ye genişletilir (luma'da ters Reinhard, beyazda ×1.8, SEZGİSEL) — kırpılmış gün batımı ışıltısı Fresnel ağırlığıyla sönük kalıyordu.
4. **Güneş.** Oyunun ışık yönü (`GameSun.toward`, 100 ms'de bir okunur; hava durumuyla kısılır; ufkun altındaysa yok). Işık bloğu, "gökyüzü adresi" değil: sis capture'larında ekranda izlenen disk ışık yönünün 2° üstündeydi (23634-1: 22.6° / 20.7°), adres ise ~53°'de (ÇIKARIM). Tüm normal üzerinde GGX lobu (ışıma × π, güç = SUN GLITTER); dünya-uzayına sabit, zamanla parlayıp sönen .4 / .17 yd hücreler seyrek parıltı verir (piksel hücreyi kapsayınca 1'e ortalanır, SEZGİSEL); renk tonunu koruyan yumuşak omuz (LDR kırpma turuncu yolu düz sarı-beyaza çeviriyordu).
5. **Su gövdesi.** Görüş ışını boyunca su içindeki yol (yüzey → sahne derinliğindeki taban; taban yoksa derin), kanal başına Beer–Lambert. `T = 1`: suyun hiç olmadığı taban (oyunun karışımı geri alınarak kurtarılır), `T = 0`: oyunun su rengi **dokusuz** (aşağıda). Kırmızı önce gider: sığ su taban üstünde berrak mavi-yeşil. Ton seçimi gövde rengine uygulanır.
6. **SSS.** Güneşe karşı bakınca ince dalga tepelerinden geçen ışık (Crest'in terimi): `güneş·pow(max(0, L·−V), 4)·tepe·(1 − (V·yukarı)²)`, suyun kendi rengiyle.
7. **Kırılma (katman 2).** Taban, normalin eğimiyle kaydırılarak örneklenir: kalın suda daha çok, uzakta daha az. Suyun önündeki bir şeye ya da su dışına düşen örnek kullanılmaz (Crest/Mistral sızıntı reddi); taban o pikselin kendi W ve alfasıyla kurtarılır.
8. **Köpük ve kostik.** Kıyı köpüğü CLASSIC yöntemiyle (MODERN köpük dokusuyla); whitecap'ler Jacobian'dan (aşağıda); kostik Crest yöntemiyle güneşe projekte.

### Kıyı çizgisi ve suyun üstündeki efektler (9 Ekim 2026)

Oyunda (Booty Bay, oturum 39664) iki bozulma vardı. Sis yakalaması (`fog-capture-39664-1`) ikisinin de sisten önce, su composite'inde oluştuğunu gösterdi.

- **Kıyıda siyah/kırmızı çizgi.** Kayanın su düzleminden biraz önde kaldığı 1–2 piksellik bantta oyunun suyu derinlik testini kaybediyor. Bizim testimiz 2e-5'lik tolerans yüzünden geçiyordu. Orada olmayan suyun karışımı geri alınıyor, `a·W` kayadan çıkarılıp 1/(1−a) ≈ 5 ile büyütülüyordu: koyu kayada siyah, parlak kayada doygun kırmızı. Artık **`gameWaterAt`** testi var: oyun suyu çizdiyse geri alınan taban rengi her kanalda [0,1] içinde olmalı (pay .06; 8 bitlik yuvarlama × 1/(1−a) ≈ .01). Olmayan pikseller oyunun çizdiği gibi kalıyor. Kırılma örneği de aynı testten geçiyor. CLASSIC değişmedi.
- **Balık sürüsünün etrafında mor halka.** Efekt (additive, derinlik yazmıyor) aynı pass'te sudan sonra çiziliyor, composite onu deniz tabanı sanıyordu. Büyütülüp soğurmayla boyanınca mavi/mor çıkıyordu. Artık aynı pass'te su çizildikten sonra gelen TRANSPARENT/ADDITIVE draw'lar sis kapalıyken de bekletiliyor (`fog_defer_candidate`, `Command::waterSeenInPass`), su composite'inden hemen sonra yeniden çiziliyor (`clear_target`, `[WATER-PERF] heldOverWater=`). `transparent_fog` açıksa sisten sonra çiziliyorlar (artık varsayılan). Su draw'larının kendileri hiç bekletilmiyor.
- **Yeniden çizimde VUID-08608.** `replay_app_draw` önce pipeline'ı bağlayıp sonra dinamik durumu kuruyordu. `WITH_COUNT` viewport iki durum olarak kurulduğu için, statik viewport'lu pipeline'da validation hatası çıkıyordu (sisin `transparent_fog` yolunda da). Artık sıra önce durum, sonra pipeline.
- **`exports/water-capture-request`:** dosya oluşturulunca bir sonraki su pass'i yakalanıyor (30 pass'te bir bakılıyor), menüye gerek yok.
- **İkinci tur (yakalama `48451-2`, uzaktan istekle alındı).** Kıyıda hâlâ ince turuncu bir kenar ve su altındaki kaya boyunca dikey turuncu çizikler vardı.
  - *Kenar:* `gameWaterAt` parlak kayayı ayıramıyor. Kaya `a·W`'den her kanalda parlaksa geri alınan renk makul bir taban gibi görünüyor ([117,79,51] → [.96,.42,.16]). ÖLÇÜLDÜ: oyunun suyu tam olarak sahne − su ≥ 0 olan yerde başlıyor; −3e-7 farklı piksel bile kayanın kendisi. Derinlik toleransı 2e-5'ten **2e-7**'ye indi (~3 D24 adımı). Bu üst seviye testte olduğu için CLASSIC'i de düzeltiyor. Diğer yakalamalarda değişen pikseller (8–151) yalnızca bu kıyı bandı.
  - *Çizikler:* taban kostikleriydi. Desen dünya xy'sine bağlı olduğu için dik yüzeylerde sütun boyunca uzuyordu. Ayrıca su çizgisinde en güçlüydü ve tabanı yer yer siyaha çekiyordu (×2.4).
    - Artık tabanın eğimi sahne derinliğinden (±3 px) hesaplanıyor; kostikler 70°'den 37° eğime doğru açılıyor.
    - İlk ~1 yard suda kostik oluşmuyor.
    - Çarpan ×1.5'e indi; ışık en çok %45 karartıp %120 aydınlatabiliyor (SEZGİSEL / AYAR).
  - Test: kaya levhasının rengi (.3,.3,.8). Geri alınınca makul bir taban veriyor (.86,.86,.43), yani onu yalnız derinlik testi ayırabiliyor. Eski 2e-5 toleransıyla 275 piksel bozuluyor ve test başarısız oluyor.

Testler (`water_vulkan_smoke`, 32 ve 64 bit, Khronos validation 0 hata):
- Sudan sonra çizilen additive efekt, composite'in üstüne aynen ekleniyor: 5426 su pikselinde en kötü 1/255. Bekletme kapatılınca test başarısız oluyor.
- Su yüzeyinin 0,5 mm üstündeki bir kaya levhası (tolerans içinde ama oyunun suyunun önünde): suyun dokunmadığı 11233 pikselin hiçbiri değişmiyor. `gameWaterAt` kaldırılınca 275 piksel bozuluyor ve test başarısız oluyor.

### Oyunun dokusu kaldırıldı, whitecap'ler Jacobian'dan (8 Ekim 2026)

Oyunda (Booty Bay, gün batımı, yakalama `13655-5`) MODERN su "yarım" görünüyordu. Üç neden vardı:
- **Eski katman + yeni shader.** Oyun 29 Eylül tarihli `.so` ile, binding 10'u (MODERN dokuları) okuyan yeni `.spv`'yi birlikte yükledi; dokular bağlı değildi, yüzey cam gibi düzdü. Yeniden derleme + oyunu yeniden başlatma gerekir.
- **Derin renk oyunun dokulu suyuydu.** `T = 0` ucu sahnenin oyunun karışımı (`sceneUnder`) idi, WoW ocean dokusunun beyaz hücre/parıltı deseni her yerden görünüyordu. Artık maske pass'inden sonra su renginin (`waterColor`) **mip zinciri** kuruluyor (1×1'e kadar tam zincir, 2:1 lineer blit, `water_color_mips`). Composite, texel'leri ~6 yard suyu kaplayan seviyeyi okuyor. Seviye, pikselin bakış ekseni boyunca uzamamış ayak izine göre seçiliyor; iki seviye arasında kübik B-spline (4 bilinear örnek) ile okunuyor. Sonra ortalanmış alfaya bölünüyor, çünkü zincir suyun çevresindeki boş texel'leri de ortalıyor. ÖLÇÜLDÜ (32267-2, 13655-5, 56682-1): dokunun parlaklık sapması 4 yardlık pencerede 4–6 kat düşüyor; geride kalan oyunun geniş ışığı, o korunuyor. Sonuç: bölge tonu, günün saati ve oyunun güneşe doğru geniş parlaması kalıyor, doku deseni gidiyor. İlk sürüm sabit bir ekran seviyesi kullanıyordu (yüksekliğin 1/17'si). Oyunda suya tepeden ve yakından bakınca doku desenleri ekranda büyüyor, o seviye onları silmek yerine büyük, köşeli, bulanık lekelere çeviriyordu (oturum 32267). Bilinear büyütme de blok kenarı bırakıyordu. SSS de bu renkle. CLASSIC değişmedi. Bu oturumdan önceki shader ile üç gerçek yakalamada 0 piksel fark var; whitecaps 1 ile ölçüldü, köpük yolu da çalıştı.
- **Whitecap lekeleri.** Yükseklik eşiği her yüksek noktayı köpürtüyordu: uzakta beyaz boya lekeleri (13655-1), sakin limanda tek tük beyaz benekler (13655-5). MODERN artık Crest/Tessendorf yöntemini kullanıyor: yüzeyin sıkıştığı yerde (yatay yer değiştirmenin Jacobian'ı < 1) ve .7 / 1.4 s sonrasında, dalgaların ulaşabileceği en büyük sıkışmaya göre oranlanmış hâlde köpük çıkıyor. Miktar dalga dikliğiyle (Σ k·A) kapılanıyor: gerçek dalgalar ancak dikken kırılır (tek dalga için Stokes sınırı k·A ≈ .44). WoW'un varsayılan sakin dalgası (Σ k·A ≈ .09) hiç köpürmez, dalga yüksekliği 2 (~.18) iz kadar, 4 (~.37) tam köpürür. Eşikler AYAR değeri.
- **REFLECTION görünümü (4)** MODERN'de de çalışıyor (önceden MODERN'de son görüntüyü gösteriyordu). **WATER TINT (21)** dokusuz su rengini gösteriyor.
- **Not:** ilk oturum karesindeki otomatik yakalamalarda (`-1`) kameranın önündeki su parçası bazen maskede yok (13655-1, 48186-1). Sonraki karelerde (13655-3/-5, kare taraması) tüm su draw'ları maskede. Oyunda görülen durum değil.

Test: `water_vulkan_smoke` alfa-blend'li (WoW gibi, mod 1) düz renkli test suyunda WATER TINT her su pikselinde suyun rengini veriyor (en kötü 0/255). Alfa bölmesi kaldırılınca test başarısız oluyor (en kötü 209/255). 8 piksellik dama desenli test suyunda (oyunun dokusunun yerine) ton her yerde desenin ortalaması (191 ± 1). Eski sabit ekran seviyesiyle 157..225 çıkıyor ve test başarısız oluyor. 64-bit Khronos validation ile 0 hata.

**Görsel sonuç (gerçek capture'lar üzerinde önizleme, oyunda henüz görülmedi):** güneş önde iken suyun üstünde turuncu, dalgacıklarla kırılan, seyrek parıltılı bir güneş yolu (CLASSIC'te yok); gün batımında uzak deniz gökyüzünün rengini alıyor, liman suyunda büyük ölçekli dalga şekli yansımada görünüyor; kıyıda sığ su daha berrak. Zayıf kalanlar: köpük CLASSIC'le aynı, gece sahnesinde (ay) güneş terimi test edilmedi, kostik güneşe projekte değil.

**Maliyet (ÖLÇÜLDÜ, `tools/water-preview.py`, GTX 1660 Ti, 1080p, 18039-1; ±0.1–0.2 ms gürültü):** CLASSIC 1.09 ms, MODERN 1.05 ms; yansıma (SSR) ≈ 0.49 ms, köpük ≈ 0.07, güneş ≈ 0.02, gövde/soğurma/normaller gürültü içinde.

**Testler:** `water_surface` (ctest) Fresnel (R0, tavan, monotonluk, CLASSIC tabanın altıda birinin altında), Beer–Lambert (kırmızı önce, yol ile çarpımsal, clarity), piksel pürüzlülüğü. `water_vulkan_smoke`: GPU'nun FRESNEL ve ABSORPTION görünümleri her su pikselinde CPU referansıyla (`water_shading.cpp`) aynı (0/255 ve 1/255; clarity ölçeği ve Fresnel üssü mutasyonları 23/255 ve 20/255 ile başarısız), A/B: iki stil yalnız su piksellerinde farklı. Tüm eski piksel-kesin değişmezler MODERN varsayılanla da geçer. `inspector_test`: `style`, `absorption`, `sun_fallback` gidiş-dönüşü.

## Bilinen WoW su verisi

- Su VS UBO'su set 2/binding 0, 896 bayt; `c0..c3` bayt 256'dan (D3D tarzı +Z ileri, 0..1 projection: P22 ≈ 1.00025, P23 ≈ −0.20005), `c5..c8` bayt 336'dan (afin ortonormal, det ≈ −1): `t = c6·y + c5·x + c7·z + c8`, `clip = c1·t.y + c0·t.x + c2·t.z + c3·t.w`. Girdi uzayı z-yukarı WoW dünyasıdır. Viewport `[0.5, 1080.5, 1920, −1080, 0, 0.94]` (negatif yükseklik, maxDepth 0.94). Scene depth D24S8. DXVK dynamic rendering (VkRenderPass yok).
- Su draw'ı: VS 21.332 bayt, FS 16.312 bayt; topology triangle strip, depth testi LESS_OR_EQUAL, depth yazımı kapalı, blend açık (zemin draw'ları tersi: yazım açık, blend kapalı).
- Su alfası ≈ 0.75 (derinliğe göre değişir). DXVK descriptor buffer kullanırsa Inspector çözemez; launcher `dxvk.enableDescriptorBuffer = False` verir.
- Su draw'ı derinlik yazmaz, blend açık, depth-clamp içeren raster zinciri vardır (pipeline uygunluğunda `depth-clamp` reddi buradan gelmişti; şimdi desteklenir).
- Kamera matrisinin girdi uzayı (dünya, z-yukarı), diğer draw'ların aynı `P·M·x` uzayında olduğu (planar için) ve draw-anı değerlerinin pass-sonu GPU snapshot'ına eşitliği hâlâ yalnız görsel olarak doğrulanmıştır (Normal / Water Depth / Mask görünümleri).

## Tanılama ve log satırları

`~/vulkan-depth-probe.log` (launcher varsayılanı):

```bash
rg "WATER-|water:|FRAME-PERF|WATER-PERF|WATER-BLEND" ~/vulkan-depth-probe.log
```

| Satır | Anlamı |
|---|---|
| `[WATER-REFLECTION] mode=… passes=… waterDraws=… reflectedDraws=…` | ilk 4 pass, sonra her 600 pass |
| `[WATER-PIPELINE-REJECT]` / `[WATER-PIPELINE-SKIP]` | pipeline uygun değil / özel pipeline yaratılamadı, `reasons=…` ile |
| `water:Water replay blocked: …` | tüm engeller tek satırda (push range, layout, kaynak image vb.) |
| `[WATER-BLEND] … src=… dst=… -> mode N` | oyunun su karışım denklemi (taban netliği için) |
| `[WATER-SETTINGS] loaded …` | kalıcı ayarların yüklenmesi |
| `[WATER-TEXTURE] …` | doku dosyası sorunu (düz yedek) |
| `[FRAME-PERF] fps=… avg=… worst=… water=ssr\|planar\|off layers=… quality=… overlayGpu=…` | her 300 karede present-to-present süresi |
| `[LAYER-PERF] per frame: present hook … submit hooks … draws …` | katmanın present/submit yolundaki bekleme ve sürücü süreleri |
| `[PERF-AB] phase N …` / `summary` | menüdeki PERFORMANCE TEST |
| `[WATER-PERF] cpu record avg … \| gpu avg … = mask+planar … + sceneCopy … + skyMemory … + composite …` | her 300 kayıtta CPU süresi ve GPU zaman damgaları (bir yürütme sonra okunur, hiç beklenmez) |
| `[WATER-CAPTURE] recorded/written dir=…` | debug capture |

## Debug capture

`DEPTH_PROBE_WATER_CAPTURE=1` (launcher varsayılanı) her mod/görünüm için ilk pass'i, en çok 4 tane, `exports/water-capture-<pid>-<n>-mode<M>-view<V>/` altına kaydeder (~90 MB; menüde *Capture Debug Data* ile de). İçerik (ham, sıkı paketli, satır 0 önce): `camera.bin`, `mask.bin` (RGBA32F view konumu + alfa), `mask_depth.bin`, `water_color.bin` (RGBA16F), `planar_color.bin`, `planar_depth.bin`, `sky_memory.bin`, `scene_before/after.bin`, `scene_depth.bin` (D24 → 32 bit kelimenin alt 24 biti), `water_ubo.bin`, `water_vertices.bin`, `water_indices.bin`, `shaders/` (kullanılan SPIR-V'ler), `capture.json` (parametreler, bölümler, pass draw listesi ve her draw'ın rolü/nedeni), `README.txt`. Yakalamalar büyüktür; önizleme için birkaç tanesi yeter, gerisi silinebilir.

## Araçlar

- `tools/water-preview.py <capture> [--time … --foam … --clarity … --blend-mode 1 --crop … --debug 6 --wave-height 2 --default-waves]`: yakalanmış kareyi (gerçek WoW veya sentetik) gerçek GPU'da composite shader'ından geçirip PNG yazar ve composite süresini ölçer (`tools/water_preview.cpp`, ilk kullanımda derlenir). Planar için gereken pass'ler yoktur (SSR). Kayıtlı capture'larda `water_color.bin` yoksa blend modu 0 kullanın. Dalgalar capture'daki paketten gelir; dalgalardan önceki capture'larda `--default-waves` katmanın varsayılan dalgalarını aynı C++ `pack_waves` ile koyar, `--wave-height` genliği ölçekler.
- `tools/make-synthetic-water-capture.py <dizin> [--height --pitch --far --sea-depth --sun-elevation]`: herhangi bir kamera için sahte kıyı yakalaması (yakın yukarıdan bakış, uzaktan kıyı, açık deniz, güneşli gökyüzü).
- `tools/generate-water-textures.py [çıktı] [--preview DIR]`: `water_detail.bin`'i üretir.
- `tools/reflection-input-report.py`: bilinen su UBO penceresini raporlar; kasıtlı olarak muhafazakârdır (yalnız bilinen pencereler).
- `hook_bench` (isteğe bağlı CMake hedefi): hook başına maliyet (~100 ns/çağrı).

## Performans

**Gerçek oyun verisi (21 Eylül, 5 oturum, 339 `[FRAME-PERF]` penceresi, GTX 1660 Ti, 1080p, hep SSR açık):** su GPU maliyeti pencere başına 1.2–5.7 ms (ortalama ≈ 3 ms): composite 0.8–4.5 ms (asıl kısım; HIGH kalitede ortalama 3.1 ms, MEDIUM'da 2.2 ms), mask+kamera 0.35–1.0 ms, sahne kopyası 0.06 ms, gökyüzü belleği 0.01 ms; CPU kaydı 0.2–0.9 ms/kare. Kare süresi 5–32 ms arasında oynuyor (fps 31–197; WoW ön planda 200 fps'te sınırlanıyor, başlangıç sahnesi hep ≈197). En yavaş pencerelerde (fps ≈ 43, kare ≈ 23 ms, saatlerce sabit) su GPU'su yalnız 4–5 ms; yani **su tek başına 180 → 40'ı açıklamıyor**, kalan ≈ 18 ms suyun dışından geliyor (oyunun kendi maliyeti, overlay efekti veya katmanın present/submit yolundaki beklemeler). Su kapalı (OFF) karşılaştırması log'da yoktu.

**İkinci log (LAYER-PERF ile):** katmanın present hook'u 0.23–0.26 ms, submit hook'ları 0.09 ms, mutex beklemesi 0.00, fence beklemesi 0.00, overlay GPU 0.03–0.06 ms: yani **kilitler, fence'ler ve overlay suçlu değil**. Ama kare süresi kare başına draw sayısıyla neredeyse doğrusal: 706 draw → 94 fps (10.6 ms), 1103 → 65, 1340 → 54, 1717 → 43 fps (23.3 ms); eğim ≈ **12.5 µs/draw** (oyun + DXVK + sürücü + katman). Yani sorun CPU'ya bağlı, draw sayısıyla ölçekleniyor; 180 fps yalnız az draw'lı sahnelerde (başlangıç/yükleme) görülüyor.

**Katmanın payı (gerçek NVIDIA sürücüsünde, `water_vulkan_smoke`, 1700 draw'lık pass):** kayıt sırasında draw başına ≈ 1.2–1.8 µs (5 hook'lu kayıt) ve **submit sırasında ≈ 3.7 ms (2.2 µs/draw)**: `submitted_frame` her submit'te her draw'ı kopyalayıp `resolve_resources` ile descriptor'larını çözüyordu; gerçek WoW'da (draw başına çok texture/set) daha da pahalıdır. Bunların sonucu yalnız Inspector pick/highlight, explorer capture/isolation/sayfası veya suyun kendi draw'ları için kullanılıyor. Düzeltme: **draw anlık görüntüsü ve submit'teki draw çözümleme artık yalnız birinin ihtiyacı olduğunda yapılır** (`draw_records_wanted`: pick/highlight/capture/explorer; `water_wants_draw`: SSR'da yalnız su shader çiftinin draw'ları, planar'da hepsi). Layout olayları her zaman işlenir. Ölçülen: submit ek maliyeti 3.66 ms → 0.007 ms, kayıt 1.2–1.8 → 0.33 µs/draw (1700 draw'lık karede ≈ 6 ms → ≈ 0.6 ms). Gerçek oyunda etkisi henüz doğrulanmadı.

Ölçüm araçları:

- `[FRAME-PERF]` + `[LAYER-PERF]` (her 300 kare): `present hook` (mutex bekleme, overlay, fence beklemeleri, sürücünün present'i), `submit hooks` (mutex bekleme, sürücünün submit'i), kare başına draw ve submit sayısı.
- **PERFORMANCE TEST** (Water Reflections menüsünde satır 27): hareketsiz dururken menüyü kapat; layer ≈ 40 sn boyunca beş aşamayı kendisi dener (yapılandırılmış → su kapalı → + overlay efekti kapalı → + komut takibi kapalı → yapılandırılmış) ve `[PERF-AB] summary: … water replay costs X | overlay effect costs Y | command tracking costs Z` yazar (kare başına draw sayısıyla birlikte). Ayarlar kaydedilmez; oyun yüklenirken/hareket ederken yapılmamalıdır.
- Launcher `DEPTH_PROBE_GPU_TIMING=1` verir (`overlayGpu` alanı).
- `hook_bench` (gerçekçi çok-draw'lı kayıt senaryosu) ve `water_vulkan_smoke` ("recording …", "submitting …" satırları) katman maliyetini ölçer.
- Katmanı tamamen ayırmak için: launch options'ı boşaltıp aynı yerde fps'e bak; launcher'ın `dxvk.enableDescriptorBuffer = False` ayarının DXVK'nın CPU maliyetine etkisi ölçülmedi (Inspector descriptor set'lerine ihtiyaç duyduğu için verilir).

Yapılan iyileştirmeler: composite `game_opacity` yumuşatması 16 → 8 kademe (döndürülmüş halka, mask yerine 8 baytlık `waterColor` alfası): kullanıcının ayarlarıyla gerçek yakalamada composite 2.5 → ≈ 2.0 ms; SSR adım konumunda `pow()` kalktı. Önceki turlarda giderilenler: dinamik state kayıtları için `std::map<string,std::function>` (her `vkCmdSet*`'te heap tahsisi) → düz veri; her çağrıda `vkGetDeviceProcAddr` → cihaz başına bir kez; ~170 karo için tam `resources_ready` doğrulaması → "ilk karoyla aynı durum" hızlı yolu; aynı durumlu draw'larda set/push/pipeline/viewport bağlamayı atlama; hook aramaları için önbellek. Ölçülen maliyet dağılımı (tools/water-preview.py, gerçek yakalama, HIGH, kullanıcı ayarları): `game_opacity` halkası ≈ 0.9 ms (şimdi ≈ 0.4), HIGH → MEDIUM −0.6 ms, dalga dokusu detayı ≈ 0.7 ms, kostik ≈ 0.4 ms, taban netliği/parıltı ≈ 0.

## Testler

`ctest` (32-bit `build32`) ve 64-bit doğrulamalı GPU testi:

```bash
cmake --build build32 && ctest --test-dir build32 --output-on-failure
cmake -S . -B /tmp/depth-probe-water-build64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEPTH_PROBE_BUILD_TESTS=ON && cmake --build /tmp/depth-probe-water-build64
DEPTH_PROBE_SMOKE_VALIDATION=1 /tmp/depth-probe-water-build64/water_vulkan_smoke     # Khronos validation, gerçek GPU
```

`water_vulkan_smoke`: çok karolu deniz, 7 command buffer'ın tek batch'i paylaşması, dynamic rendering varyantı (piksel-özdeş), bloklanmış replay, capture doğrulaması (tüm bölümler), gökyüzü belleği öğrenmesi, tüm katmanlar yalnız maskeyi değiştirir, `DEPTH_PROBE_WATER_FREEZE=1`, doku dosyası ve düz yedek, `[WATER-PERF]` satırı, 172 draw'lık CPU maliyeti. `inspector_test`: ayar ayrıştırma/kalıcılık, `WaterLook` adımları, köpük saati, push/target gate'leri. `discovery_test`: pipeline uygunluk nedenleri, `WaterRestore` kaydı. `water_spirv_test`: SPIR-V enstrümantasyonu (SPIRV-Tools ile doğrulama; gerçek WoW çifti için `water_spirv_test <vs.spv> <fs.spv>`). Sentetik test süreleri WoW FPS ölçümü değildir.

## Bilinçli sınırlar

Planar yalnız aynı pass'in opak draw'larını çizer (gökyüzü/bulut/blend'li katmanlar, başka pass'ler yok); kırılma ekran-uzayıdır (yalnız başka su pikselleri örneklenir); birden fazla **yatay** su seviyesi desteklenir (26 Eylül 2026: Shadowglen'deki iki gölet z 1306.97 ve 1308.36'da; önceden yalnız ilk tile'ın seviyesi kabul edildiği için ikincisi bütünüyle atlanıyordu, yakalama `26156-2`). Doğrulanan düzlemin dışındaki bir su pikseli, komşu su pikselleri aynı dünya yüksekliğindeyse kabul edilir (eğimli su ve seviye basamakları hâlâ oyunun görünümünde kalır); derinlik o pikselin kendi yüzey yüksekliğinden hesaplanır; planar ayna yalnız doğrulanan seviyeyi yansıtır, diğer seviyeler planar modda da SSR ile yansır. Test: `water_vulkan_smoke` ikinci bir yatay gölet (z .4) efekt alır, eğimli parça almaz (eski davranışla test başarısız). ****İlk draw kameranın üstünde (26 Eylül 2026, Booty Bay, yakalama `39521-2`):** pass'in ilk su draw'ı denizin değil, tepedeki havuzlarındı (z 45.8/84.8/104; kamera z 14.6). Önceki kural o draw'ın ilk seviyesini düzlem sayıp kamera altında kaldığı için bütün kareyi reddediyordu: maske boş, su görünümü yok ve **sis deniz tabanına kadar iniyordu**, bakış açısına göre gelip giden. Artık maske yalnız kamera geçerliliğine bağlı, referans seviye kameranın altındakilerden seçilir; yoksa planar mod SSR'a düşer, her piksel kendi seviyesiyle işlenir. Kameranın üstündeki (alttan görülen) seviyeler oyunun görünümünde kalır. **Düzlük testi eğimle:** komşu yükseklik farkı ≤ 0.004 + derinlik hassasiyeti + %3 × yatay uzaklık (önceki sabit 0.02, kameraya yakın 3 px ≈ 0.1 yd olduğundan %17 eğimi düz sayıyordu); eğimin görülebilmesi için iki ekran ekseninde de su komşusu gerekir. Gerçek yakalamalarda düz suyun kaybı en fazla %0.08. Testler: ilk draw kameranın üstünde → deniz 25/25 (SSR ve planar; eski kuralla 0/25), yana %10 eğimli deniz → 0 piksel (eski toleransla 3012), tek düzlem halinde %18.75 eğimli deniz → görüntü birebir aynı. **Bilinen sınır:** farklı yönlere eğik iki üçgenin birleştiği 1 piksellik kırışık şeridi düz geçebilir (sentetik durum; WoW suyu ya düz ya tek eğimli şerit). **Tek draw'da iki seviye (26 Eylül 2026):** WoW'un ilk su draw'ı iki göleti birlikte içerebilir (yakalama `27777-1`: 240 indeksin 54'ü z 1306.97, 186'sı z 1308.36); tüm draw'ın tek seviyede olması istendiği için düzlem reddediliyor ve bakış açısına göre **iki gölette de** efekt kayboluyordu. Kamera pass'i artık referans seviyeyi ardışık ilk 3 aynı yükseklikteki vertex'ten alır (tüm vertex'ler yine okunabilir/sonlu olmalı); diğer seviyeleri composite'in yataylık testi kabul eder. Test: iki seviyeli ilk draw'da deniz ve gölet 25/25 (eski shader ile 0/25 ve 0/25); MSAA, farklı color/depth boyutu, çok queue, reusable/secondary/indirect/instanced draw, multiview, MRT, çoklu subpass, stencil/depth-bounds açık draw'lar desteklenmez; bilinmeyen shader çifti kabul edilmez (menü yalnız yukarıdaki hash'leri seçer). **VS varyantları (25 Eylül 2026):** WoW denizin bir kısmını su fragment shader'ını paylaşan başka vertex shader'larla çizer (gerçek yakalamalar: `vs-ed2bfda7fbd0b2c4`, `vs-35d602d33ba6ba90`, seçili `vs-2fce10e0177c7507`'nin yanında). Bunların ilk 8 sabit-buffer okuması (c6,c5,c7,c8 görünüm; c1,c0,c2,c3 projeksiyon) birebir aynıdır, yalnız sonrasında fazladan register (c37–c42) okurlar. Önceden bu tile'lar atlanıyordu: kameraya/mesafeye göre değişen **kare kare SSR/derinlik kaybı** (gerçek yakalama `51558-2`: maskedeki 952k su pikselinin ortadaki açık su tamamen dışarıdaydı). Artık `water_draw_shaders()` su FS'sini paylaşan ve `water_transform_signature()` (SPIR-V'den ilk 8 set2/binding0 erişim zinciri) seçili VS ile aynı olan VS'leri de su sayar; seçim, maske, düzlem doğrulama ve SSR kaydı (`water_wants_draw`) hepsi aynı kuralı kullanır. Test: `water_vulkan_smoke` ikinci tile'ı varyant VS ile çizer, maske ve SSR karesi seçili shader'la bayt bayt aynıdır (düzeltmeden önce başarısız). Açık deniz köpüğünün kalıcı tamponu yoktur. Diğer su türleri (göl/nehir/farklı okyanus shader'ları) desteklenmez; gerekirse `WATER-PIPELINE-REJECT` satırlarıyla incelenmelidir.

## Geliştirme geçmişinden kalanlar

Gerçek WoW denemelerinde engel sırası: pipeline reddi (`depth-clamp` recipe), sampled depth olmaması, push range aşaması (`push-range-stage`), color layout bilinmemesi (event aramasından önceki layout), tek batch sınırı yüzünden yanıp sönme (batch/slot ayrımıyla giderildi), sığ/kum gibi görünen yansıma (SSR kalınlığı adım başına derinliğe göre), kamera aşağı bakınca gökyüzü kaybı (gökyüzü belleği), köpük dikişleri (`valueNoise` koordinat yerine kafes koordinatında sarılır). **Oturum ortasında kalıcı kapanma (29 Eylül 2026, Booty Bay):** su ~5000 geçiş çalıştıktan sonra açık denize çıkılınca `water-draw[image-layout-unobserved:set1/binding0,set1/binding1]` ile durdu ve oturum boyunca kapalı kaldı (aynı mesaj bir kez loglandığı için sonrası sessizdi). Neden: kesilen (`truncated`) ya da katmanın kaydını bilmediği command buffer içeren her submit **bütün image'ların** layout kaydını siler (`accept_submit`). Render target'lar her kare bariyerden geçip kaydı geri kazanır, ama suyun statik dokuları bir daha hiç geçiş görmez. Düzeltme: replay, dokuyu draw'dan sonra aynı command buffer'da hiçbir olay değiştirmediyse descriptor'ın belirttiği layout'u kullanır (Vulkan draw anında ikisinin eşit olmasını zorunlu tutar). Draw'dan sonra bir geçiş varsa eskisi gibi o layout karşılaştırılır. Kayıt silindiğinde nedeni `[LAYOUT-RESET]` satırında yazar (cihaz başına ilk 8 kez). Test: `inspector_test` `water_layout_cases`. Sentetik test hataları: silinen `g_depth_candidates` kaydı, eksik `next_cmd_pipeline_barrier`, refraction ölçeği. İlk tanı (DXVK dinamik state üçlüsü) yanlıştı; hata nedeni gerekçeli loglama ile bulundu — bir sonraki engelde tahmin etmeden `WATER-PIPELINE-REJECT` / `water:` satırını oku.
