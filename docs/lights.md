# Oyunun ışık kaynakları (gizmo altyapısı)

Kullanıcı 2026-09-27'de oyundaki tüm ışık kaynaklarının (nokta ışıklar vb.) tespit edilip ekranda gizmolarla çizilmesini istedi; mevcut hiçbir şey bozulmamalı. Plan üç aşama:

1. **Araştırma**: WoW 3.3.5a modelleri vertex başına aydınlatıyor ve ışıkları shader sabitleri olarak alıyor. Hangi register'ların nokta ışık taşıdığını bulmak.
2. **Tespit**: bulunan register'ları her karede dünya çizimlerinden okuyup bir ışık listesi (konum, renk, menzil, kaynak) tutmak. Güneş/ay yönü ve ortam rengi zaten okunuyor (bkz. `docs/fog.md`).
3. **Gizmo**: mevcut ışık gizmosunun (`shaders/gizmo.vert`, yön işaretleri) yanına her nokta ışık için ekranda bir işaret ve menzil halkası çizmek; varsayılan kapalı.

## Aşama 1: araştırma

- Frame scan'e `lights` sözcüğü eklendi: `frame-scan-request` içeriği `2 lights` olunca katman, sahnenin derinlik hedefine çizilen her draw'un uniform buffer'larını kopyalıyor. Sınırlar: pipeline başına 12, en fazla 900 draw, 64 MiB. Her kareye kamera bağlamı yazılıyor. Unit test: `tests/discovery_test.cpp` ("1 lights").
- `tools/light-constants.py --request`: taramayı ister ve analiz eder. Her shader için xyz'si kameraya 250 yd içinde bir dünya konumu olan register'ları buluyor ve sıralıyor. Yanında renge ve menzile benzeyen register olanlar ve aynı shader'ın başka draw'larında sıfır olanlar (boş ışık yuvası) üste çıkıyor. Bulunan konumları 1 yd içinde tekil ışıklara grupluyor. Arazi parçalarının köşeleri (33.33 yd ızgarası, Shadowglen'de her arazi draw'unun c39'u) eleniyor.
- **Bilinenler (Shadowglen gündüz, `frame-scan-34266-1`):** M2 vertex shader'ı `75b52ce1`'de c25 = ortam rengi (0.478, 0.282, 0.518), c26 = güneş rengi, c28 = eksi güneş yönü, c30..c33 = sıfır (yakında meşale yok). Arazi shader'ı `44c15e1a`'da c40 = güneş yönü, c41 = ortam, c42 = güneş rengi, c39 = parçanın köşesi. Bu taramada nokta ışık yok (araç 0 buluyor, doğru).
- **Kamp ateşi taraması (`frame-scan-105226-2`):** ateşin yanında modeller ışıklı shader sürümlerine geçiyor (`d4fa457d`, `f07b6101`, `4ea014ed`...). Düzen:
  - c28 = eksi modellerin ışık yönü (görüş uzayı, birim, w 0.996)
  - c33..c36 = en fazla 4 nokta ışık rengi (w 1)
  - c37..c40 = aynı sıradaki konumları (görüş uzayı: x kamera sağı, y yukarı, z ileri; w 1)
  - Boş yuva tamamen sıfır. Karenin her ışıklı çiziminde aynı liste var (nesneye göre sıralanmış).
- Görüş uzayından dünyaya kameranın tabanıyla çevirince üç meşale **aynı dünya yüksekliğinde** çıktı (55.39): dönüşüm doğru. c28'in dünyadaki karşılığı azimut 45° veriyor ama yükseklik ışık bloğununkiyle aynı değil (modeller için ayrı bir yön; gizmo için gerekmiyor).
- Kamp ateşinin kendi çiziminde kendi ışığı (0.03, 0, 0.04), yani kamerada görünüyor: ışık değil, eleniyor. Menzil shader'da sabit sayı olarak yok (literaller yalnızca 0, 1, 3; ışıklar dinamik indeksle okunuyor); henüz çözülmedi.

## Aşama 2 + 3: tespit ve gizmo

- `src/game_lights.*` (saf, `tests/game_lights_test.cpp` gerçek register'larla): `parse_m2_lights` (c28..c40 → 0..4 ışık, başka düzen → -1), `view_to_world`, `Registry` (0.75 yd içinde birleşir, 120 kare görünmeyen düşer, en fazla 64).
- `src/game_lights_capture.*`: kayıt anında (`observe()` → `game_lights_observe_draw`), yalnızca LIGHT SOURCES açıkken, sahnenin derinlik hedefine giden her çizimin set 2 / binding 0'ından c28..c40 okunuyor (`read_bound_uniform_bytes`, host-coherent eşlenmiş bellek). Present'te karenin görülen ışıkları, sisin kullandığı kare kamerasıyla (`scene_debugger_frame_reconstruction`) dünyaya çevrilip kayıt defterine giriyor. `[GAME-LIGHTS]` logu her 900 karede: kaç ışık, karede okunan / ışıklı / başka düzende çizim, en yakın 4.
- Gizmo: `OverlayUniform` + `pointLights[16]`, `pointLightColors[16]`, kamera sağ/yukarı. `shaders/gizmo.vert` yön işaretlerinden sonra ışık başına 40 köşe çiziyor: kameraya bakan 0.6 yd'lik bir çember (ışığın renginde, en parlak kanal 1), içinde bir artı ve 1 yd aşağı bir çizgi. En yakın 16 ışık.
- Açma: sis menüsünde (F10) **LIGHT SOURCES** satırı ya da fog.ini `light_sources = 1`. Varsayılan kapalı; kapalıyken çizim başına tek bir atomik okuma.
- Test sahtesi: overlay bloğu 4 KiB'ı aştı (`OverlayFrameData` 4240). `tests/lifecycle_test.cpp`'nin sahte sürücüsü her tampon için sabit 4096 bayt veriyordu, 8192'ye çıkarıldı (gerçek sürücüler tamponun boyutunu döndürür).
- **Oyunda henüz görülmedi.**

## Daha iyi bulma ve nokta ışık gölgeleri (2026-09-28)

**Kullanıcı:** "ışık bulmayı kolaylaştırıp ekstra gölge cast edebilir miyiz bu point lightlardan".

**Kopyalar (log, pid 39191):**
- Kamera dururken 3–4 meşale sıfır sapmayla bulunuyordu.
- Dönerken 27, 31, 64 "ışık" çıkıyordu. Bunlar 1–2 gerçek ışığın 1.7–2.5 yd kaymış kopyalarıydı. Sunumun kamerası, çizimlerin kamerasından bir kare farklıydı.
- Lit M2 sabitlerinde doğrudan bir dünya→görüş matrisi yok: c0..c15 kemik matrisleri, c30..c32 ise kamera bazı değil.

**Düzeltme (`game_lights.*`, `game_lights_capture.cpp`):**
- Kamera bir karede 0.3°'den fazla döndüyse o karenin görmeleri yerleştirilmiyor. Log'da "turning" olarak sayılıyor.
- Birleştirme yarıçapı `merge_radius` = max(0.75, uzaklığın %2'si); görme en yakın ışığa bağlanıyor.
- Işıklar 1800 kare tutuluyor. Önceki süre 120 kareydi; ama bir ışık ancak yakınında aydınlanan bir model varken görülüyor, meşaleler de sabit.

**Gölgeler (`fog_shade.frag` `pointShadows`, fog.ini `point_shadows`, `point_shadow_range`, menü sayfa 12):**
- Kameraya en yakın 8 ışık (100 yd içinde) FogParams'a görüş uzayında aktarılıyor (`pointShadow`, `pointLights[8]`; FogParams 1232 bayt).
- Gölgelendirme geçişi her yüzeyden menzil içindeki ve yüzün baktığı her ışığa doğru derinlik üzerinde 12 adım yürüyor. Yürüyüş ışığa 0.6 yd kala duruyor; meşalenin kendi modeli sayılmıyor.
- Engel kalınlığı 1 yd + %1 (bir karakter ölçeği). Engel varsa gölge koyuluğu = güç × gündüz/gece ağırlığı × (1 − d/menzil)².
- Gündüz/gece ağırlığı: gece 1; güneş 30°'den yükseğe çıkınca 0.3.
- Havada duran isimler ve simgeler engel sayılmıyor (`floating`); karakterler, modeller ve binalar sayılıyor.
- Ekran uzayı: yalnızca ekranda görünen şeyler gölge atar.
- Oyunun ışık menzili ve zayıflaması sabitlerde olmadığı için menzil bir ayar.
- Işık okumak için LIGHT SOURCES gizmosunun açık olması gerekmiyor.

**Smoke:** Bloğun 4 yd arkasına enjekte edilmiş bir ışık (`FogState::pointLightsInjected`):
- Bloğun önündeki zemin −0.090, 14 yd yandaki zemin 0.
- Işık yokken ya da ayar kapalıyken kare birebir aynı.
- Mutasyon: gölge fonksiyonu kapatılınca test düşüyor.
- 64-bit validation 0 hata (NVIDIA, Intel).

**İlk oyun denemesi (The Den, mağara, "Crude Brazier"):** Kullanıcı: "çok açınca çıkıyor ama çok kumlu; içinde bulunduğu ızgaraya/lambaya çarpıyor, küresel bir range versek?"
- **Zayıftı:** Gündüz ağırlığı (%30) mağarada da uygulanıyordu. Artık yalnızca güneşin eriştiği yüzeylerde uygulanıyor (`gameLit`); iç mekânda ve oyunun güneş gölgesinde tam. Zayıflama (1 − d/r)² yerine 1 − smoothstep(0.25r, r, d); menzilin yarısında 0.25 yerine ~0.8.
- **Kendi mangalına çarpıyordu:** Işığın `point_shadow_ignore` yd içinde (varsayılan 1.5, menü 0.5/1/1.5/2.5) hiçbir şey engel sayılmıyor, bu kürenin içindeki yüzeyler de gölge almıyor. Smoke: blok ışığın 0.8 yd yanındayken 1.5 yd ile 0, 0.5 yd ile −0.125.
- **Kumluydu:**
  - Işık piksel başına 0.3 yd'lik bir küre içinde bir nokta (alan ışığı).
  - Adım sayısı 12 → 16.
  - Gölge ayrı kanalda (a); composite 4×4 texel'lik (tam çözünürlükte 8×8) derinliğe duyarlı bir tent ile süzüyor.
  - Smoke'ta komşu piksel farkı 0.0067 → 0.0045.

**İkinci deneme (The Den), kullanıcı: "geometri geçişlerinde gölgeler kırılıyor":**
- fog-capture-44672-1'de oyun gölge haritası yoktu (`gameCascadesFound: false`; WMO iç mekânları güneş gölgesi çizmiyor).
- Eski "havada" kuralı (zeminden 1.5 yd yukarıda + haritada yok + 8/24 px içinde derinlik kenarı) karakterlerin gövde ve başlarını, sütun ve basamak kenarlarını havada sayıyordu. Karakterin yalnızca bacakları gölge atıyor, gölge yarıda kesiliyordu. Kontrol, yakalama üzerinde çevrimdışı yapıldı.
- **Yeni kural:** `unsupported`, yani noktanın 0.2 / 0.5 / 1 / 1.5 yd altı ekranda daha uzak bir şey (ya da gök) görüyorsa hiçbir şey onu taşımıyor demektir.
  - İsim: altı boşluk → havada.
  - Karakterin başı: altında gövdesi var → havada değil.
  - Duvar ya da basamak: altında kendi yüzü var → havada değil.
  - Aynı yakalamada karakterlerde yalnızca bazı siluet pikselleri kalıyor; dışarıda karakterler zaten haritada.
- **İç mekân:** oyunun kaskadları bu oturumda görüldü ama 30 kare yok. O zaman güneş kaynaklı contact/terrain gölgeleri kapanıyor, nokta ışıklar tam ağırlıkta çalışıyor. Smoke: içeride contact 0 (dışarıda −0.035), nokta −0.573 (dışarıda −0.173).
- **Adım sayısı:** en fazla 0.4 yd'lik adımlar (8..40); uzak zeminde ince karakterler atlanmıyor.
- **Smoke'taki levha:** artık isim boyutunda (0.6 yd, zeminden 3.8 yd yukarıda). Kuralla gölgesi 0; kural kapatılınca −0.020 (mutasyon).

**Üçüncü deneme (Valley of Trials, gündüz, kamp ateşi), kullanıcı: "kırılma var, mangal gölgesi değil mi bu":**
- Evet: gölge ateşin tam tersine düşüyordu.
- Ateş alçak (~1 yd), karakterin başı daha yüksek. Alçak bir ışıktan başın gölgesi geometrik olarak sonsuza uzar; 20 yd menzille zeminde uzayıp tümseğin yamacına tırmanıyor, yamaçta da kırılmış görünüyordu.
- **`point_shadow_length`** (varsayılan 5 yd, menü: POINT SHADOW LENGTH 2/5/10/20): gölge, gölgeyi atan cisimden (bloklayan yüzey ile alıcı arasındaki mesafe) uzunluğun üçte birinden itibaren sönüyor ve uzunluğun sonunda kayboluyor. Ayaktaki kısım tam, uzaktaki kafa gölgesi yok.
- Smoke, bloğun 3.3 yd arkasında: uzunluk 2 yd → −0.004, 5 yd → −0.039, 20 yd → −0.173.
- Menü sayfa 12 artık 12 satır; alt yazı 29. satırda.

**Dördüncü deneme, kullanıcı: "üçgensel alanlar geometri ile ilgili gibi" (haklıydı):**
- fog-capture-49703-1: ateş yer seviyesinde. Zemin üçgenlerinin ateşe N·L değeri −0.55..0.8 arasında; %19'u < 0.05.
- Eski kod bu üçgenleri tamamen atlıyordu (`dot(n, l) < .05 → continue`, 2 px'lik normal). Karakterin gölgesi yalnızca ateşe bakan üçgenlerde çiziliyor, üçgen kenarlarında kesiliyordu. Karartma haritası bu sınırlarla örtüşüyordu.
- **Düzeltme:**
  - Nokta gölgeler ±8 px'lik normal (`wideNormal`) kullanıyor.
  - Yüz yönü çarpanı yumuşak: smoothstep(−0.5, −0.2, N·L). Yalnızca ışığa açıkça sırtını dönenler (direk ya da karakterin arka yüzü) gölge almıyor.
  - Aynı karede zemin çarpanı ortalama 0.81 → 0.93, 1'in altı %19 → %9, üçgen sınırları yok.
- Yalnızca shader değişti (ABI değişmedi): çalışan oyunda QUALITY ön ayarını değiştirip geri almak yeterli.
