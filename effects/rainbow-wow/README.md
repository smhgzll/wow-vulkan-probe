**rainbow-wow — WoW için heartbeat rainbow AO**

Mevcut rainbow efektinin saklanan sürümü. Ekranın merkezinden dışarı renk dalgaları yayılır; kalp atışı gibi iki vuruşun ardından duraklama olur. Efekt depth tabanlı AO bölgelerinde görünür. Varsayılan tempo 72 BPM; hız FPS'ten bağımsızdır.

Bu klasör shader kaynaklarını, derlenmiş SPIR-V'leri, uyumlu 32-bit Vulkan layer'ını ve layer kaynak arşivini birlikte içerir. Ana projede shaderlar veya renderer değişse de bu paketi tekrar kullanabilirsin. Klasör taşınabilir; scriptler konumlarını kendileri bulur. WoW dosyalarına veya Wine prefix'ine dosya kopyalamaz.

**Uygulama: Steam'de tek satır**

1. WoW'u tamamen kapat.
2. Steam → WoW → Özellikler → Launch Options alanındaki önceki layer/efekt satırını aşağıdaki satırla değiştir. Varsa oyununa özel diğer seçenekleri koru.
3. Oyunu yeniden aç. Paket hazır binary ve shader içerdiğinden ilk kullanım için derleme gerekmez.

```text
/mnt/Games/Projects/vulkan-depth-probe/effects/rainbow-wow/run.sh %command%
```

Klasörü başka yere taşırsan satırdaki yolu güncelle. Yolda boşluk varsa script yolunu çift tırnak içine al. `%command%` Steam'in oyunu Proton ile başlatan komutudur; onu değiştirme.

Script bu paketin layer ve shader yollarını seçer, depth overlay'i açar, magenta debug clear'ı kapatır ve oyunun komut/argümanlarını olduğu gibi çalıştırır. Önceki ana proje launch options'ını kullanmaya devam etmek bu arşivlenmiş paketi seçmez; yukarıdaki scripti kullan.

**Görünümü ayarlama**

Daha yavaş ve hafif animasyon:

```text
DEPTH_PROBE_HEARTBEAT_BPM=60 DEPTH_PROBE_HEARTBEAT_STRENGTH=0.7 /mnt/Games/Projects/vulkan-depth-probe/effects/rainbow-wow/run.sh %command%
```

Animasyonsuz, sabit rainbow:

```text
DEPTH_PROBE_HEARTBEAT=0 /mnt/Games/Projects/vulkan-depth-probe/effects/rainbow-wow/run.sh %command%
```

| Ayar | Varsayılan | Anlamı |
|---|---|---|
| `DEPTH_PROBE_HEARTBEAT_BPM` | `72` | Kalp atışı temposu, 20–240 BPM |
| `DEPTH_PROBE_HEARTBEAT_STRENGTH` | `1` | Animasyon yoğunluğu, 0–1; 0 eski sabit görünüm |
| `DEPTH_PROBE_HEARTBEAT` | `1` | 0 animasyonu kapatır; rainbow overlay devam eder |
| `DEPTH_PROBE_LAYER_LOG` | `/tmp/rainbow-wow.log` | Log dosyasının yolu |

Ayarlar başlangıçta okunur; değiştirdikten sonra oyunu yeniden başlat. AO radius/bias gibi genel SSAO ayarları bu sürümde sabittir.

**Kontrol ve kapatma**

```bash
tail -F /tmp/rainbow-wow.log
```

Başlangıçta `[layer-loaded]`, `[CUSTOM-DEPTH-VIEW]`, `[DEPTH-OVERLAY-READY]`, `[OVERLAY-HEARTBEAT]` ve ardından `[DEPTH-DRAW]` beklenir. `DEPTH_PROBE_LAYER_LOG` verdiysen o dosyaya bak. Log önceki çalıştırmaların sonuna eklenir.

Efekti tamamen kaldırmak için Steam Launch Options'tan `run.sh` satırını çıkar ve oyunu yeniden başlat. Uninstall veya oyun dosyası geri yükleme işlemi gerekmez. Script yalnız başlattığı sürecin environment'ını değiştirir.

**Gereken çalışma koşulları**

Bu sürüm Linux/Nobara, Proton + DXVK, 32-bit WoW 3.3.5a ve 32-bit Vulkan sürücüsü içindir. Oyunda 1920×1080 çözünürlük ve multisampling 1x (`SET gxMultisample "1"`) kullan. Depth seçimi hâlâ D24S8 / samples=1 / 1920×1080; GENERAL depth layout ve aynı family 0 render/present queue varsayımı sürer. Debug `vkQueueWaitIdle` maliyeti vardır. Başka oyunlar için genel veya bağımsız bir ReShade shader paketi değildir.

Bu heartbeat shader'ı layer'dan 16 byte fragment push constant verisi alır. Yalnız `.frag.spv` dosyasını heartbeat desteği olmayan eski bir layer'a kopyalamak yeterli değildir. `run.sh` bu nedenle beraberindeki uyumlu layer'ı seçer. Başka bir makineye taşındığında hazır Linux binary'sinin sistem kütüphaneleriyle uyumu sorun çıkarırsa aşağıdaki adımla yeniden derle.

**Kaynaklardan yeniden oluşturma**

Oyun kapalıyken:

```bash
cd /mnt/Games/Projects/vulkan-depth-probe/effects/rainbow-wow
./build.sh
```

Gereken araçlar: `cmake`, `ninja`, `glslc`, Vulkan header'ları ve `-m32` derlemeyi destekleyen C++ toolchain/32-bit geliştirme kütüphaneleri. Script `source/layer-source.tar.gz` içindeki sabitlenmiş kaynakları geçici bir klasöre açar, bu klasördeki GLSL dosyalarını ve 32-bit layer'ı derler, başarılı olunca yalnız paketin `layer32/` ve `shaders/*.spv` dosyalarını günceller. Ana projenin `build32/`, `src/` veya aktif `shaders/` dosyalarını değiştirmez.

Shader üzerinde deneme yapmak için önce bu klasörün bir kopyasını al. Kopyanın `shaders/depth.frag` dosyasını düzenle, `./build.sh` çalıştır ve kopyanın `run.sh` yolunu Steam'e yaz.

**Klasör içeriği**

| Yol | İçerik |
|---|---|
| `run.sh` | Tek satırlık Steam/terminal başlatıcısı |
| `build.sh` | Saklanan sürümü kaynaklardan yeniden oluşturur |
| `shaders/` | Heartbeat rainbow GLSL ve SPIR-V dosyaları |
| `layer32/` | Uyumlu i386 `.so` ve Vulkan manifest'i |
| `source/layer-source.tar.gz` | CMake, manifest, `layer.cpp`, `src/` ve testlerin anlık kopyası |

Saklanan sürümde shader derlemesi, 32-bit build ve lifecycle testleri geçti. Paket kendi kaynak arşivinden yeniden derlendi; gerçek 32-bit Vulkan loader/Lavapipe testi paket içindeki `.so` dosyasının yüklendiğini doğruladı. Boşluk içeren başka bir klasöre taşıma, komut argümanları, mevcut layer yollarıyla öncelik ve eksik dosya kontrolü de sınandı. Önceki sabit rainbow/refactor sürümü kullanıcı tarafından oyunda doğrulandı; heartbeat görünümü için ayrı bir oyun içi doğrulama kaydı henüz yok.
