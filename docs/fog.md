# Hacimsel sis (froxel) - durum ve kullanım

Son güncelleme: 25 Eylül 2026. **Durum: Faz A (temporal yok) yazıldı ve GPU testinde doğrulandı; gerçek WoW'da henüz görülmedi.**

Mimari referanslar (kod kopyalanmadı): Wronski 2014 (AC IV froxel), Hillaire 2015 (Frostbite enerji korunumlu integral), Unreal'ın kameraya hizalı hacimsel sis ızgarası.

## Tuşlar ve ayarlar

**Varsayılanlar: her şey açık (8 Ekim 2026).**
- `tools/run-inspector.sh` sisi açık başlatır (`DEPTH_PROBE_FOG_START=1`, kapalı başlatmak için `DEPTH_PROBE_FOG_START=0`).
- Kod içi varsayılanlar (`FogSettings`) kullanıcının ayarladığı tam görünümdür: SHAFTS görünümü ve gölge sayfası. Bu değerler soft shadow 2 (solma .5 / 5 yd), oyun gölge filtresi ×3, kontakt gölge .5, AO .8, arazi gölgesi .4 (80 yd'den), nokta ışık gölgesi .3, sun rays 1.5, glare .7, aureole .04, otomatik pozlama, opaklık tavanı .6, ışık tavanı .8 ve WoW'un mesafe sisi kapalı (mesafeyi bizim sisimiz yapar).
- **RESET TO DEFAULTS** (sis sayfası satır 29) artık bunlara döner. Önceden hepsini kapatıyordu: oyunda "sisi açınca soft shadow gidiyor" diye görülen buydu (oturum 35912: satır 29 → `size 0.00`).
- Soft shadow'un shader yaması oyun açılırken `soft_shadows > 0` ise yapılır, sonradan açmak yeniden başlatma ister.
- `transparent_fog` da varsayılan açık (9 Ekim 2026). Bekletme mekanizması artık su için de kullanılıyor, ayrıntılar [water.md](water.md)'de.
- İlk çalıştırmada (`water.ini` yoksa) su yansıması da WoW su profiliyle SSR'de açık başlar.
- `tests/fog_vulkan_smoke.cpp`, her özelliği ayrı ölçebilmek için eski kapalı değerleri ini'sine sabitler ve varsayılanların açık olduğunu denetler.

- **Shift+F5**: sisi döndürür. Sıra: OFF → FINAL → FROXEL DENSITY → FROXEL EXTINCTION → FROXEL LIGHTING → INTEGRATED SCATTERING → TRANSMITTANCE → OFF.
  - FINAL modunda HUD açılmaz (sis HUD'un altında kalmasın). F9 ile açılır.
  - Debug görünümleri HUD'u kendiliğinden açar.
  - DENSITY ve EXTINCTION, sisin gerçekte kullandığı zamansal birikmiş sönümü gösterir (lighting hacminin alfası). 26 Eylül'e kadar tek karelik ham medium hacmini gösteriyorlardı. O hacim her kare froxel içinde kaydırılan (Halton jitter) bir noktada örneklenir. Uzak froxeller 10–20 yarda derin olduğu için duman kümelerinin kenarında her kare küme ile açık hava arasında atlıyordu. Bu, sisin kendisinde görünmeyen bir titremeydi.
- HUD'daki sis bloğu şunları gösterir:
  - mod ve froxel boyutu
  - kare başına uygulanan composite, dünya pass'i ve düşürülen sayısı
  - sisin nerede uygulandığı ya da neden uygulanmadığı
  - pass başına GPU süreleri (INJ/LIT/INT/CMP), toplam GPU ve CPU
  - `TEMPORAL OFF`
  - yoğunluk parametreleri
  - su yüzeyinin kullanılıp kullanılmadığı
- **`~/.config/vulkan-depth-probe/fog.ini`** (ya da `DEPTH_PROBE_FOG_SETTINGS`): ilk açılışta varsayılanlarla yazılır, oyun açıkken ~1 saniyede bir yeniden okunur.
  - `preset` (low/medium/high)
  - `max_distance`
  - `distribution`
  - `base_density`
  - `height_falloff`
  - `base_height`
  - `max_extinction`
  - `albedo`
  - `emissive`
  - `anisotropy` (Henyey-Greenstein g)
  - `ambient_source` - `game` (varsayılan): sisin ortam ışığı WoW'un kendi ambient rengi × `ambient_scale` (bölgeye ve günün saatine göre değişir); `manual`: `ambient`
  - `ambient_scale` - oyun renginin çarpanı (varsayılan 1)
  - `ambient` - elle renk; `game` okunamazsa yedek
  - `sun_enabled`, `sun_color` - sisteki yönlü ışık ve rengi
  - `sun_source` - `game` (varsayılan): yön her karede WoW'un belleğinden okunur; `manual`: `sun_direction`
  - `height_reference` - `ground` (varsayılan): taban bölgenin zemininden; `camera`: kameranın yüksekliğinden; `world`: mutlak z
  - `temporal_moving` - kamera dönerken/adım atarken history payı (varsayılan 0.8)
  - `sun_glare` / `sun_glare_radius` - göz parlaması: diskin görünen payıyla, önündeki engelin üstüne de (0 = yok, ~0.7)
  - `sun_rays` / `sun_rays_length` - diskten, önündeki engellerin aralarından yayılan ışın huzmeleri (0 = yok, ~1.5); yol payı (0.85)
  - `sun_aureole` / `sun_aureole_g` - güneşin etrafındaki dar ileri lob (engelin kenarından taşan ışık; 0 = yok, 0.02–0.04)
  - `game_shadows` - oyunun kendi güneş gölgesi kaskadları (1 = açık); `game_shadow_flip` - haritaların v ekseni
  - `light_limit` - fog'un eklediği ışığın tavanı (0 = yok); `auto_exposure`, `exposure_target`, `exposure_min` - otomatik pozlama
  - `max_opacity` - fog'un arkasındakini en fazla ne kadar gizleyeceği (1 = sınır yok; bkz. "max_opacity" bölümü)
  - `sun_position` - `sky` (varsayılan): gökyüzünde çizilen güneş/ay diskinin yönü; `light`: arazinin aydınlatma yönü
  - `sun_direction` - elle yön (dünya, ışığa doğru); `game` okunamazsa yedek
  - `shadows` (varsayılan 1), `shadow_length` (yarda, varsayılan 120) - sisteki güneş ve gökyüzü gölgeleri, yükseklik belleği
  - `sky_light` (varsayılan 0.5) - ortam ışığının yukarıdan gelen ve gölgelenen payı (ikinci ışık kaynağı)

## F10 menüsü ve yükseklik referansı (26 Eylül 2026)

**F10 → `[ VOLUMETRIC FOG ]`:** satıra tıkladıkça değer bir sonraki adıma geçer ve `fog.ini`'ye kaydedilir. Satırlar:
- VIEW (Shift+F5 ile aynı)
- QUALITY
- DENSITY
- HEIGHT FALLOFF
- HEIGHT REFERENCE
- BASE
- MAX DISTANCE
- FORWARD SCATTER G
- ALBEDO
- AMBIENT LIGHT (oyun renginde çarpan ×0.5…×3, elle modda seviye)
- AMBIENT COLOR (GAME ↔ FOG.INI; okunuyorsa rengi gösterir)
- SUN LIGHT (yoğunluk)
- SUN DIRECTION (SKY DISC → GAME LIGHT → FOG.INI; okunuyorsa yükseklik/azimut gösterir)
- SUN SHADOWS (KAPALI / 60 / 120 / 200 yarda)
- SKY LIGHT FROM ABOVE (KAPALI / %25 / %50 / %75)
- RESET TO DEFAULTS

Sayfa ayrıca sis tabanının o anki dünya z'sini gösterir.

**`height_reference`:**
- **`camera` (yeni dosyalarda varsayılan):** `base_height`, kameranın yaklaşık 6 saniyelik zaman sabitiyle yavaşça izlenen yüksekliğine göre bir ofsettir; varsayılan −12 yarda. Böylece sis Booty Bay'de (z ≈ 0) de Teldrassil'de (z ≈ 1300) de çevrende olur. Zıplama ve zoom sisi oynatmaz. 80 yardadan büyük sıçramada (ışınlanma, bölge değişimi) referans hemen yeni yüksekliğe geçer.
- **`world`:** `base_height` mutlak dünya z'sidir.
- Bu anahtar olmayan eski dosyalar `world` olarak okunur. Menüde referans değiştirilince sis olduğu yerde kalır: taban o anki dünya z'sine ya da kameraya göre ofsete çevrilir.
- Bedel: kamera modunda tepeye çıkınca sis tabanı da yavaşça yükselir; vadide biriken, dünyaya tam sabit bir katman değildir.

## Güneş yönü (26 Eylül 2026)

**Kaynak (GPU gerçeği):** WoW su VS UBO'sunda c49 (bayt 784), güneşin görünüm uzayındaki yönüdür; aynı UBO'nun c21..c23'ü (su kamerasının c5..c7'si) ile dünyaya çevrilir. Eski 36 su capture'ında (farklı günler, bölgeler, 21:55–02:46) dünya yönü hep −(sinφ·cos45°, sinφ·sin45°, cosφ): **x = y** (sabit 45° azimut), yükseklik oyun saatiyle değişiyor. c50/c51/c52 saate ve bölgeye bağlı renkler.

**Bellek:** `tools/find-sun-direction.py` (çalışan oyunun `/proc/<pid>/mem`'i; capture'dan referans, sonraki capture'da güneş kaydıktan sonra eleme). Sonuç: `[0x00CE04A8]` (aynı değer `[0x00AEEDDC]`) → blok:

| Blok + | UBO | Anlam (değerlerinden yorum) |
|---|---|---|
| 0x7C | c49 (görünüm uzayında) | ışığın gittiği yön, dünya, birim |
| 0x88 | c50 | ortam ışığı |
| 0x94 | c51 | doğrudan ışık rengi |
| 0xA0 | c52 | parlama/specular |

Üç capture boyunca (01:16–01:18) bellek değeri güneşle birlikte ilerledi (z −0.53496 → −0.53573 → −0.53612, bellek −0.53622). Oyun yeniden başlatılınca da tuttu (pid 35588): blok başka adrese taşındı (0x17530020 → 0x175627f8), zincir yine buldu; üç yeni capture'ın GPU yönü ile bellek ve `[FOG-SUN]` log satırları ~1e-4 içinde, güneşle birlikte ilerleyerek uyuştu, hiç `not read` yok.

**Kullanım (`src/memory_sun.*`):** sis açıkken her present'te okunur (iki küçük self-memory okuması). Birim uzunluk ve renk aralığı kontrol edilir; okunamazsa `sun_direction`'a düşülür. Güneş ufkun altındaysa yönlü terim kapanır, çünkü ışık sisi yerin altından aydınlatırdı. **Ortam rengi (26 Eylül 2026):** blok +0x88 (su UBO c50) sisin ortam ışığı olarak kullanılır (`ambient_source = game`); örnek değerler Booty Bay gece 0.16/0.24/0.35, Teldrassil 0.38/0.29/0.59 (mor-mavi). Doğrudan ışık ve specular renkleri henüz kullanılmıyor. HUD satır 18: `AMBIENT GAME r g b`. Test: yerleştirilen blokla `game` × 1.5, aynı rengin elle girilmesiyle birebir aynı (fark 0), fog.ini renginden farklı (42/255); oyun rengi yok sayılınca test başarısız. Log: `[FOG-SUN] game [0xce04a8]=… toward … elevation … azimuth … ambient … diffuse … specular …` (durum değişince ve ~15 s'de bir). HUD satır 17: `SUN GAME EL … AZ … USED`. Ortam: `DEPTH_PROBE_MEM_SUN_STATIC=<hex>[,<hex>]`, `DEPTH_PROBE_MEM_SUN_OFFSET=<hex>`.

**Testler:** `memory_sun_test` (canlı bloğun değerleri, yedek zincir, birim olmayan yön ve NaN renk reddi, kendi belleğinde güvenli okuma). `fog_vulkan_smoke`: test sürecine yerleştirilen blokla `sun_source=game`, aynı yönle `manual` ile birebir aynı (fark 0), fog.ini yönünden farklı (32/255); ufuk altı güneş = güneşsiz sis. Sis yönü yok sayılınca test başarısız oluyor (mutasyon kontrolü).

## Güneş gölgeleri (26 Eylül 2026)

**Kısıt:** hacim geçişleri (inject/light/integrate) oyunun o karedeki dünyası çizilmeden önce, kendi compute submit'imizde çalışır; o anda bu karenin derinliği yoktur. Composite ise dünyadan sonra, oyunun command buffer'ında çalışır.

**Yöntem (ekran uzayı, bir kare gecikmeli):**
1. Composite'ten hemen önce küçük bir grafik pass'i (`fog_shadow_depth.frag`) sahne derinliğini tam çözünürlükte R32F görüş derinliğine yazar (gökyüzü = uzak). DXVK'nın compute durumuna dokunulmaz. İlk denemede 4×4 "en yakın" küçültme vardı; eğik bakılan zeminde bloğun yakın satırları zemini kendi gölgesine sokuyordu (testte açık zeminde 9/255 sahte gölge), bu yüzden tam çözünürlük.
2. Sonraki karenin light pass'i (`fogSunVisibility`) her froxel'den güneşe doğru 12 adım yürür (aralık karesel, froxel ve kare başına jitter), her örneği ÖNCEKİ karenin kamerasıyla (derinliği o kareye ait) ekrana izdüşürür. Örnek bir yüzeyin arkasına, yüzeyin varsayılan kalınlığı (max(1.5, %4 derinlik)) + son adımın kat ettiği derinlik kadar geçerse gölge (SSR kesişme testi: ince duvar/gövde iki örnek arasında kaçmaz). Kameranın arkasına ya da ekran dışına çıkan örnek bilinmez sayılır → aydınlık (yalnız önceki karenin gördüğü şey gölge düşürür).
3. **Yüzeyin arkasındaki froxel'ler:** zemin bir froxel'i kesince merkezi yerin altında kalabilir; gölgesi doğrudur ama composite onu yüzeyin önündeki sise interpolasyonla karıştırır → açık zeminde yatay koyu bantlar. Froxel noktası, kendi görüş ışını boyunca görünen yüzeyin hemen önüne çekilir ve yürüyüş oradan başlar; başlangıç ayrıca %2 × uzaklık kadar ileriden (eğik açıda piksel nicemlemesi).
4. Temporal birikim kenarları yumuşatır. Güneş ufkun altındaysa ya da `shadows = 0` ise hiç yürünmez.

**Hata ayıklama:** Shift+F5 → `SUN VISIBILITY` (aşağıdaki renk anahtarı). HUD satır 19: `SHADOWS SUN 120 YD SKY 50%` ya da nedeni (`off`, `no sun`, `waiting for a depth`).

**Test (`fog_vulkan_smoke`):** güneş yakın duvarın arkasında; CPU referansına analitik görünürlük eklendi (güneşe doğru doğru parçası × duvar dikdörtgenleri). Gölgenin değiştirdiği 41 froxel-merkezi pikselde GPU'nun ortalama hatası gölgeli referansa göre 0.0045 (≈1 LSB), gölgesiz referansa göre 0.063; en kötü 0.014 (gölge kenarında froxel çözünürlüğü). Yakın duvar üstündeki sis 0.25 koyulaşıyor, güneş ışını açık zemin pikseli birebir aynı. Mutasyonlar: görünürlük hep 1 → referans kontrolü başarısız; yüzeye çekme kaldırılınca zeminde 5/255 sahte gölge → başarısız. Mevcut testler `shadows=0` ile çalışır (başka özellikleri ölçerler).

**Doğrulama katmanı:** `fog_vulkan_smoke` artık `DEPTH_PROBE_SMOKE_VALIDATION=1` destekliyor (64-bit derleme; hata sayısı sıfır olmalı). İlk çalıştırma iki eski hatayı buldu ve düzeltildi: 64-bit'te `pass_images` handle dönüşümü eksikti (derlenmiyordu), composite zaman damgaları henüz submit edilmemiş command buffer'daki sorgulardan okunuyordu (artık yalnız submit edilmiş slotlar). Şimdi 0 hata; su testi de doğrulamayla 0 hata.

**Bilinen sınırlar:** ekran dışındaki (ya da önceki karede görünmeyen) nesneler gölge düşürmez; güneşe doğru bakarken en iyi, güneş arkadayken zayıf. Tam çözüm güneşten gölge haritası (dünyayı güneşin gözünden yeniden çizmek) olur. Oyundaki GPU maliyeti HUD'da `LIT` süresine eklenir, henüz ölçülmedi.

## "Ne olurdu" yeniden çizimi ve ışın ayarı (26 Eylül 2026)

`tools/fog-whatif.py DIR --variant "ad:density=…,falloff=…,sun=…,g=…,sky=…,color=r/g/b"`: bir sis yakalamasını başka ayarlarla çevrimdışı yeniden çizer. Yoğunluk yeni ayarlardan hesaplanır, yakalamadaki gürültü deseni korunur. Güneş/gökyüzü görünürlüğü yakalamanın SUN VISIBILITY karesinden alınır (hafif bulanıklaştırılır, temporal birikim yerine). Integrasyon `fog_integrate.comp`, composite `fog_composite.frag` ile aynıdır.
- `--check`: yakalanan hacmi yeniden birleştirip oyunun `color_after`'ıyla karşılaştırır (Teldrassil yakalamasında ortalama 0.35/255, en çok 1).
- Teldrassil yakalamasından: güçlü ayarlar (yoğunluk 0.06, güneş 8) kanopi arkasındaki güneşten yayılan net ışınlar verdi. Beyaz ışınlar mor dünyada gri/yapay duruyordu, oyunun disk parıltısı rengi (leylak) oturdu. "Orta" (yoğunluk 0.04, düşüş 0.04, güneş 6, G 0.35, gök %85) doğala en yakın aday oldu.
- **`sun_color_source = game`** (varsayılan): sisteki güneşin rengi = WoW'un güneş parıltı rengi (ışık bloğu +0xA0, su UBO c52; Teldrassil leylak, Booty Bay sıcak beyaz) × `sun_color` ilk değeri (şiddet). Test: yerleştirilen blokla aynı rengin elle girilmesiyle birebir aynı, beyazdan farklı (21/255).
- **Menü `LOOK: FOREST LIGHT SHAFTS`:** yukarıdaki satırları tek tıkla ayarlar ve kaydeder. İlk sürüm yoğunluk 0.04 / güneş 6 idi. Oyunda ilk gerçek ışınları verdi (yakalama `28697-3`: kanopi açıklığından düşen leylak huzmeler, 10–30 yardada sisin %36'sı güneş alıyor), ama arka plan ormanını koyu bir perdeye gömüyordu. Işın parlaklığı yoğunluk × güneş ile, perde yalnız yoğunlukla orantılı. "Ne olurdu" karşılaştırmasında yarı yoğunluk + iki kat güneş (0.02 / 12) ışınları korudu, arka planı geri getirdi. Şimdiki değerler bunlar; SUN LIGHT adımları 18'e kadar uzatıldı.
- **Yakalama ısınması:** yakalama, sis açıldıktan sonra 60 kare bekler (oyun güneşi, bir derinlik, bellekler, temporal geçmiş). Oyundaki ikinci yakalama sisin ilk karesini yakalamıştı: gölge yok, bellek boş.

## Işık kaynağı gizmo'su, ekranda görülen güneş diski, açık gökte güneş (26 Eylül 2026)

**Işık gizmo'su** (menü `LIGHT GIZMO`, fog.ini `light_gizmo`): ekranın ortasından her ışık yönünün ekrana düştüğü yere bir çizgi ve orada bir çember çizilir. Kaynak ekran dışındaysa çizgi kenara kadar gider, uçta ok başı olur. Kaynağa tam bakınca çizgi kısalıp noktaya iner. Renkler: sarı = sisin güneşi (ışınlar buradan gelir), beyaz = ekranda görülen disk, camgöbeği = oyunun aydınlatma yönü, eflatun = `0x00D43180`. `shaders/gizmo.vert` kamera gizmo'sunun 24 köşesinden sonra 4 × 40 köşe; dünya yönleri overlay uniform'unda, izdüşüm `gizmoVP` × (yön, 0).

**Görülen disk** (`fog_sun_find.comp`): Booty Bay yakalaması (`30307-2`), ışınların diskin ~350 px altındaki bir noktadan yayıldığını gösterdi. Görünen disk 73.9°'de, `0x00D43180` 55.6°'de, ışık bloğu 34.8°'de; üçünün azimutu da 45°. Teldrassil'deki "eşleşme" tesadüftü. `0x00D43180` Booty Bay'de 60 s'de hiç kıpırdamadı. Bellekte x = y taraması yalnız mesh normalleri buldu.
- Çözüm, oyunun gökyüzüne çizdiği diski görüntüden bulmak. Composite'te oyunun resmi (sisten önce) çeyrek çözünürlüğe blit edilir.
- Sonraki karenin compute'unda görüş derinliği gökyüzü olan pikseller arasından beyazımsı ve parlak olanlar sayılır: atomik toplamlar, slot başına 8 kelime, host-görünür tampon.
- Host, slotun fence'i sinyallenince okur. Kompakt bir küme ise (4–4000 texel, göğün yarısından az, yayılım ≤ 25 texel) merkezi o karenin kamerasıyla dünya yönüne çevrilir; ufkun altı reddedilir, küçük sıçramalar yumuşatılır.
- `sun_position = sky` iken sırası: görülen disk (son 20 dakika) → `0x00D43180` → ışık bloğu → fog.ini.
- Işınlanmada unutulur. Işınlanmadan önce yapılmış ama sonra okunan aramalar bir "epoch" ile atılır; bu olmadan eski bölgenin diski geri geliyordu, testle bulundu.
- Log: `[FOG-SUN] disc seen at pixel … -> toward … elevation … azimuth …`. HUD satır 17: `SUN = SEEN DISC EL …`.
- Test: sahnenin göğüne beyaz bir disk çizilir, yönü 0.33° hatayla bulunur (NVIDIA'da da aynı), sisin güneşi ondan gelir; disk yokken bulunmaz; ışınlanmada unutulur (epoch kontrolü kaldırılınca başarısız).

**Açık gökte güneş** (`open_sun`, varsayılan 0.25; menü `OPEN-SKY SUN`): kanopi altında ışın veren güneş gücü (12) açık bölgede her yeri beyaz bir parlamaya çeviriyordu (Booty Bay). Işık pass'inde güneş terimi `mix(1, open_sun, skyVisibility)` ile çarpılır: gökyüzü açık olan froxel'de güneş zayıflar, kanopi altında tam kalır. Tek kural iki bölgeye de uyuyor: "ne olurdu" ile Booty Bay'de beyaz parlama kalktı, Teldrassil'in ışınları neredeyse aynı kaldı. Bölgeye göre ayar gerekmiyor. Test: açık gökte `open_sun 0.25` × güneş 4 ≈ güneş 1 (ortalama fark 0.16/255), tam güneşten farklı; çarpan kaldırılınca başarısız. Referanslı eski testler `open_sun=1` ile çalışır.

## Güneşli sabah kontrolü: sarı disk ve gökyüzü (27 Eylül 2026, Booty Bay 8:22, `fog-capture-8343-1`)

**Işınlar:** `sun_position = light` seçiliydi; fog'un güneşi oyunun aydınlatma yönüydü (yükseklik 26.8°). Görünen disk görüntüden ölçüldü: 32.6° yükseklik, 45° azimut. Aradaki fark 5.8°; ışınlar diskin hemen altından yayılıyor. `0x00D43180` 53.4° (20.8° sapma). Işık yönü WoW'un kendi gölgelerinin geldiği yön; gözle doğru olan ise diskin kendisi.

**Disk bulucu sarı güneşi görmüyordu:** oturumda 0 tespit vardı. `fog_sun_find.comp`, en düşük kanalı ≥ 0.86 olan pikselleri sayıyordu. Bu sabahın diski 255/255/190 (mavi 0.745) olduğu için eleniyordu. Tek bir gevşek eşik de yetmiyordu: sabah pusu neredeyse disk kadar parlak (1448 texel diskin renginden geçiyordu).

Çözüm iki küme:
- **Beyaz:** eski eşik.
- **Sıcak:** en güçlü iki kanal ≥ 0.975, en zayıfı ≥ 0.55. Pusta yeşil en fazla ~191, mavi ~123 kalıyor.

Host önce kompakt bir beyaz kümeyi, yoksa kompakt bir sıcak kümeyi kabul ediyor (slot başına 16 sayaç). Sonuçlar:
- Eski 21 capture'da beyaz kümenin bulduğu diskler aynı kaldı.
- Bu sabah diski 254 texel, (1118, 248)'de kompakt bulundu; görüntüdeki yeri (1120, 250).
- Log: `[FOG-SUN] warm disc seen …`.
- Test: sarı (1, 1, 0.75) disk 0.33° hatayla bulunuyor; sıcak küme kapatılınca test başarısız oluyor.

**Fog parlak göğü söndürüyordu:**
- Gökyüzü 144'ten 108'e, güneşin halesi (parlaklık > 170) 197'den 111'e düştü; dünya neredeyse değişmedi.
- Sebep: `open_sun 0.15` açık gökte güneşi kısıyor, g 0.2 ile ileri saçılma zayıf. Fog'un kendi ışığı arkasındaki gökten sönük kalıyor ve gökyüzü griye dönüyor.

**`sky_fog`** (varsayılan 1, eski davranış) eklendi: gökyüzü pikselleri fog'un bu kadarını alır (`sonuç = mix(gök, gök·T + S, k)`). 0'da oyunun göğü, pusu ve güneş halesi olduğu gibi kalır. Test: 0'da gök fog'suz kareyle birebir, 0.5 tam ortada, dünya pikselleri değişmiyor. `fog-whatif.py` `skyfog=` değişkenini ve capture'ın `skyFog` değerini kullanıyor.

**"Ne olurdu" karşılaştırması** (`exports/fog-capture-8343-1/whatif-sunny.png`; sıra: fog'suz, şimdiki, skyfog 0.4, open_sun 0.5, open_sun 0.5 + g 0.4, + skyfog 0.5):
- `open_sun 0.5` + g 0.4: sıcak altın parıltı ve güçlü huzmeler; en "güneşli sabah" görüntüsü.
- `skyfog 0.5` eklenince gökteki parıltı yumuşuyor.

## Öğle güneşi: disk bulucu yayda arar, tek tespitle zıplamaz (27 Eylül 2026, Booty Bay 9:00–9:16)

**Belirti:** Kamera her oynatıldığında sisin güneşi (gizmo'daki sarı çember) başka yerden çıkıyordu. Bir karede karakterin ayağına, ufka indi.

**Log'dan sebep** (`[FOG-SUN]`, pid 11672): güneş yükselince gökyüzü parlak sarıya döndü. Tüm gökte arama yanlış kümeler buldu:
- disk etrafındaki pus: 15×23 texel yayılım,
- ufuk boyunca bir şerit: 24×5, yükseklik 1.8°, azimut 73.8°,
- 5 texel'lik parıltılar: yükseklik 1.7°, azimut 35°,
- kompakt ama yanlış yükseklikte bulutlar: 49.5° ve 51.2°; disk ~45°'de.

Eski süzgeç 5°'den büyük farkta yumuşatmadan zıplıyordu (`keep = 0`), yani tek bir yanlış tespit sisin güneşini gökyüzünün öbür ucuna taşıyordu.

**Çözüm** (`fog_sun_find.comp`, `fog_math.hpp`):
- **Yay:** WoW diski ışığının azimutunda çizer (her capture'da x = y, 45°). Oyunun ışığı okunuyorsa yalnız o dikey yarım düzlemin ±4° bandındaki ve ufkun 3° üstündeki gök texel'leri sayılır (`FogParams::sunArc`). Bulunan yön yaya oturtulur, resimden sadece yükseklik alınır. Işık okunmuyorsa bütün gök aranır.
- **Küme kuralları** (`disc_cluster_problem`):
  - en az resmin 1/10000'i (1080p'de 12 texel; oyunda disk 119–245),
  - yayılım ≤ 6 texel,
  - şerit değil (bir eksen diğerinin 2.5 katından fazla değil),
  - göğün yarısından az,
  - kenar: kenardan 2σ uzakta (kesik diskin merkezi kayar).
- **Zaman süzgeci** (`DiscTrack`): mevcut yönün 2° içindeki tespit yumuşatır (0.8/0.2). Farklı bir yön aday olur; fog güneşi ancak üst üste 3 tespit (her biri adayın 1° içinde) gelince taşınır. Yanlış tespitler gerçek diskle araya girse de güneşi oynatamaz.
- **Diskin parçası güneşi taşıyamaz** (`disc_whole`): bir küme, diskin o konumda ekranda kaplayacağı alanın (`sun_glare_radius` yarıçapı, merkezden uzakta cos³ ile büyür) en az %40'ı ise "bütün disk" sayılır. 1080p'de ~130 teksel, bütün sayılması için ~53 gerekir. Daha küçük bir küme yalnızca mevcut yönün 2° yakınındaysa onu doğrular: yumuşatmaz, çünkü kısmi diskin ağırlık merkezi kaymıştır. Başka yerdeyse yok sayılır ("part of a disc away from the fog sun"). Takibi de başlatamaz.
  - Booty Bay (capture `71199-1`, 2026-09-27): turuncu ufkun önündeki 13 tekselli bir meşale alevi (derinliği yok, gök sayılıyor) ışığın yayında 3.5° yükseklikte üç kez görüldü. Fog güneşini ve parlamayı gerçek diskin 42° altına taşıdı. Gerçek disk geminin gövdesinin arkasından yalnızca 13–25 teksel görünüyordu.
- **Kamera dönerken aranmaz:** aranan resim bir önceki kareye ait. Kamera kareler arasında 0.3°'den fazla dönmüşse (ileri ya da sağ vektör) o karede arama yapılmaz, bakış durunca devam eder.

**Log:** `[FOG-SUN] … (on the light's arc) | since the last log: fog sun moved Nx, sightings waiting for confirmation N, rejected N spread, N edge…, searches skipped while turning N, sun map clears N`.

**Testler:**
- `fog_math_test`: log'daki gerçek kümeler (disk kabul, pus/şerit/parıltı/kenar ret), yaya oturtma, süzgeç (dönüşümlü yanlış tespitler güneşi oynatmaz, 3 tutarlı tespit taşır).
- Fog smoke (32-bit): ışık diskin azimutundayken disk 0.12° hatayla bulunuyor (yaysız 0.33°), azimut sapması 0. Işık 50° ötedeyken aynı disk bulunmuyor. Kamera kare başına 1° dönerken 10 aramanın 10'u atlanıyor. İki mutasyon (yay kapalı, dönüş kontrolü kapalı) testleri düşürüyor.

**Oyunda henüz görülmedi** (yeni build oyunun yeniden başlatılmasını ister).

## sky_fog suyun altını gösteriyordu (27 Eylül 2026, Booty Bay)

`sky_fog = 0.3` ile iskelenin su altındaki direkleri ve deniz tabanının tepeleri suyun içinden görünmeye başladı. WoW'un suyu depth yazmaz; suyun arkasında depth'te ya deniz tabanı ya hiçbir şey (gök derinliği) vardır. Fog su yüzeyinde duruyordu (su maskesi), ama "gök mü" kararı ham depth'ten veriliyordu. Arkasında geometri olan su pikseli tam fog, arkası boş olan %30 alıyordu. Aradaki sınır su altındaki silüetleri çiziyordu. `sky_fog = 1` iken fark yoktu.

Düzeltme (`fog_composite.frag`): su maskesinde yüzeyi olan piksel her zaman dünya sayılır. Test: aynı su maskesi `sky_fog 0` ve `1` ile aynı resmi vermeli; düzeltmeden önce fark 7, sonra 0. Oyunda preset değiştirilince fog kaynakları yeniden kurulur ve yeni shader restart'sız yüklenir.

## max_opacity: shaft yoğunluğu açık limanı silmesin (27 Eylül 2026, Booty Bay, `fog-capture-20013-5`)

**Belirti:** Shaft'lar ancak F10'daki SHAFTS görünümüyle (yoğunluk 0.02, g 0.35) görünüyordu, ama o ayarda liman süt gibi pusa gömülüyordu. Yoğunluk 0.01 ve g 0.2'de ise pus azalıyor, shaft'lar da kayboluyordu.

**Sebep:** Shaft'ların görünürlüğü de pusun kalınlığı da aynı yoğunluktan geliyor. Kamerada 0.0124/yd yoğunlukla 100 yd'de sisin opaklığı ~%73, adada (150–250 yd) ~%90. Uzak sahne neredeyse tamamen fog ışığıyla değişiyor. Güneşi kapatınca bile etki 30'dan sadece 24'e iniyor (/255); pusun çoğu oyunun turkuaz ambient'inin uzun mesafede birikmesi. Ambient'i yarıya indirmek pusu griye çeviriyor.

**Çözüm:** `max_opacity` (fog.ini; Unreal'deki Fog Max Opacity). Composite'te `1 - T` bu değerle sınırlanır, fog'un ışığı da aynı oranla ölçeklenir (renk korunur). Yakın fog ve shaft'lar sınırın altında kalır, uzak sahne sisin içinden görünür.
- Parametre `depthRange.z`'de taşınır; 0 gelirse (ayarı bilmeyen eski build) "sınır yok" sayılır.
- Varsayılan 1 (değişiklik yok). SHAFTS görünümü 0.6 yazar.
- what-if: `maxopacity=`; capture JSON'da `maxOpacity`.
- What-if'te 0.6 ile gemi ve ada pusun içinden görünüyor, fog etkisi 30.2'den 24.1'e iniyor.
- Test: `max_opacity 0.3`'te 117 pikselin 72'si sınıra takılıyor, CPU referansıyla fark ≤ 0.0022. Sınır kaldırılınca 0.21.

## Dünyaya sabit sis: zemin referansı, hareketli history, oyunun gölge haritaları (27 Eylül 2026, Teldrassil)

Kullanıcı: "Dolaşınca sis ve shaft'lar değişiyor; kameradan bağımsız, ışığa ve sisin yerine göre sabit olmaları gerekmez mi?" Üç kaynak bulundu, üçü de ele alındı.

**1. `height_reference = ground`** (yeni varsayılan). Eskiden taban kameranın yavaşça takip edilen yüksekliğine bağlıydı (capture 23704-1: taban = kamera − 12). Tepeye çıkınca sis katmanı da seninle yükseliyordu: aynı vadi yukarıdan sisli, içinden açık görünüyordu. Şimdi bölgenin zemin seviyesinden ölçülüyor:
- **Zemin hafızası** (`floorMemory`, ısı hafızasıyla aynı 1024² × 0.5 yd ızgara): her hücrede görülen en alçak yüzey. Kanopinin altındaki toprak ve su yüzeyi tutulur. Hiç sönmez; ızgara kayınca ya da ışınlanmada temizlenir.
- **`fog_ground.comp`:** kameranın 128 yd çevresindeki zemin hücrelerini yüksekliğe göre sayar (1 yd'lik kutular). Host bunun %20'lik dilimini (`fog::ground_level`) zemin kabul eder ve ~3 s ile takip eder. Tepelerin, çatıların üstü daha yüksekte kaldığı için sayılmaz.
- **Su:** `fog_shadow_depth.frag` artık su maskesindeki yüzeyi de derinlik olarak yazıyor. Hafızalar deniz tabanını değil deniz seviyesini görüyor. Disk bulucu da açık denizi gökyüzü sanmıyor. Su yüzeyi görüş derinliğine **negatif** yazılır (okuyanlar `abs` alır; capture'daki `view_depth.bin` için de geçerli).
- **Su seviyesi (2026-09-29, Booty Bay):** maskenin olmadığı tek bir kare deniz tabanını gösterir ve taban hafızası hiç yükselmediği için orada kalır: zemin −3'ten −11 yd'ye kaydı, sis deniz seviyesinde inceldi. Hücre bazlı "yüzeye kaldırma" işe yaramaz: aynı pikselin yüzey noktası dip noktasından daha yakında, başka bir hücrededir; dip hücrelerinin çoğuna hiçbir yüzey örneği düşmez (smoke: 1182 yüzey hücresi, dip görülünce 1942). Bunun yerine `fog_memory_scatter.comp` kameranın altındaki 128 yd içindeki su piksellerini yüksekliğe göre sayar; host, piksellerin en az %2'sini tutan en alçak kutuyu bölgenin su seviyesi olarak saklar ve zemin histogramında bunun altındaki hücreleri su seviyesine sayar. Seviye ışınlanmada ya da su en son görülen yerden 128 yd uzaklaşınca unutulur. Bilinen sınır: bir dağ gölünün yanında, göl seviyesinin altındaki kuru bir vadi de göl seviyesine sayılır.
- Zemin hafızasına sadece kameranın altındaki yüzeyler yazılır. Shadowglen'de bir sıfırlamadan sonra kamera yukarı bakıyordu; kanopinin alt yüzü "zemin" sayıldı (kameranın 59 yd üstü, 5715 hücre) ve sis tabanı karakterin üstüne çıktı. Test: hafıza boşken sadece çatıya bakınca 0 hücre, okuma yok (düzeltmeden önce zemin 49.3 = çatı).
- İlk okumaya kadar (birkaç kare) taban kamera − 5 alınır. Gölgeler kapalıysa hafıza çalışmadığı için hep bu geçici değer kullanılır.
- Menü: HEIGHT REFERENCE CAMERA → WORLD → GROUND. Sis olduğu yerde kalacak şekilde taban dönüştürülür. SHAFTS görünümü ground / −7 yazar (kamera ~5 yd yukarıdayken eski camera / −12 ile aynı yoğunluk).
- Log: `[FOG-GROUND] ground z … (reading … from N floor cells within 128 yd), water level z … (N surface pixels this reading) | no water level, camera z …, fog base z …`. Capture: `floor_memory`, JSON'da `groundZ` / `groundCells` / `waterLevelValid` / `waterLevelZ` / `waterPixels`.
- Test: smoke'ta zemin 0.09 bulunuyor (düz zemin z = 0), kamera 20 yd yükselince taban yerinde kalıyor. Zemin modu kamerayı kullanınca test düşüyor. Unit: yüzdelik, büyük/küçük vadi, az hücre.

**2. `temporal_moving`** (varsayılan 0.8). Bir froxel'in ışığı kameranın gördüğüne bağlı olduğu için, dönüş ya da adım sonrası history eski görüntünün shaft'larını taşıyordu. 0.95'te ~20 karede yetişiyordu. Ağırlık artık kare başına 1° dönüşte ya da 0.5 yd adımda `temporal_weight`'ten `temporal_moving`'e doğrusal iner (`fog::motion_history_weight`). HUD satır 14 kullanılan ağırlığı gösterir.

**3. Oyunun gölge haritaları (M10, araştırma başladı).** WoW her karede 4 adet 2048² D24S8 güneş kaskadı çiziyor (`frame-scan` bölüm 7). Opak shader'lar bunları FS içinde, sabit register'larla hesaplanan koordinatlarla örnekliyor (`tools/spirv-trace.py`: kaskad seçimi + PCF, D3D9 register'ları). Işık matrisleri büyük olasılıkla FS sabitlerinde duruyor.
- Frame-scan isteğine `uniforms` sözcüğü eklenirse (`tools/shadow-constants.py --request`), büyük kare bir depth image'ı karşılaştırmayla örnekleyen draw'ların uniform buffer'ları kopyalanır (submit anında host-coherent bellekten; 64 draw, pipeline başına 6, 8 MiB). Her kareye fog'un kamerası ve oyunun ışığı eklenir.
- `tools/shadow-constants.py` sabitlerde ortografik ışık bloklarını arar: güneşe paralel bir satır, ona dik iki satır. Dünya ve görüş uzayında, satır ve sütun düzeninde dener. Sahte veride dört kaskadı boyutlarıyla buluyor.
- **Oyunda bulundu (Shadowglen, `frame-scan-34266-1`):** VS sabitleri c53–c64 (DXVK set 2 binding 0), kaskad başına 3 satır (x, y, derinlik). Görüş uzayındaki konuma (x = sağ, y = yukarı, z = ileri; fog'un kendi kurgusuyla aynı) uygulanıyor. Çıkan değer ışığın kırpma uzayı: x, y −1..1, derinlik 0..1. Genişlikler 40/80/320/1280 yd, kamerayı çevreliyor. Derinlik satırı dört kaskadda ortak ve `0x00D43180` yönünün tersine paralel; yani oyunun gölgeleri bu yönü kullanıyor. Pixel shader sabitleri (set 2/b3): c20 = bu yön (görüş uzayında), c21–c27 = PCF ofsetleri (1/2048), c18 = gölge rengi. İlk tarama 384 buffer'ın hiçbirini kopyalayamamıştı; DXVK'nın sabit ring'leri fazladan kullanım bayrakları taşıyor, tarama için `allowWritableUsage` eklendi.

**Entegrasyon (`game_shadows`, varsayılan 0):**
- **Bulma** (`observe_cascades`, world draw'larında): fragment shader'ında 4+ karşılaştırmalı binding olan pipeline'larda karede en fazla 8 deneme. Karşılaştırmayla örneklenen 4 büyük kare derinlik görüntüsü binding sırasına göre alınır. VS sabitleri host-coherent ring'den okunur, `fog::cascade_rows_problem` ile sınanır ve inceden kabaya sıralanır.
- **İndirgeme** (composite'te, `fog_cascade_reduce.frag`): her harita 512²'ye, blok içindeki en yakın derinlikle yazılır. Oyunun kendi örnekleme layout'unda okunur, compute değil fragment pass kullanılır (DXVK'nın compute durumu).
- **Kullanım** (`fog_light.comp fogGameShadow`): bir sonraki karede froxel, o karenin görüş uzayına (ekran derinliğiyle aynı gölge kamerası) ve onu kapsayan en ince kaskada çevrilir, 2×2 texel ve 1.5 yd bias ile test edilir. Kaskadın dışında eski yöntem çalışır.
- **Güneş yönü:** `game_shadows` açıkken fog'un güneşi kaskadların ışığıdır (ortak derinlik satırından), yani zemindeki gölgelerle aynı yön. HUD/log: "game shadows".
- **Doğrulama:** `game_shadow_flip` haritaların v eksenini seçer (1 = D3D, v = .5 − .5y). Capture haritaları ve bu karenin matrislerini (`cascadeRowsThisFrame`) yazar. `tools/cascade-check.py` her harita×matris×v-işaretini sahne derinliğine karşı sıralar.
- **Çözüldü (2026-09-28, yel değirmeni, `frame-scan-110504-1` + `fog-capture-110504-1`):** eşleştirme `tools/spirv-eval.py` ile bulundu. Bu küçük bir SPIR-V yorumlayıcısı: DXVK'nin D3D9 shader'larını taramanın yakaladığı sabitlerle sayısal olarak çalıştırıyor. Arazi alıcı çifti `44c15e1a` / `9e7279ff` dünya noktalarıyla çalıştırıldı:
  - VS dünya konumu alıyor. Kamera projeksiyonu birebir tutuyor (kameranın 20 yd önü → ndc (0, 0)).
  - tex3 = görüş uzayı konumu. tex4..tex6 = kaskad koordinatları; 4. kaskad bunların w bileşenlerine paketli. Bunlar c53..c64 satırları × görüş konumunun tam kendisi.
  - FS, kaskad k'yi **(.5 + .5x, .5 + .5y)** noktasında, **z** referansıyla örnekliyor: 20 yd'de 9 örnekli PCF ile kaskad 0, 40 yd'de kaskad 1.
  - Hata v işaretiymiş: `game_shadow_flip` artık varsayılan olarak −1.
- Eski "imkânsız %56/%86" sonuçları doğrulamanın yanlış varsayımındanmış. WoW haritalara yalnızca gölge düşürenleri çiziyor (modeller, binalar; arazi yok). Texel'lerin %83–96'sı derinlik 1'de kalıyor, yani zemin hiçbir zaman "haritada kendisi" değil.
- `tools/cascade-check.py` artık oyunun kendi görüntüsüyle sınıyor: gölgede sayılan pikseller ne kadar karanlık? Katmanın eşleştirmesinde gölgedeki/aydınlık parlaklık oranı 51.8/75.1 = **0.69**, diğer üç eksen seçeneğinde 0.94–1.10.
- `fog_light.comp`: kaskadların kapsadığı yerde görünürlük min(kaskad, güneş haritası). Arazinin (dağların) gölgesi dünyaya sabit güneş haritasından geliyor; kameraya bağlı ekran yürüyüşü orada kullanılmıyor.
- Oyunda (`fog-capture-110504-1`): sis katkısında yel değirmeni kanatlarının gölgesinin şekli görünüyor.
- **Ama huzmeler için kötü (2026-09-28, Mulgore, A/B `fog-capture-114104-1` kapalı / `-2` açık, aynı kare):** `game_shadows` açıkken fog'un güneşi kaskadların ışığıdır (gölgelerin yönü, 54°). WoW'un gökyüzüne çizdiği disk ise 29°'deydi. Huzmeler ve ileri saçılma görünen güneşten 25° uzakta toplanıyor. En uzak kaskad da (1280 yd / 512 = texel başına 2.5 yd, en yakın derinliğe indirgenmiş) uzaktaki aralıkları kapatıyor.
  - `tools/fog-shafts.py`: huzmeler 2.81 → 0.31, huzme payı 0.40 → 0.06, toplam etki 7.1 → 4.9.
  - Kullanıcı: "uzakta shaft göremiyorum, en etkileyici kısımlardan biriydi". `game_shadows` varsayılan 0 kalıyor; SHAFTS görünümü onu açmıyor.
  - Oyunun gölgeleri yalnızca disk ile gölge yönü yakınken (Booty Bay ~46° / 53°) sise uyar.
- Disk takibi: fog güneşi yokken disk parçaları da onu başlatabilir (üst üste 3 kez). Başlamış bir takibi yalnızca bütün bir disk taşıyabilir; meşale hatası buna bağlı.
- **Log:** `[FOG-CASCADES] 4 maps … at bindings …, … yd wide; their light el … az … is the fog's sun; used in N frames` ya da neden kullanılmadığı.
- **Testler:** Unit: Shadowglen'in gerçek satırları geçiyor; bozuk satırlar yakalanıyor; ışık yönü; hangi kaskadın hangi noktayı kapsadığı. Smoke: enjekte edilmiş 4 harita (ışık tepede, haritalar 0.5 = kamera yüksekliğinde düz bir örtü): zemin gölgede 0.00 (kaskadsız 1.00), duvarın tepesi 1.00, fog güneşi "game shadows" yukarı.
- **Oyunda henüz görülmedi.**

## Işık tavanı ve otomatik pozlama (27 Eylül 2026, Shadowglen)

**Belirti:** Kanopideki açıklıktan dosdoğru güneşe bakınca ekran pembe-beyaza gömüldü. Ekran görüntüsünde parlak bölge ortalaması 0.89, %90'lık dilim 1.0.

**Sebep:** Güneşe bakarken faz fonksiyonu ~4 kat güçlü saçıyor (0.254, yandan 0.059). Güneş 12 × albedo 0.9 × 0.254 ile ışık alan sütunun eklediği ışık beyazın ~2.7 katı. `max_opacity` sisin gizlediğini sınırlıyor, eklediği ışığı değil. Sonra WoW'un glow'u.

**1. `light_limit`** (fog.ini; 0 = kapalı, SHAFTS görünümü 0.8). Eklenen ışığın en parlak kanalı tavanın yarısına kadar aynen kalır. Üstünde aynı eğimle bükülerek tavana yaklaşır, renk korunur (`fogSoftLimit` = `fog::soft_limit`). Parametre: `dist.z`.

**2. Otomatik pozlama** (göz adaptasyonu; `auto_exposure`, `exposure_target` 0.55, `exposure_min` 0.6).
- **Ölçüm:** `fog_exposure.comp`, entegrasyondan sonra önceki karenin çeyrek çözünürlüklü resmine bu karenin fog'unu composite'in uyguladığı gibi uygular (`fogShape`). Sonucu log2 parlaklığa göre 64 kutuya sayar. Hesap, oyuncunun glow'dan önce gördüğüne yakındır.
- **Anahtar:** Parlaklığı %40–98 dilimleri arasındaki texel'lerin ortalaması (`fog::exposure_key`).
- **Pozlama:** min(1, hedef / anahtar), `exposure_min`'in altına inmez (`fog::exposure_for`). Hiç aydınlatmaz; gece ve mağaralar oyunun çizdiği kadar karanlık kalır.
- **Hız:** Kararma ~0.4 s, geri dönüş ~1.5 s.
- **Nerede:** Composite dünyanın tamamını bu çarpanla ölçekler (karışımın iki terimi). Bu, WoW'un glow'undan önce (glow daha az patlar) ve UI'dan önce olur (UI hiç kararmaz). Sadece fog açıkken çalışır; gölgeler kapalıyken de ölçer (resim kopyası ve derinlik pass'i artık pozlama için de yapılıyor).
- **Kalibrasyon** (geçmiş capture ve ekran görüntüleri, parlak bölge anahtarı): oyunun fog'suz gündüz görüntüsü 0.44–0.57, fog'lu Booty Bay 0.53–0.70, Teldrassil 0.17–0.37, patlamış ekran görüntüleri 0.67–0.89 → hedef 0.55. Normal sahneler değişmez.
- **Log:** `[FOG-EXPOSURE] bright key … -> exposure goal …, applied …`. HUD satır 16: `EXP … KEY …`. what-if: `lightlimit=`, `exposure=`.

**Maliyet:** İlk sürümde pozlama ölçümü 1.63 ms tutuyordu (bench, 1660 Ti): 130 bin thread aynı birkaç kutuya global atomik yazıyordu. Workgroup başına paylaşımlı bellek histogramıyla 0.10 ms'ye indi. Zemin histogramı da aynı şekilde yazıldı.

**Testler:**
- Unit: tavanın dizde sürekliliği, tavanın altında kalması; anahtar bandı; pozlamanın sadece karartması ve tabanı.
- Smoke: `light_limit 0.1`'de 117 pikselin hepsi bükülüyor, CPU referansıyla fark 0.005. Pozlama (hedef 0.05, taban 0.5): anahtar 0.376, uygulanan 0.5, kare = düz kare × 0.5 (fark 0.002).

## Işık huzmeleri: ölçerek ayar (yol haritası aşama 5, 27 Eylül 2026)

**Araç:** `tools/fog-shafts.py DIR... --variant "ad:anahtar=değer,..."`. Capture'ı iki kez yeniden çizer: bir kez güneş görünürlüğü olduğu gibi, bir kez de 25 texel'lik düz bulanıklıkla. Raporladıkları:
- `change`: sisin toplam etkisi.
- `shafts`: ışık/gölge yapısından gelen kısım.
- `share`: ikisinin oranı.
- `p99`: parlak noktalar.

what-if'e `g2`/`lobe` (ikinci faz lobu), `density_scale`/`sun_scale` eklendi.

**Bulgular** (4 capture: Teldrassil, Shadowglen ×2, Booty Bay):
- **Faz fonksiyonu kaldıraç değil.** g 0.2 / 0.35 / 0.5 ve iki loblu faz (0.2 + 0.85 × 0.3) huzme payını neredeyse hiç değiştirmiyor. Pay sahneye bağlı: kanopi açıklığı 0.31, orman 0.15–0.16, açık liman 0.05.
- **Kaldıraç yoğunluk/güneş dengesi.** Yoğunluk ×0.6 + güneş ×1.6: ormanda huzmeler %20–25 artıyor, toplam etki %3–9 artıyor, p99 yükseliyor. ×0.5/×2 ve ×0.4/×2.5 daha az kazandırıyor, parlak noktaları daha çok artırıyor.
- **Oyunda A/B** (Shadowglen, açıklığa bakış, `fog-capture-62384-1` → `-2`): yoğunluk 0.02 → 0.012, güneş 12 → 19.2. Toplam etki 35.4 → 33.0, huzme aynı, görüntüde ışınlar daha çizgili.

**Güneş halesi (`sun_aureole`, `sun_aureole_g`):** Kullanıcı "güneşin yarısı bir engelin arkasındayken kenardan ışık taşsın" istedi. Faz fonksiyonuna dar bir ileri lob karıştırıldı: (1 − w)·HG(g) + w·HG(g₂), g₂ = 0.9 (`fogSunPhase` = `fog::sun_phase`). Engelin hemen yanında hâlâ güneş alan hava, güneşe bakan kameraya doğru parlıyor, gölgedeki hava karanlık kalıyor. Böylece ışık silüeti kendiliğinden sarıyor, ekran efekti yok.
- `62384-2` üzerinde ölçüldü: w 0.02–0.04 kenarları sarıyor. 0.1 ve üstü açıklığın tamamını beyaza gömüyor.
- `sky_fog` 0.3 ile güneş diski görünür kalıyor.
- Varsayılan 0 (değişiklik yok); SHAFTS görünümü 0.04 yazar.
- Testler: faz küre üzerinde normalize (1 ± 0.002), güneş yönünde 3.3× (0.254 → 0.848), yan tarafta biraz daha sönük. Smoke: w 0.3'te 117 pikselin 55'i değişiyor, CPU referansıyla fark ≤ 0.0175.

**Güneş parlaması (`sun_glare`, `sun_glare_radius`):** Kullanıcı, güneş yarısı önündeki direğin arkasındayken ışığın direğin üstüne de taşmasını istedi. Hava bunu yapamaz, çünkü birkaç yard ötedeki direğin önündeki hava onun gölgesinde (capture `65973-1`: fog direğin çevresine neredeyse hiçbir şey eklemiyor). Bu göz/mercek parlaması (veiling glare).
- Diskin görünen payı çizim başına bir kez ölçülüyor: composite'in vertex shader'ı (`fog_glare.vert`), çizilen diskin (taze ise görülen disk, değilse fog güneşi) ekrandaki izdüşümünde o karenin sahne derinliğinden 64 nokta okuyor. Ekran dışı noktalar sayılmıyor, sonuç fragment shader'a flat olarak geçiyor.
- Diskin yönü ~0.5° hassas (yay üzerine oturtuluyor). Bu yüzden tahmin edilen disk ve yarıçapının 0.35'i kadar kaydırılmış 8 kopyası ölçülüyor, en açık olanı alınıyor. Geniş bir engelin iyice arkasındaki disk yine 0 veriyor.
- Booty Bay (capture `68897-1`, 2026-09-27): tek diskli ilk sürüm tahmini 8 px kirişin içine düşürdü ve payı %4 ölçtü, ekranda görünen ~%18'di. Parlama 0.7'de neredeyse görünmüyordu. Toleranslı ölçüm payı %25 buluyor (önizleme: `glare-preview.png` = önce | 0.7 | 1.0 | 1.4).
- Sonucu güneş rengine doğru çekiyor: k = güç × pay^(1/3) × PSF(açı), PSF = 2.5°'lik çekirdek + 8°'lik etek, 12–20° arasında sönen. Kesimde halka oluşmuyor, testle yakalandı.
- Karışım: sonuç = mix(sahne·T + S, renk, k), iki blend terimi de (1 − k) ile ölçeklenir. Sis hacmi, huzmeler ve ışık hesabı değişmiyor.
- Önizleme (`glare-preview.png`): diskin %6'sı görünürken küp kök 0.4.
- Diskin kendisine uygulanmıyor: gökyüzü piksellerinde disk yarıçapı içinde 0, iki katına kadar açılıyor. Güneş rengine doğru karışım WoW'un soluk sarı diskini turuncuya çekiyordu (Booty Bay akşamı, `92693-1`: 255,255,165 → 255,179,88). Düzeltmeden sonra `92693-2`: 255,255,159. Varsayılan 0; SHAFTS görünümü 0.7 yazar.
- Testler: PSF merkezde 1, azalıyor, 20°'de 0; görünür pay yoksa 0. Smoke: güneş gökteyken yakın pikseller +1.70 (kanal toplamı), uzak zemin ve UI birebir aynı, güneş duvarın arkasındayken kare birebir aynı; disk yarısı duvarın üst kenarının arkasındayken 2° aşağıdaki duvar +1.23.

**Güneş ışınları (`sun_rays`, `sun_rays_length`):** Kullanıcı, Booty Bay'de güneş geminin gövdesinin arkasındayken aralardan ışık huzmeleri çıkmasını istedi. Froxel ızgarası (8-16 px karo, 48-64 dilim) ince aralıkları çözemiyor. Bu yüzden ekran uzayında, GPU Gems 3 bölüm 13'teki "Volumetric Light Scattering as a Post-Process" yöntemi kullanılıyor. Composite'ten hemen önce iki çeyrek çözünürlüklü geçiş çalışıyor:
- `fog_rays_mask.frag` (R8): her 4×4 blok için açık gökyüzü payını hesaplıyor (yüzey ve su hariç; 4 `textureGather` = 16 teksel, ince ipler ve yapraklar kaplamalarıyla sayılıyor, titremiyor). Bunu diskten uzaklaşınca düşen bir ağırlıkla çarpıyor: disk 1, çevresi 0.4/(1+(açı/8°)²), 40°'nin ötesi 0.
- `fog_rays.frag` (R16F): her teksel diske doğru yolun 0.85'i boyunca 64 adım yürüyor. Her adımın ağırlığı öncekinin 0.97'si, ağırlıklı ortalama alınıyor. Başlangıç teksel başına kaydırılıyor, bantlanma olmuyor.
- Composite ışınların yalnızca **huzme yapısını** ekliyor: ışın dokusundan, aynı dokunun disk etrafında ±4° ve ±8° döndürülmüş ortalamasını çıkarıyor ve artı kalanı güneş renginde, sisten sonra ekliyor (×4 × `sun_rays`, 0.8'e doğru bükülerek). Açılar boyunca yüksek geçiren bir süzgeç bu: pürüzsüz parıltı gidiyor, aralıklardan açılan çizgiler kalıyor.
  - Önceki denemeler (Booty Bay akşamı): (1) sisin opaklığıyla çarpınca yakındaki tabelanın ya da direğin önünde huzmeler kayboldu (`92693-1..3`, opaklık 0.02). (2) Doğrudan tüm alan eklenince, gizli güneşin etrafındaki gökyüzü sarı-beyaz bir lekeye döndü ve WoW'un glow'u onu patlattı (`92693-5`). Diskin görünen payıyla %15'e indirmek, diski içinde olan bir kanopi boşluğunu kurtardı (`62384-2`), gizli güneşi kurtarmadı. (3) "Screen" karışımı parlak zeminde huzmeleri siliyordu.
  - Yapı süzgeci üç capture'da da (kapı + flamalar `92693-5`, goblin `92693-4`, kanopi `62384-2`) patlama olmadan huzme gösterdi. Oyunda (`92693-6`) güneş açıktayken ortalama parlaklık 65.1 → 64.5.
- Diskin üstündeki gökyüzü pikselleri almıyor (yarıçap içinde 0, iki katına kadar açılıyor): diski WoW çiziyor.
- Güç: `sun_rays` × havanın güneş payı (yağmurda zayıflar). Fog güneşi kapalıyken (gece) yok, disk kameranın arkasındayken 0.
- Composite set layout'una binding 9 (maske) ve 10 (ışınlar) eklendi. Yalnız 32 dünya-derinlik seti yazıyor; cascade setleri composite'i çalıştırmıyor.
- Önizleme (prototip, capture'lar üzerinde): `40147-2` (Shadowglen kanopi boşluğu) tepenin üstüne boşluktan yelpaze gibi açılan huzmeler gösterdi, güç 1'de ortalama +1.8/255, 2'de +3.2. `73516-1`'de (düz gövde kenarı, aralık yok) ışın yapısı çıkmıyor, beklendiği gibi.
- Testler: smoke'ta güç 1.5'te, disk yakın duvarın dikey kenarının yarısı arkasındayken en parlak huzme +0.53 (kanal toplamı), açık gökyüzündeki disk +0.17. Disk, UI köşesi ve kameranın arkasındaki diskle çekilen kare birebir aynı. 64-bit validation altında 0 hata.

**Oyunun kendi sisi (`game_fog_distance`, 2026-09-28):** Kullanıcı, Durotar'da "oyunun kendi sisi ışık yakalamıyor, bloklıyor gibi" dedi. WoW 3.3.5a, D3D9'un sis durumlarını kullanmıyor; DXVK'nin `render_state` sis alanlarını oyunun FS'leri hiç okumuyor. Onun yerine:
- **Sis çarpanı (VS):** Tek bir register'dan doğrusal sis çarpanı hesaplıyor: f = clamp(x · görüş derinliği + y), register (x, y, 1, 0). Arazi ve bina shader'larında (44c15e1a, a9262451, 5a11f860) c28, yedi model shader'ında c46, cc9ffdd2'de c24. Kimi shader'da c6/c12/c14'te birebir kopyaları var ama okunmuyor. w hiç okunmuyor.
- **Sis rengi (FS):** Düz bir sis rengine doğru karıştırıyor; ışık ve gölge yok.
- **Nasıl bulundu:** `tools/spirv-eval.py` + register duyarlılık taraması (her register'ı değiştirip VS'in sis çıktısına bakmak). FS'i sis 0 ile çalıştırmak sis rengini doğrudan veriyor.
- **Ölçümler:** Durotar alacakaranlıkta (−0.00327, 2.0, 1, 0): 303 yd'ye kadar yok, 606 yd'den itibaren tam. Renk (0.41, 0.24, 0.35). Mulgore: 400 yd'ye kadar yok, 700 yd'de %74. `farclip` zaten 1277 (en yüksek); mesafe bölgenin ışık tablosundan geliyor.
- **Uygulama (`src/game_fog*`, `tests/game_fog_test.cpp`):**
  - Kayıt anında, sahne hedefine giden her çizimin set 2 / binding 0 VS sabitlerinde (host-coherent eşlenmiş bellek, `bound_uniform_span`) sis biçimindeki register'lar okunuyor. Biçim: x < 0 küçük, 1 ≤ y ≤ 100, z = 1, w = 0. Hangi register'ların sis taşıdığı VS hash'i başına hatırlanıyor (ilk 3 çizimde ve her 5000 çizimde bir tam tarama).
  - Karenin sisi oyla öğreniliyor (en sık değer). Ona %1 içinde eşit register'larda x ← x / k yapılıyor: iki uç da k kat uzağa gidiyor; k = 0 sisi kaldırıyor.
  - İtilmiş değer (aynı bellek sonraki çizimlerde) ne oy veriyor ne ikinci kez itiliyor.
  - Menü: GAME'S OWN FOG (1 → 1.5 → 2 → 3 → kapalı). Log: `[GAME-FOG]` (sis değeri, eski ve yeni mesafe, karede itilen register'lar).
  - k = 1 iken kanca hiçbir şey yapmıyor.
- **Nesne nesne yanıp sönen sis (2026-09-29, Booty Bay, kullanıcı: "periyodik objeleri seçiyor... ağacın dalları foglanıyor sonra gövdesi"):**
  - Aynı VS, sisi bazı nesnelerde bir register'da (c46), bazılarında başka bir register'da (c24) tutabiliyor.
  - Shader başına register listesi her 5000 çizimde bir yeniden taranıyor ve **siliniyordu**, yani yalnızca o an bakılan nesnenin register'ı kalıyordu. Diğer nesneler bir sonraki taramaya kadar oyunun düz sisini alıyordu; nesneler sırayla sislenip netleşiyordu.
  - **Düzeltme:** `ShaderFogRegisters::learn` birleşim tutuyor (en fazla 16 register, silinmiyor). Tarama ilk 8 çizimde, sonra 500 çizimde bir. Başka veri tutan bir register'a `push()` dokunmuyor (şekil ve değer kontrolü). Birim test: gövde c46, dallar c24; ikisi de öğreniliyor.
- **Menü düzeltmesi:** LIGHT SOURCES satırı sayıyı 25'e çıkarınca durum satırı (29) RESET TO DEFAULTS'un üstüne yazılıyordu. Görünmeyen satıra tıklamak her ayarı sıfırlıyordu. Sis satırları artık 4..29'da, durum satırı 1'de.

**Gölgelerin modernleştirilmesi (2026-09-28, kullanıcı: "hepsini yap"):** Oyunun gölge ayarları zaten en yüksekte (`extShadowQuality 5`: 4 kaskad, 2048², 9 örnekli PCF). Dört ek; hepsi fog.ini'de ve ana menüdeki yeni **SHADOWS AND OCCLUSION** sayfasında, varsayılan olarak kapalı.
- **`shadow_softness`, oyunun gölge kenarları:**
  - WoW'un FS'i her kaskad örneğini 9 derinlik karşılaştırmasıyla süzüyor. Kaydırmalar piksel sabitlerinde duruyor: arazi FS'i 9e7279ff'de c19..c27, (u, v, 0, 0), en fazla 1 texel (1/2048). c18 oyunun sis rengi. `tools/spirv-eval.py` ile her sabit oynatılarak bulundu.
  - `src/game_pcf.*` bu dizinin yerini desenle buluyor: en az 7 ardışık kaydırma, register numarası shader'a göre değişebilir. Kayıt anında diziyi s kat genişletiyor ve gölge kenarı s kat yumuşuyor.
  - Genişletilmiş bir dizi 1 texel'i aştığı için desene artık uymuyor, iki kez genişletilmiyor.
- **`contact_shadows`, `ambient_occlusion`, `terrain_shadows` (`shaders/fog_shade.frag`):** Yarım çözünürlükte, composite'ten hemen önce çalışan tek bir geçiş, bu karenin derinliğinden (RGBA16F: AO, güneş görünürlüğü, derinlik).
  - AO: 1.5 yd içinde 6 yön × 4 adımlık ufuk testi, normal derinlikten.
  - Temas gölgesi: güneşe doğru 16 adım, 0.4–3 yd.
  - Arazi gölgesi: 24 adım, 100 yd'ye kadar, kalınlık sınırlı.
  - Güneş terimleri yalnızca oyunun kendi kaskadlarının güneş gördüğü yerde uygulanıyor: bu karenin satırları + küçültülmüş haritalar, (.5 + .5x, .5 + .5y). Güneş yönü de kaskadların ışığı; gölge üstüne gölge yok.
  - Composite dört yarım texel'i derinliğe duyarlı büyütüyor ve blend'in sahne terimini (f.a) ölçekliyor. Sisin kendi ışığı etkilenmiyor. Gökyüzü, su ve gece (güneş terimleri) hariç.
  - Composite set layout: binding 11 (küçültülmüş kaskadlar), 12 (gölgelendirme). FogParams 1088 bayt (`shade`, `shadeLight`, `frameCascade[12]`).
- **Arazi gölgesi karakterlerden huzme çiziyordu (2026-09-28, kullanıcı: "terrain shadows yapıyor"):**
  - Sebep: tarama derinliği ~4 yd kalınlığında duvar sayıyor. Karakterlerin arkasından geçen ışınlar engellenmiş sayılıyor ve yere uzun koyu huzmeler çıkıyor. Bu huzmeler oyunun zaten çizdiği gölgelerin üstüne ekleniyordu.
  - Düzeltme: arazi taramasında oyunun gölge haritalarında bulunan engeller (stored − ışık derinliği ≤ 2e-4: bir caster'ın yüzeyi ya da arkası) sayılmıyor. Karakter, model ve binaların gölgesi oyunun, araziyse haritalarda yok. `gameDepthGap` / `gameCaster`, `fog_shade.frag`.
  - Contact shadows (3 yd) değişmedi.
- **İsimler ve simgeler gölge atıyordu (aynı gün, kullanıcı ekran görüntüsüyle, "Lazy Peon"):**
  - WoW'un karakter isimleri ve uyku "Z" simgeleri dünya geçişinde derinlik yazıyor. Frame-scan-32358-1: derinlik ön geçişi + derinlik yazan saydam çizim (VS 75c42f7a); aynı desen solan modellerde de var, bu yüzden çizim sınıfıyla ayrılamadı.
  - Oyunun gölge haritalarında yoklar, bu yüzden ilk düzeltme (`gameCaster`) onları yakalamadı. fog-capture-32358-1'de her harf ve Z yere uzun bir çizgi atıyordu (sis öncesi/sonrası farkı).
  - Yeni kural (`floating`, `fog_shade.frag`): üç koşul birden sağlanırsa şey "havada" sayılıyor ve arazi taramasında engel sayılmıyor, alıcı olarak da gölgelendirilmiyor:
    - Oyunun haritalarında değil.
    - Sisin zemin hafızasındaki (hücre başına en alçak yüzey, composite binding 13, storage image) yüksekliğin 1.5 yd (+%2 uzaklık) üstünde.
    - 8 veya 24 piksel içinde bir derinlik kenarı var. Bu şart, yüksek ama düz bir yamaç yüzünü korumak için.
  - Veride (capture 32358-1): isim/Z pikselleri zeminin 2.4–3.2 yd üstünde; zemin pikselleri ortanca 0.6, %90'ı 1.2 yd altında.
  - Hafızalar ve görünüm derinliği artık `terrain_shadows > 0` iken de tutuluyor (sisin kendi gölgesi kapalı kalıyor).
  - Smoke: arazi testi artık bir sırtla (45° rampa) yapılıyor: −0.306. Zeminden 3 yd yukarıda duran bir levha 0 gölge atıyor (kural kapatılınca −0.290: mutasyonla doğrulandı).
  - `terrain_shadows_from` (varsayılan 20 yd, menü: TERRAIN SHADOWS FROM 0/10/20/40/80): terrain shadows kameradan bu uzaklıktan sonra başlıyor ve sonraki max(10, yarısı) yd boyunca yumuşakça devreye giriyor. Kullanıcı: "karakterin yakınında gerekli mi, uzaklarda daha etkili". Yakında tarama da yapılmıyor. `shadeLight.w`. Smoke: 80 yd'den itibaren ayarında, 38 yd'deki sırt gölgesi 0.
  - AO eşiği −0.02 → −0.015: 64-bit yapı bloğun dibini biraz farklı yere düşürüyor (−0.020, 32 bitte −0.024). Bu değişiklikten bağımsız.
- **Smoke:** Yalnızca bu testte çizilen, yere oturan bir blok (45 yd, 6 yd yükseklik), güneş 30°'de arkasında.
  - Kapalıyken kare birebir aynı.
  - AO: bloğun dibinde −0.024, açık zeminde 0.
  - Temas gölgesi: hemen önde −0.035, 7 yd önde 0.
  - Arazi gölgesi: 7 yd önde, gölgenin içinde −0.29, yanında 0.
  - 64-bit validation: 0 hata.

**Oyunun kendi gölgeleri yumuşak ve uzaklaştıkça kaybolan (2026-09-28, kullanıcı: "daha flu ve yumuşayıp kaybolan gölgeler istiyorum"):** Ekran uzayındaki ek gölgelendirme mevcut gölgelere dokunmuyordu ("tuhaf", sise bağlı). Bu yüzden değişiklik doğrudan WoW'un piksel shader'larında, sürücü derlemeden önce.
- **Ölçüm (`tools/spirv-eval.py`):**
  - Gölge alan her FS dört kaskadı `OpImageSampleDrefExplicitLod` ile örnekliyor: en incesi 9–10, diğerleri 5'er kez. Her örnek kendi basic block'unda (DXVK'nin örnekleyici türü dalı).
  - FS'in rengi gölge görünürlüğünde doğrusal: renk(v) = renk(0) + v · (renk(1) − renk(0)). Örnek başına daha yumuşak bir değer, doğru aydınlatılmış yumuşak bir gölge demek.
- **Yama (`src/spirv_soft_shadows.*`, saf kod):** Her örnek kendi bloğunda, kontrol akışı değişmeden yeniden yazılıyor (PCSS):
  1. Engelleyici arama: aynı haritadan 2 ham derinlik (`OpImageFetch`).
  2. Alıcı–engelleyici mesafesi yarda çevriliyor (derinlik birimi 4000 yd).
  3. Penumbra bu mesafeyle büyüyor: `soft_shadows` × 0.05 yd / yd, 1–24 texel.
  4. Penumbra üzerinde 3 derinlik karşılaştırması.
  5. `shadow_fade`: gölge mesafeyle doğrusal açılıyor: 0.5 yd'de 0, `shadow_fade_distance` yd'de (varsayılan 10) `shadow_fade` kadar (1 = tamamen kaybolur).
  6. Engelleyici bulunmazsa oyunun kendi örneği.
  - Bir pikselin örnekleri desenin farklı parçalarını kullanıyor (altın açı). İnce kaskadda toplam 18–20 arama ve 27–30 süzme noktası.
- **Kanca (`src/game_soft_shadows.*`, `src/pipeline_discovery.cpp`):**
  - Üç yol da kapsanıyor: pipeline'daki inline SPIR-V (kopyalanmış stage dizisi; oyunun yapıları değişmiyor), `vkCreateShaderModule`, `vkGetShaderModuleCreateInfoIdentifierEXT`.
  - Identifier yamalı koddan hesaplanıyor. DXVK identifier ile önbellekten ararken yamalı pipeline'ı buluyor ya da inline koddan (yamalanarak) yeniden derliyor.
  - Discovery oyunun orijinal kodunu ve hash'lerini kaydetmeye devam ediyor.
  - Sürücü yamalı bir pipeline'ı reddederse orijinaliyle yeniden deneniyor ve yama o oturum için kapanıyor (`[SOFT-SHADOWS] the driver refused`).
  - Yama, oyun `soft_shadows > 0` ile açıldıysa uygulanıyor (oyunun ilk shader'ında bir kez okunuyor).
- **Canlı ayar (ilk oyun denemesinden sonra, kullanıcı: "aynı deminki gibi"):**
  - Sorun: ilk sürümde her ayar değişikliği oyunun yeniden başlatılmasını istiyordu; karşılaştırma yapılamıyordu.
  - Ölçüm (fog-capture-23634-1, Durotar): sahnedeki tek gölge karakterinki, cisme uzaklık 1–2.5 yd. Size 1 ile penumbra 0.05–0.13 yd, eski fade 4 yd'de başlıyordu: fark çok küçüktü.
  - Çözüm: yamalı kod `size`, `fade` ve `fade_distance` değerlerini DXVK'nin paylaşılan piksel verisinden okuyor (set 2 / binding 4, `D3D9SharedPS`, 360. bayttan itibaren: 7. aşamanın bump-env alanları, WoW'un shader'larında hiç kullanılmıyor). Değerler: sihirli sayı 1234.5678, fade mesafesi, size, fade.
  - Katman bu dört değeri kayıt anında her yamalı pipeline çizimi için yazıyor (`bound_uniform_span`, host-coherent). Sihirli sayı yoksa shader derleme anındaki değerleri kullanıyor. Canlı size 0 oyunun kendi örneğini veriyor (spirv-eval ile birebir aynı).
  - Menü: SOFT SHADOWS (kapalı/0.5/1/2/3/4), SHADOW FADE (0/%30/%50/%80/%100), FADE DISTANCE (3/5/10/20/40 yd). Hepsi anında uygulanıyor.
  - Log: `[SOFT-SHADOWS] live: ... written for N draws (M without DXVK's shared pixel data)`. M > 0 ise canlı yazma o çizimlerde çalışmıyor.
- **Doğrulama:**
  - `tests/spirv_soft_shadows_test.cpp`, oyunun shader'larıyla (`tests/fixtures/`, frame-scan-110504-1):
    - Arazi FS'inde 24, model FS'inde 25 örnek yeniden yazılıyor.
    - Talimat akışı sağlam, id'ler tekil.
    - Örnek başına 3 karşılaştırma ve 2 fetch.
    - SPIRV-Tools validator'ından (Vulkan 1.1) geçiyor.
    - VS'e dokunulmuyor.
  - `spirv-eval` sentetik kenarla (gerçek bir çizimin sabitleriyle, arazi FS'i, texel adımı 4):
    - Engelleyici 1 yd uzaktayken kenar oyunla aynı sertlikte (1 texel).
    - 5 yd uzaktayken ±12–16 texel.
    - 20 yd uzaktayken ±24 texel, fade 0.6 ile gölge %44 açık.
  - `soft_shadows_vulkan_smoke` (32 ve 64 bit): layout shader'lardan çıkarılıyor, kod DXVK gibi inline veriliyor, `SoftShadowPipelines` üzerinden GTX 1660 Ti'de gerçekten derleniyor (NVIDIA disk önbelleği kapalı).
    - Arazi: 136 → 202 ms. Model: 104 → 191 ms. Tek seferlik; sürücü önbelleğe alıyor.
    - Validation: 0 hata.

**İç mekânda oyunun gölgeleri (2026-09-28, The Den, kullanıcı: "kapalı mekanda oyunun güneşten gelen gölgesini kapatmak mümkün mü"):**
- frame-scan-49703-1/-2: iç mekânda oyun 4 kaskad değil, tek bir 2048² gölge haritası çiziyor (25 derinlik çizimi).
- Bu haritayı yalnızca modeller okuyor, set 1 / binding 1'den 14 karşılaştırmayla: karakterler (VS 67c5ece8 ve 2b8ec04c; FS 17cef8ff ve 4026ab41), objeler (4ea014ed; 11d206b5, 10d28aad, 77b9ed9d) ve 49d20072. Mağaranın WMO zemini okumuyor; oradaki tek karşılaştırma DXVK'nin normal dokular için ürettiği yedek yol.
- 4 kaskad aramayan yama bu shader'lara dokunmuyordu; sisin kaskad tespiti de içeride bu yüzden "bulunamadı" diyordu.
- **Yama (`spirv_soft_shadows`, iç mekân modu):** tüm derinlik karşılaştırmaları tek bir imajdan geliyorsa (en az 8) her örnek 1 − k (1 − oyununki) oluyor. k, canlı değer olarak D3D9SharedPS'in 352. baytından okunuyor (7. aşama BumpEnvMat[0].x); sihirli sayı yoksa derleme anındaki değer kullanılıyor.
- **Ayar:** `indoor_shadows` (1 = oyundaki gibi, 0.5, 0 = yok), menü INDOOR SUN SHADOWS. Katman 352..376 aralığına 6 değer yazıyor. Yama `soft_shadows > 0` ya da `indoor_shadows < 1` ile açılan oyunda uygulanıyor.
- **Test:** fixture `wow_indoor_fs_17cef8ff.spv`; 14 örnek, geçerli SPIR-V, NVIDIA'da derleniyor (105 → 106 ms). spirv-eval'de güç 0 ile yamalı örnekler 1.0 veriyor. Yakalanan karakter çiziminin sabitleriyle o yüzeyde oyunun gölgesi zaten çıktıyı değiştirmiyordu, bu yüzden görüntü üzerinde değil örnek düzeyinde doğrulandı.
- Menü sayfa 12 artık 13 satır: 4..29. satırlar; başlık 2., alt yazı 1. satırda.

**SHAFTS görünümü** artık aşama 5'in dengesini yazıyor: yoğunluk 0.012, güneş 19.2 (kullanıcı onayladı).

**Sonuç:** Kalan büyük kazançlar (kameradan bağımsızlık, uzaktan görünürlük) ekran tabanlı gölgenin sınırından geliyor. Onlar aşama 10'un, yani oyunun gölge haritalarının konusu. Aşama 5'ten kalanlar: ölçüm aracı, yeni denge (kullanıcının fog.ini'sinde), pozlama/ışık tavanı, hareket history'si ve açık gök güneşi (önceki bölümler).

## Transparanlara duyarlı fog (yol haritası aşama 3, 27 Eylül 2026)

**Sorun:** Composite, fog'u sahne derinliğiyle uyguluyor. Depth yazmayan blend'li bir draw (parçacık, parıltı, bir M2'nin alfa parçası) orada kendi derinliğine sahip değil; **arkasındaki yüzeyin fog'unu alıyordu**. Örneğin gökyüzü önündeki bir meşale, hacmin sonundaymış gibi soluklaşıyordu.

**Kare taramalarının gösterdiği** ([draw-classes.md](draw-classes.md)):
- Opak draw'lar ve depth yazan blend'liler (yapraklar, WMO camları) kendi derinliklerini bıraktıkları için bugünkü composite ile zaten doğru.
- Su, maskesi sayesinde doğru.
- Bu yüzden "dünya pass'ini tek bir noktada böl" yaklaşımı zararlı olur: bölmeden sonra gelen yapraklar ve su fog'suz kalır.

**Çözüm (`transparent_fog = 1`, varsayılan 0):**
1. Kayıt anında sahne hedefine giden, dünya aralığındaki, depth testli, depth yazmayan TRANSPARENT/ADDITIVE draw'lar (su hariç) ileri gönderilmez. Tam anlık görüntüleriyle (pipeline, setler, dinamik offset'ler, push constant'lar, vertex/index tamponları, uygulamanın ayarladığı tüm dinamik durum) komut tamponunda bekletilir (`fog_defer_candidate`, `fog_defer_draw`).
2. Fog composite aynı komut tamponunda kaydedilince, hemen ardından özgün rendering (aynı ekler, düzenler, render alanı, LOAD/STORE) yeniden açılır. Bekleyen draw'lar özgün sıralarıyla, uygulamanın kendi pipeline'larıyla çizilir (`replay_app_draw`), ardından uygulama durumu geri kurulur (`restore_after_private_pass`). Sonuç: fog'lu dünya + parçacık.
3. **Yedek boşaltma:** Composite o tamponda gelmezse draw'lar kaybolmaz. Hedeflerine dokunan bir bariyerden, transferden (kopya/blit/temizleme/resolve), yeni bir pass'ten, `ExecuteCommands`'tan ya da tamponun kapanışından önce aynı şekilde çizilirler. Bu durumda sonuç bugünkünün aynısıdır.
4. **Başka tampona taşınmaz.** Kare taramasında dünya iki komut tamponuna bölünüyordu. DXVK, bir tamponun kaynaklarını o tampon bittikten sonra geri dönüştürebilir; bu yüzden bekletilen her draw kendi tamponunda çizilir.

**Yerinde bırakılanlar:**
- Önceki draw'ın geometrisine çizilen katmanlar (EQUAL test ya da aynı indeks aralığı). Bunlar WoW'un ikinci malzeme geçişleri; yüzeyin derinliğinde duruyorlar.
- Yeniden çizilemeyecek durum taşıyanlar. `app_draw_replay_problem` şunlardan birinde reddeder: sorgu/koşullu rendering, render pass nesnesi, bilinmeyen dinamik durum, push descriptor, eski set, ölü tampon, dolaylı çizim.
- Inspector ya da izolasyon açıkken hiçbir şey bekletilmez.

**Uygulama durumu geri kurulamıyorsa** (push descriptor gibi) boşaltma tamponun kapanışına ertelenir (`flush postponed` sayacı); orada geri kurmaya gerek yoktur.

**Sınırlar:**
- Bekletilen draw'lar hacimsel fog almaz. Yakınlar için doğru, uzaktakiler biraz parlak kalır. Kendi derinliklerinde fog için shader enstrümantasyonu gerekir; sonraki adım.
- Suyun altındaki bir parçacık artık suyun üstünde görünebilir (su derinlik yazmıyor ve sıra değişti).
- Opak fazdaki parıltılar bekletilir; ama farklı bir komut tamponundalarsa yedek boşaltmayla bugünkü sonucu verirler.

**Log (900 karede bir):** `[FOG-TRANSPARENT] per frame: held back N (drawn after the fog A, by a fallback flush B, flush postponed P), layers kept in place L | not held: ...`

**Test (fog smoke, gerçek GPU, doğrulama katmanı 0 hata, Intel ve NVIDIA):**
- 300 yd'deki duvarın önünde, 50 yd'de additive bir "parçacık" var. Özellik açıkken 1099 parçacık pikseli "fog'lu dünya + parçacık" ile **birebir aynı** (0/255). Kapalıyken parçacık duvarın fog'unu alıyor (86/255 fark). Parçacık dışındaki pikseller değişmiyor.
- Uzak duvarın geometrisine EQUAL testle çizilen additive bir katman yerinde kalıyor; görüntü özellik açık ve kapalıyken aynı. Katman kuralı kaldırılınca test başarısız oluyor.
- Fog kapalıyken hiçbir şey bekletilmiyor.

## Performans (27 Eylül 2026)

**Oyunda ölçülen:** `preset = high` (240×135×96 froxel) ile sis kare başına 7.4 ms GPU tutuyordu. FPS sis kapalıyken yaklaşık 195, açıkken 105–110 idi. Dağılım: hafızalar + inject 3.9 ms, light 3.1 ms.

**Ölçüm aracı:** `DEPTH_PROBE_FOG_BENCH=1 build32/fog_vulkan_smoke`.
- 1920×1080 çözünürlükte, kullanıcının `fog.ini`'sine yakın ayarlarla (high, gölge 200 yd, duman 12 yd, temporal) 400 kare çalışır ve aşama aşama GPU sürelerini yazar.
- `DEPTH_PROBE_FOG_BENCH_PRESET` preset'i değiştirir. `DEPTH_PROBE_FOG_BENCH_INI="anahtar=değer"` tek bir özelliğin maliyetini ölçmek için satır ekler.
- NVIDIA'da çalıştırmak için `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.i686.json` gerekir; varsayılan ilk cihaz Intel UHD 630'dur.
- Bench modunda her karedeki 8 MB geri okuma yapılmaz. Yapılınca GPU %32 kullanımda boşta kalıyor ve saat 1455'ten 1095 MHz'e kayıyordu. Dizüstü 1660 Ti olduğu için sonuçlar koşudan koşuya kıyaslanamıyordu. Şimdi GPU %95 dolu ve 1880 MHz'te, yani oyundaki koşula yakın.
- Bench'teki composite süresi (~2.9 ms) gerçeği yansıtmaz. Hafif test sahnesinde composite'in bariyeri, bizim compute gönderimimizin bitmesini de beklediği için onu da ölçüyor. Oyunda aynı aşama 0.25 ms.

**Zaman damgaları:** Compute gönderimi artık 7 damga kullanıyor: başlangıç, temizleme, scatter, sun find, inject, light, integrate. `[FOG-PERF]` logu `memories (clear/scatter/sun find) inject light integrate composite total | grid` biçiminde yazar. HUD satır 13: `GPU MEM … INJ … LIT … INT … CMP …`.

**Nereye gidiyordu (high, 1880 MHz):**

| Aşama | Süre |
|---|---|
| Duman gürültüsü (inject'in neredeyse tamamı) | 2.1 ms |
| Işık aşaması | 3.4 ms |
| Güneş taraması | ~1.6 ms |
| Gök taraması (height memory'den 8 okuma) | ~0.8 ms |
| Görünür-noktaya çekme | ~0.6 ms |
| Hafızalar | 0.55 ms |

**1. Duman gürültüsü pişirildi.**
- Önceden froxel başına 64 hash hesaplanıyordu (3 warp + 5 oktav değer gürültüsü, her biri 8 köşe).
- Şimdi `fog_noise_bake.comp` ilk karede 64³ RGBA16F bir örgüyü aynı tamsayı hash'iyle bir kez doldurur. rgb warp'ın, a oktavların üç bağımsız örgüsüdür.
- `fog_inject.comp` donanımın trilinear filtresini `floor + smoothstep(fract) + 0.5` koordinatıyla okur. Bu, smoothstep ile ara değerlenmiş değer gürültüsünün aynısıdır. Froxel başına 6 doku okuması yeter.
- Oktavlar aynı örgüyü kullanır. Hizalanmasınlar diye her oktav hücre kesri kadar kaydırılır.
- Örgü 64 hücrede tekrar ettiği için gürültü orijini artık `fog::noise_period(ölçek) = 2 × ölçek × 64` yarda modülüyle indirgenir (12 yd → 1536, 24 → 3072, 48 → 6144). Eskiden hepsi 1536 idi.
- Sonuç: inject 2.13 → 0.32 ms. Desen aynı istatistikte, ama farklı bir rastgele alan.
- Test: dünyaya sabitlik 2 periyotla. Yeni süreklilik testi: orijin sarılırken 1 yd adım 3/255 değiştiriyor, başka yerde 5/255. Modül eski 1536'ya döndürülünce 24/255 çıkıp test başarısız oluyor.

**2. Işık aşamasında workgroup 8×8×1 → 4×4×4.**
- Aynı görüş ışını üzerindeki froxel'lerin güneşe taraması ekranda aynı çizgi üzerinden geçer. Derinlik komşuları aynı grupta olunca aynı derinlik texel'lerini önbellekten paylaşırlar.
- Işık aşaması 3.44 → 2.68 ms; hesap birebir aynı.
- Denenen şekiller: 2×2×16 → 2.73, 4×2×8 → 2.75, 8×4×2 → 3.15.
- Güneş adımını 16'dan 8'e indirmek hızlandırmadı (3.83); darboğaz dağınık bellek okuması ve doluluk.

**Toplam (bench, high, 1880 MHz):** compute 6.3 → 3.8 ms. Oyunda composite 0.25 ms olduğu için sis toplamının yaklaşık 4 ms olması bekleniyor. Oyunda henüz doğrulanmadı.

**Yapılmayanlar (görüntüyü değiştirir):**
- Gök taramasını 4 adıma indirmek: −0.37 ms.
- Gök görünürlüğünü yarım çözünürlükte ayrı bir hacimde hesaplamak: ~−0.6 ms.
- Scatter'ı dönen çeyrek piksel alt kümeleriyle yapmak: ~−0.3 ms.
- Görünmeyen (yüzeyin arkasındaki) froxel'leri atlamak: kamera hareketinde açığa çıkan yerlerde birkaç kare "eksik sis" riski var.

## Hava durumu: yağmurda güneş bulutların arkasında (26 Eylül 2026)

**Sorun:** Booty Bay'de yağmurda gökyüzü süt beyazına dönüyordu. Yakalama `45967-2`'de gökyüzü ortalaması sisten önce 66/255, sisle 147/255 idi. "Ne olurdu" ölçümüne göre bu artışın yaklaşık 66 birimi güneş teriminden, 15 birimi ambient'ten geliyordu. Oyun yağmurda kendi ışık renklerini değiştirmiyor; parıltı rengi hâlâ sıcak beyaz. Sis de bulutların arkasındaki güneşi tam güçle kullanıyordu.

**Hava bilgisi `Wow.exe`'nin kendisinden çıkarıldı:**
- `SMSG_WEATHER` (0x2F4) önce 0x526530'daki ortak işleyiciye gider. Oradan atlama tablosuyla 0x526A69'a geçer. Bu kol, log metni "Weather changed to %d, intensity %f" olan kısımdır.
- Bu kol `[0x00CD7544]->0x7846A0(tür, yoğunluk, …)` çağırır. O fonksiyon hedef yoğunluğu +0x00'a, eski yoğunluğu +0x04'e yazar; yoğunluk 0–1 arasına kırpılır.
- Her kare 0x784850 çalışır. Eski değerden hedefe zamanla geçerek o an geçerli yoğunluğu +0x08'e yazar.
- +0x13C / +0x140 / +0x144 alanları yağmur / kar / kum fırtınası efekt nesneleridir; boş değilse o hava açıktır. Oyunun kendi 0x783B60 fonksiyonu türü de 1 / 2 / 3 olarak buradan döndürür.
- Oyun yağışı `(yoğunluk − 0.25) × 4/3` ile çizer. 0.25'in altı açık hava sayılır.
- Canlı doğrulama (Booty Bay, yağmur): +0x08 = 0.466, yağmur nesnesi dolu, +0x38'de `RainDrop01.blp` dokusunun adı var.

**Katman tarafı:**
- `read_wow_weather` (`src/memory_sun.*`) her present'te okur; `DEPTH_PROBE_MEM_WEATHER` ile adres değiştirilebilir.
- `weather_overcast` şöyle hesaplanır: 0.25'te 0, 0.45'te (orta yağmur) 1, arası yumuşak. Yağış nesnesi yoksa sayı ne olursa olsun 0 kabul edilir.
- Güneş rengi `1 − weather_sun × overcast` ile çarpılır. `weather_sun` fog.ini'de, varsayılan 1; 0 hava durumunu yok sayar. Yeni menü satırı yok, çünkü sayfa dolu. Bunun yerine `SUN LIGHT` satırı etkiyi `… (RAIN 0.47, SUN 0%)` olarak gösterir: önce oyunun yağmur yoğunluğu, sonra sisin güneşten kalan payı.
- HUD satır 18: `… RAIN 0.47 SUN 0%`.
- Log: `[FOG-WEATHER]`. Tür değişince yazılır.
- Yakalamada `weather`, `weatherIntensity`, `sunWeatherScale` ve `sunColorClear` alanları bulunur. `fog-whatif.py` için `weather=` değişkeni eklendi.

**"Ne olurdu" sonucu (`45967-2`):**
- Güneş kısılınca gökyüzü +81 yerine +15 artıyor, dünya +48 yerine +7.
- Hafif yağmurda (güneş %50) gökyüzü +43 artıyor.

**Testler:**
- `memory_sun_test`: canlı nesnenin baytları, eşik altı, kar, yağış bitince davranış, bozuk değer, kendi belleğini okumanın güvenle başarısız olması.
- Fog smoke: orta yağmur güneşsiz sisle birebir aynı (fark 0), `weather_sun=0` farklı (161), hafif yağmur elle yarım güneşle aynı (0), yağmur bitince etkisiz (0).

**Ölçüm aracı düzeltmesi:** `fog-whatif.py`, yakalamanın `open_sun` değerini okumuyordu; 1 varsayıyordu. Yeniden çizim oyunla 98/255 ayrışıyordu. Artık 1.7/255.

## Titreme: gökyüzü geçişi UI sanılıyordu (26 Eylül 2026)

Kullanıcı Teldrassil'de sisin titrediğini söyledi. Log'da `fog dropped, UI protected` sık tekrarlanıyordu: sis o karelerde hiç uygulanmıyordu. Kural "dünyadan sonra aynı hedeflerde, dünya çizimi olmayan bir geçiş = UI → sisi bırak" idi ve gökyüzü çizimlerini (derinlik aralığı .998..1) de "dünya değil" sayıyordu. DXVK dünya ile gökyüzü arasında geçişi bölünce sis düşüyordu.
- Artık yalnız gökyüzü çizen bir geçiş dünyanın devamıdır; UI yalnız tam derinlik aralıklı çizimlerle (`other`) tanınır.
- Test: gökyüzü kendi geçişinde → sis bir kez uygulanıyor, düşmüyor, görüntü tek geçişle aynı; eski kuralla başarısız.
- Başka bir neden varsa diye her düşüşte o karenin geçiş sırası `[FOG-DROP]` satırıyla loglanır (oturum başına ilk 12).

## Güneş haritası (26 Eylül 2026)

**Oyundaki yakalamanın gösterdiği (`fog-capture-16805-2`, Teldrassil, güneşe bakarken):** 80 yardaya kadar froxel'lerin %0'ı güneş alıyordu. Yükseklik belleği her sütunda en yüksek yüzeyi tutar; Dünya Ağacı'nın tepesi kameranın 60–215 yarda üstünde (medyan 162) olduğu için, altındaki her örnek "üstünde bir şey var → gölge" sayıldı. Bu test dikey gökyüzü ışını için doğru, eğik güneş ışını için yanlış: noktanın tam üstü kapalıyken güneşe giden eğik ışın başka bir sütundaki açıklıktan çıkabilir. Kanopi aralarından süzülen ışınları tam olarak bu öldürüyordu. Kareyi sis ortalama yalnız 2/255 değiştiriyordu; güneşin yanındaki "patlama" sisten önceki karede de var (WoW'un kendi parlaması).

**Çözüm (`sunMap`):** aynı toroidal ızgara, ama güneşe dik düzlemde (eksenler u, v; s güneşe doğru). Her hücrede (yani her güneş ışını sütununda) görülen yüzeylerden güneşe en yakın olanın s mesafesi tutulur: görülenlerden doldurulan bir gölge haritası. Bir nokta, sütununda ondan güneşe daha yakın bir yüzey varsa gölgededir. Kanopi yalnız ona gerçekten çarpan ışınları keser, açıklıktan geçenler aydınlık kalır.
- Froxel başına tek bakış: 4 alt-hücre PCF ile yumuşak kenar, uzaklık sınırı yok (ekran uzayı yürüyüşü ince ayrıntı için kalır, ikisi çarpılır).
- Taban, güneş yarım dereceden fazla kaymadıkça (~10 dakika) korunur; kayınca yenisi alınır ve harita silinir, görülenlerden yeniden öğrenilir. Işınlanma ve tag'ler yükseklik belleğindeki gibi. HUD satır 19: `CLEARS MEM … SUN …`.
- Yükseklik belleği yalnız gökyüzü ışığı için kalır.
- Test (45° güneş, delikli çatı, yalnız güneş): çatılı referansa hata 0.030; eski yükseklik belleği yaklaşımı 0.076 (çatı altındaki tüm havayı karartıyordu), bellek yok 0.113. Eşik 0.045, eski yaklaşımı yakalıyor. Doğrulama katmanıyla 0 hata, NVIDIA'da da aynı sayı (0.0298).

## Sis yakalama (fog capture, 26 Eylül 2026)

Oyundaki sisi tahminle değil veriyle incelemek için. İki yoldan tetiklenir: F10 → VOLUMETRIC FOG → `CAPTURE FOG DATA`, ya da `$DEPTH_PROBE_EXPORT_DIR/fog-capture-request` dosyası (`tools/fog-capture.py --request`; katman ~30 karede bir bakar, sis açık olmalı).
- **Tek kare, ekranda hiçbir şey değişmez:** oyunun rengi sisten önce ve sonra, dünya görüş derinliği (R32F), yükseklik belleği, güneş haritası, medium/lighting/integrated hacimleri ve görünürlük hacmi (RGBA8, froxel başına r = güneş, b = gökyüzü; light pass yalnız yakalama karesinde `mode.z = 1` ile yazar).
- İlk sürüm görünürlüğü ikinci bir karede SUN VISIBILITY görünümünü ekrana basarak alıyordu; oyunda bir karelik flaş (titreme) olarak görüldü, kaldırıldı.
- Sis görünümü kaydedilmez ve her açılışta KAPALI başlar: yakalama sis kapalıyken istenirse iki karelik yakalama için FINAL'ı açar, sonra kapatır (ilk oyun denemesinde sis kapalı olduğu için yakalama sonsuza dek bekledi). ~2000 karede sis karesi gelmezse vazgeçer ve menüde nedenini yazar.
- Kopyalar oyunun command buffer'ına composite'in önüne ve arkasına kaydedilir; host bir event'i yoklar, GPU'yu beklemez. Parametreler GPU'nun gerçekten kullandığı haliyle submit anında yazılır (`final_params.json`, `visibility_params.json`: kamera, güneş/disk, gölge durumu, fog.ini kopyası). Yazıldıktan sonra ~100 MB'lık okuma tamponu hemen bırakılır.
- `tools/fog-capture.py [DIR]`: `sheet.png` (önce | sonra / sisin katkısı ×4 | görünürlük) ve özet: güneş ve ortam terimleri, merkezde/yanda/güneşe bakınca faz, kameradaki yoğunluk, güneşin ekrandaki yeri, froxel görünürlüğü uzaklığa göre, bellek doluluğu.
- Test: istek dosyası → iki kare → `color_after` testin okuduğu kareyle, `color_before` sissiz kareyle birebir aynı (UI köşesi hariç); görünürlük değerleri 0..1; görünüm geri yüklendi. 64-bit doğrulamayla 0 hata.
- Aynı turda bulunan eski test sorunu: doğrulama katmanıyla sis testi PASS'tan sonra kapanışta çöküyordu (katmanın cihaz durumu statik yıkımda, doğrulama katmanı zaten gittikten sonra yok ediliyordu; önceki doğrulama koşularında çıkış kodu kontrol edilmemişti). Test artık cihaz durumunu açıkça kapatıyor.

## Gökyüzü diski ≠ aydınlatma yönü (26 Eylül 2026)

Kullanıcı Teldrassil'de "ay gibi bir kaynak var, oradan ışık gelmiyor" dedi. Ekran görüntüsü ve bellek kamerasıyla ölçüldü: ışık bloğunun yönü (arazi aydınlatması, yükseklik 36.8°) diskin ~15–20° altına izdüşüyordu, kanopiye. Gökyüzünde çizilen disk aynı 45° azimutta ama daha yüksekte (56.3°).
- Canlı bellek taramasıyla bulundu: **statik `0x00D43180`**, x = y, ışığın gittiği yön. Kameranın hareketsiz olduğu bir ekran görüntüsünde diske izdüşüyor (x ±10 px; y'de ~2° fark, çünkü diskin üstü kanopi ve ekran kenarıyla kesik).
- Canlı bir değer: 90 s'de 0.023° kaydı (ışık bloğu 0.074°).
- `sun_position = sky` (varsayılan) sisin güneşini bu diskten alır, okunamazsa ışık bloğuna düşer. `[FOG-SUN]` log satırı iki yönü de yazar. HUD satır 17: `SUN SKY DISC EL … (LIGHT EL …)`.
- Test: `memory_sun_test` (disk okuma, bozuk disk bloğu bozmaz); `fog_vulkan_smoke` (yerleştirilen disk vektörüyle `sky` = aynı yönün elle girilmesi, fark 0; `light` farklı, 10/255; disk yok sayılınca başarısız).
- Tek oturumda doğrulandı; yeniden başlatmada tutup tutmadığı `[FOG-SUN] ... sky disc ok` satırından görülür.

## Yükseklik belleği ve gökyüzü ışığı (26 Eylül 2026)

**Sorun (Teldrassil):** ekran uzayı gölgeler yalnız ekranda görüneni bilir. Düz bakarken dev ağaçların tepesi ekranın üstündedir; oradan geçen güneş ışınları "bilinmez → aydınlık" sayılıyordu, her şey aydınlık, huzme yok. Ortam ışığı da her yerde eşitti, yani kanopi altı karanlık değildi ve huzmelerin kontrastı düşüktü.

**Yükseklik belleği (`fog_memory_clear.comp`, `fog_memory_scatter.comp`):** dünyaya sabit, kuşbakışı 1024² ızgara, 0.5 yarda hücre (kameranın çevresinde 512 yarda), R32_UINT. Her hücrede görülen en yüksek yüzeyin dünya z'si saklanır (sıralamayı koruyan uint kodlama, `imageAtomicMax`).
- Her karede önceki karenin tam çözünürlüklü derinliğindeki her piksel dünya konumuna çevrilip hücresini yükseltir. Bir kez yukarı bakılınca kanopi, çatı ve yamaç hafızada kalır.
- Izgara toroidal kayar: her texel hangi dünya hücresini tuttuğunu etiketinde saklar, pencere değişince eski hücre silinir.
- Her şey 0.05 yarda/s alçalır: geçen karakterlerin izi söner, görüşte kalan her karede yeniden yazılır.
- 200 yardadan büyük sıçramada (ışınlanma; kıtaların koordinatları çakışır) tamamen silinir. HUD satır 19: `MEM CLEARS`.
- Maliyet, HUD'daki `INJ` süresine eklenir.

**Güneş:** yürüyüş örneği ya önceki karenin ekranında bir yüzeyin arkasındaysa (eski test) ya da bellekteki yüksekliğin altındaysa gölgededir. Ekran dışındaki örnekler yalnız bellekle test edilir. Varsayılan yürüyüş 120 yarda, 16 adım (34° güneşte 50 yarda yukarıdaki kanopiye ~90 yarda gerekir).

**Gökyüzü ışığı = ikinci ışık kaynağı:** ortam ışığının `sky_light` payı yukarıdan gelir. Her froxel'den zenitten ~17° eğik (froxel ve kare başına dönen) bir ışın 100 yardaya kadar belleğe karşı test edilir. Kanopi altında sis bu payı kaybeder (kararır), açıklıklardan dikey ışık sütunları iner. Rengi ortamınki (oyunun ambient rengi), yönü güneşten bağımsız; gece de çalışır.

**Hata ayıklama:** Shift+F5 `SUN VISIBILITY` artık iki ışığı gösterir: kırmızı kanal güneş, mavi kanal gökyüzü. Beyaz = ikisi de ulaşıyor, turuncu = yalnız güneş, mavi = yalnız gökyüzü, siyah = hiçbiri.

**Test (`fog_vulkan_smoke`):** kameranın 20 yarda üstünde, ortasında delik olan bir çatı. Normal kameradan tamamen ekran dışındadır (testte doğrulanıyor). Kamera bir kez yukarı bakıp çatıyı öğreniyor, sonra normal bakışa dönüyor. CPU referansı çatıyı analitik olarak içeriyor (güneş: ışın/düzlem kesişimi, gökyüzü: dikey sütun).
- 117 froxel-merkezi pikselde GPU çatılı referansa 0.021, çatısız referansa 0.178 uzakta.
- 230 yardalık ışınlanma ve dönüşte bellek siliniyor, sonuç çatısız referansa 0.002 uzakta.
- Mutasyonlar (her biri başarısız oluyor): bellek hiç okunmazsa, gökyüzü hep görünür sayılırsa, ışınlanma silmesi kaldırılırsa.
- 300 yardalık ilk ışınlanma testi silmeyi sınamıyordu: çatı zaten pencereden çıkıyordu, etiketler siliyordu. 230 yarda pencerede bırakır.
- Doğrulama katmanıyla 0 hata.

**Sınırlar:** bellek yalnız görülen yüzeyleri bilir; hiç bakılmamış tepe gölge düşürmez. Yükseklik alanı olduğu için bir çatının altındaki nokta yandan gelen alçak güneşte de gölgede sayılır (kemerler, köprüler). Hareketli nesnelerin izi ~40 s'de söner.

## Temporal reprojection ve dünyaya bağlı gürültü (26 Eylül 2026)

**Temporal (`temporal_weight`, varsayılan 0.9; 0 = kapalı):**
- **Jitter:** her karede yoğunluk ve ışık, froxel içinde Halton(2,3,5) ile jitter'lı bir noktada hesaplanır.
- **Geçmiş:** `fog_light.comp` her froxel merkezini önceki kameranın ızgarasına geri projekte eder. Önceki kareye göre fark `camera − prevCamera` ile CPU'da double hassasiyetle alınır. Önceki ışık hacmi trilineer örneklenir ve `mix(şimdiki, geçmiş, w)` uygulanır. İki ışık hacmi ping-pong çalışır.
- **Ghosting yok:** hacim geometri içermez, bu yüzden hareketli karakterler hacimde iz bırakmaz. Temporal'ın düzelttiği şey froxel içindeki örneklemedir.
- **Geçmiş şu durumlarda atılır (HUD `TEMPORAL RESET`):**
  - ilk kare
  - önceki hacim yok (compute atlanmış)
  - ızgara ya da ayar değişmiş
  - bir karede 8 yardadan fazla yer değiştirme
  - bir karede 20°'den fazla dönüş
  - FOV değişimi
  - örnek önceki çerçevenin dışında kalıyor
- **Test (`fog_vulkan_smoke`):**
  - Sabit kamerada 40 kare sonra temporal kapalı sonuçtan en fazla 2/255 farklı.
  - 20 yardalık sıçrama geçmişi sıfırlıyor.
  - Kamera dik bir yükseklik sisinde karede 1 yarda yükselirken, doğru geri projeksiyonun ortalama hatası 4.4; hareketi hesaba katmayan geçmiş 10.5 (toplam değişim 14.5). Test bozuk sürümde başarısız oluyor; bu elle doğrulandı.
  - Kalan hata, testin 11×11'lik kaba ızgarasındaki trilineer yeniden örnekleme bulanıklığı; 1920×1080'de froxel açısı dokuz kat daha küçük.
  - Formülün kendisi `fog::froxel_uvw/froxel_offset` ile `fog_math_test`'te kesin olarak doğrulanıyor.

**Gürültü (`noise_amount` 0.3, `noise_scale` 12/24/48 yarda, `noise_wind` 0.2 yarda/s):**
- **Duman alanı (26 Eylül 2026, ikinci sürüm):** domain-warp'lı fBm, 5 oktav (s, s/2 … s/16), 1536 yarda periyotlu. Değer birim varyansa normalize edilir (z ≈ N(0,1)) ve **kapsama** ile şekillenir: sis, eşiğin üstündeki bölgelerde kümelenir, arada temiz hava kalır. Kümeler 1/ortalama kapsama ile ölçeklenir (`fog::smoke_mean`), ortalama yoğunluk korunur. 0.8'de havanın ~%60'ı temiz, kümeler ~4×. Oyunda sis biraz daha seyrek görünebilir: yamalı ortam ışığı daha çok geçirir.
- **Neden:** ilk sürüm iki oktavlı ±% dalgalanmaydı; 0.8'de bile yoğunluk ~±%23 oynuyordu, ışın boyunca ortalanıp oyunda görünmüyordu. Test sahnesinde sisli piksellere etkisi ortalama 0.85/255 idi; yenisi 4.9/255. Test bunu eşikle kontrol ediyor, eski gürültüyle başarısız oluyor. Tasarım önce numpy benzetimiyle karşılaştırıldı (aynı fizik, yer seviyesinde bakış).
- **Uzaklık:** froxel'den küçük oktavlar sönümlenir (uzakta titreşim olmaz), kalan oktavlar yeniden normalize edilir; kontrast yakında ve uzakta aynı kalır.
- **Hareket:** taban oktav rüzgarla sürüklenir; ince oktavlar ayrıca yükselir (1.5× rüzgar) ve yana kayar (0.8×). Desen tek parça kaymaz, kabarır. Rüzgar 0 ise duman durur.
- Kamera ve sürüklemeler CPU'da double hassasiyetle periyoda indirgenir; WoW'un büyük koordinatlarında bile kaymaz.
- Test: kamera 3 × 1536 yarda kaydırılınca görüntü aynı kalıyor (≤1/255), 700 yarda kaydırılınca desen değişiyor (en fazla 40/255). Yani gürültü ekrana değil dünyaya bağlı.

**Menü satırları:** TEMPORAL, NOISE, NOISE SCALE, NOISE DRIFT eklendi. HUD `TEMPORAL ON/OFF/RESET` gösterir.

## Ekleme noktası (UI ayrımı)

Gerçek veri (su yakalamalarındaki `passDraws`) şunu gösteriyor: dünya pass'i gökyüzü → opak → su → partikül sırasıyla bitiyor ve içinde UI draw'ı yok. UI sonraki bir pass'te çiziliyor.

Önceki sis denemesinden ders: DXVK dünya pass'ini kare içinde değişken noktalardan bölüp birden fazla render pass'e ayırabiliyor. Bu yüzden "ana pass'in sonu" tek bir sabit nokta değil. Kural şöyle:

1. **Dünya pass'i:** depth attachment'ı seçilen sahne depth'i olan ve dünya depth aralığında (`0..<1`, WoW'da `0..0.94`) en az bir draw içeren pass. Sonunda sis **bekler**, hemen uygulanmaz.
2. **Bölünmüş dünya:** aynı renk+depth hedeflerine yazan sonraki pass dünyanın devamıdır, bekleme sürer.
3. **Composite anı:** bu hedeflerle başka bir şey yapan ilk komuttan hemen önce kaydedilir:
   - farklı hedefli bir pass (glow, UI)
   - hedeflere dokunan copy, blit, resolve veya clear
   - Kayıt oyunun komut buffer'ına yapılır, ardından oyunun graphics durumu su sistemindeki gibi birebir geri yüklenir.
4. **UI dünya hedeflerini paylaşıyorsa sis düşürülür, asla UI'ın üstüne çizilmez.** Yani aynı hedeflerde dünya dışı draw'lar içeren bir pass görülürse durum budur. HUD `NOT APPLIED: ... UI shares them` yazar.
5. Present'e kadar hiçbir tetik gelmezse de sis düşürülür ve HUD sebebini yazar.

Sis açıldığında ve açık kaldıkça ~15 saniyede bir, iki karenin tüm pass'leri loglanır:

```bash
rg 'PASS-TIMELINE|\[FOG\]' ~/vulkan-depth-probe.log
```

Her satırda şunlar var:
- renk hedefi (swapchain/offscreen, boyut, format)
- depth (SCENE/other/none)
- dünya, gökyüzü ve diğer draw sayıları
- composite'in hangi tetikle kaydedildiği

**Bilinen sınır:** partiküller ve saydamlar dünya pass'inin içinde, depth yazmadan çiziliyor. Arkalarındaki yüzeyin sis miktarını alırlar. Doğrusu pass'i ortasından bölmek; sonraki iş.

## Geçişler ve kaynaklar

- **Izgara:** kamera NDC karesi üzerinde X×Y froxel, view derinliğinde (depth rekonstrüksiyonuyla aynı eksen) Z dilim. `d(t) = C·(e^{t·ln(1+D/C)} − 1)`: C yarda içinde neredeyse doğrusal, ötesinde üstel. D = `max_distance`, kameranın far plane'inden bağımsız.

  | Önayar | Froxel başına | Z dilim | 1920×1080'de |
  |---|---|---|---|
  | LOW | 16 px | 48 | 120×68×48 |
  | MEDIUM | 12 px | 64 | 160×90×64 |
  | HIGH | 8 px | 96 | 240×135×96 |

- **Hacimler:** üç 3D RGBA16F doku, GENERAL layout. MEDIUM ≈ 3 × 7.4 MB.
  - medium: σ_s, σ_t
  - lighting: in-scatter kaynağı, σ_t
  - integrated: in-scatter, transmittance
- **Compute** (`fog_inject.comp`, `fog_light.comp`, `fog_integrate.comp`):
  - Katmanın kendi komut buffer'ında çalışır: 3 slot, fence'li, 4 timestamp.
  - Composite'i içeren oyun submit'inden hemen önce aynı queue'ya ayrı gönderilir. Oyunun compute bağlarına hiç dokunulmaz (DXVK bağların korunduğunu varsayar).
  - Bariyerler submission sırasına dayanır: compute başında önceki composite'in okumalarını bekler, composite başında compute yazımlarını görür.
- **Medium:** `σ_t(z) = base_density · exp(−(z − base_height)·height_falloff)`, `[0, max_extinction]` aralığına kırpılır. `σ_s = albedo·σ_t`.
- **Işık:** `σ_s·(ambient + HG(cosθ, g)·sun·visibility) + emissive`. `visibility` şimdilik 1; gölge terimi yalnızca bu çarpanı değiştirecek.
- **Entegrasyon:** Frostbite'ın analitik dilim integrali, önden arkaya. `T = e^{−σ_t·ds}`, saçılan = `S·(1−T)/σ_t`. ds ışın uzunluğudur (view derinliği × ışın eğimi). Texel k, kameradan k. dilimin **sonuna** kadar birikeni tutar.
- **Composite** (`fog_composite.frag`, blend `ONE, SRC_ALPHA`): `renk·T + inscatter`, sahne rengi kopyalanmaz.
  - Piksel derinliği doğrulanmış yolla hesaplanır: viewport (yarım piksel, negatif yükseklik) ve `0..0.94` aralığı.
  - Gökyüzü ve boş alan (depth ≥ maxDepth) `max_distance`'a kadar sislenir, geometri gibi davranmaz.
  - **Su:** WoW suyu depth yazmaz, depth buffer suyun altındaki deniz tabanını tutar. Su yansıma sistemi aynı dünya pass'inin sonunda maskesini yazdıysa (her pikselde su yüzeyinin view konumu), mesafe `min(sahne, su yüzeyi)` olur. Önceki karenin maskesi kullanılmaz. Su sistemi kapalıysa (su modu OFF) su pikselleri deniz tabanına kadar sislenir; HUD `WATER SURFACE NO` yazar. Su altı sisi ayrı ve sonraki iş.
- **Kamera:** doğrulanmış bellek kamerası (kanonik poz), bu karenin ilk dünya draw'ı kaydedilirken dondurulur. Projection aynı struct'tan (`fov × 0.6`). Su GPU kamerası okunmaz.

## Doğrulama

- **`tests/fog_math_test.cpp`:**
  - dilim dağılımı
  - yükseklik ortamı
  - HG normalizasyonu (her g için integral 1)
  - analitik dilim (1 kalın = 1000 ince)
  - enerji sınırı (optik olarak kalın ortam `albedo·L`'ye yakınsar)
- **`fog_vulkan_smoke`** (gerçek GPU, Lavapipe): WoW viewport'u ve bellek kamerası test sürecinin kendi belleğinde, üretim hook'larından geçerek.
  - UI pikselleri birebir aynı.
  - GPU çıktısı, aynı fiziğin CPU'da 4000 adımlık referansıyla uyuşuyor: 117 froxel-merkezi pikselde en büyük hata 0.004, yaklaşık 1 LSB.
  - Transmittance mesafeyle azalıyor.
  - Bölünmüş dünya bir kez sisleniyor, tek pass'le aynı sonuç.
  - UI dünya hedeflerini paylaşınca sis düşürülüyor, çıktı referansla aynı.
  - Su maskesi varken sis 20 yardadaki yüzeyde duruyor (CPU referansıyla 0.0046); eski maske tekrar kullanılmıyor.
  - `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.i686.json build32/fog_vulkan_smoke`
- **İlk canlı sonuç (25 Eylül, Booty Bay):**
  - FINAL görünür, UI debug görünümlerinde bile temiz.
  - GPU 0.26–0.36 ms, CPU 0.07 ms.
  - `WORLD PASSES 2, APPLIED 1`: DXVK dünyayı iki komut buffer'ına böldü, sis yine bir kez uygulandı.
  - Su başta deniz tabanına kadar sislendi. Sebep önceden var olan bir su hatasıydı: `Inspector::queue` yalnız bir pick sırasında atanıyordu, açılışta su replay'i yüzeye tıklanana kadar `image-layout-unobserved` ile engelli kalıyordu. Artık her present'te atanıyor (`inspector_replay.cpp`, regresyon kontrolü `inspector_test`'te). Yüzey seçildikten sonra `WATER SURFACE YES` görüldü.
  - Teldrassil'de sis görünmedi: yükseklik mutlak (`base_height = 0`), Teldrassil z ≈ 1300. Boru hattı hatası değil; kamera-göreli taban ertelendi.
- **Doğrulanmayan (oyunda bakılacak):**
  - WoW'un gerçek pass yapısında composite'in UI'dan önce doğru pass'e düştüğü (`PASS-TIMELINE` logu gösterecek)
  - gerçek maliyet
  - görünüm

## Oyunda kontrol listesi

1. **Dönme:** sis dünyaya bağlı kalmalı.
2. **Yürüme:** sis ekran uzayında kaymamalı.
3. **Booty Bay:** yükseklik sisi dünya z'sini izlemeli (DENSITY ve EXTINCTION görünümlerinde yükseklik bantları).
4. **Binalar:** uzaklıkla sise gömülmeli (TRANSMITTANCE görünümü).
5. **Gökyüzü:** geometri gibi değil, `max_distance`'a kadar sislenmeli.
6. **Su:** deniz tabanına kadar sislenmemeli. Su modu SSR ya da Planar olmalı, HUD `WATER SURFACE YES`.
7. **UI:** aksiyon barı ve chat etkilenmemeli.
8. **Performans:** HUD'da pass başına GPU süreleri görünmeli.

Temporal, gürültü, oyunun güneş yönü/ortam rengi ve ekran-uzayı güneş gölgeleri eklendi (yukarıda). Partiküllerin doğru sislenmesi sıradaki iş.
