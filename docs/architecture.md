# Mimari: layer, depth seçimi, present senkronizasyonu, build ve testler

Linux/Nobara üzerinde Proton + DXVK ile çalışan **32-bit** WoW 3.3.5a için explicit Vulkan layer (`VK_LAYER_depth_probe`). Oyuna veya Wine prefix'ine DLL kurmadan sahne rengi/derinliğine erişir; present öncesi overlay çizer, render pass sonlarında Inspector ve su işlerini kaydeder. Son güncelleme: 21 Eylül 2026.

## Kod haritası

Dispatch ve instance/device yaşam döngüsü `layer.cpp`'dedir; bilinmeyen Vulkan fonksiyonları sonraki layer/driver'a aynen iletilir.

| Alan | Dosyalar |
|---|---|
| Ortak state | `src/layer_state.hpp/.cpp` (`DeviceData`, log, `WaterReplayFunctions`, `g_water_hint`) |
| Depth/swapchain/present | `src/depth_tracking.*`, `depth_selection.cpp`, `swapchain_tracking.cpp`, `present.cpp` |
| Senkronizasyon | `src/sync_tracking.*` (acquire/submit/fence takibi), `sync_capabilities.*` (gerekli feature/queue kontrolleri), `overlay_sync.cpp` (submit/present protokolü) |
| Overlay renderer | `src/overlay_renderer.*` (+ `[FRAME-PERF]`), `overlay_dispatch.cpp` (fonksiyon tablosu), `overlay_pipeline.cpp`, `overlay_uniform.*` (image başına mapped UBO), `overlay_menu.*`, `overlay_input.cpp` (XWayland input worker), `overlay_settings.*`, `heartbeat.*`, `scene_color.cpp`, `gpu_diagnostics.cpp`, `draw_probe.*`, `shader_resources.*` |
| Keşif / Inspector | `src/pipeline_discovery.*`, `discovery_*.cpp`, `inspector_*.cpp`, `surface_inspector.hpp` ([inspector.md](inspector.md)) |
| Kamera / depth → dünya | `src/camera_math.*`, `camera_analyzer.*`, `scene_debugger.*` (HUD, telemetri, gizmo, rekonstrüksiyon uniform'u), `camera_hud_input.*`, `process_memory.*` + `memory_camera*.*` (WoW CGCamera, kanonik poz), `depth_reconstruction.*` (piksel → dünya referansı; GLSL aynası `shaders/depth_world.glsl`) ([inspector.md](inspector.md)) |
| Hacimsel sis | `src/volumetric_fog.*` (kaynaklar, compute ön-gönderimi, dünya-sonrası composite, pass zaman çizelgesi), `fog_math.*` (CPU referansı), `shaders/fog_*.comp/.vert/.frag` ([fog.md](fog.md)) |
| Su | `src/water_reflections.*` (kayıt, ayarlar, kalıcılık, perf), `water_resources.cpp` (batch/slot/doku), `water_capture.cpp`, `water_spirv.cpp` ([water.md](water.md)) |
| GLSL | `shaders/*.vert/.frag` + üretilmiş `.spv` (depth, menü, inspector, water_camera/composite/sky) |

Layer shader derlemez: `./tools/build-shaders.sh` 17 shader'ı dışarıda derler, başarılı dosyaları aynı dosya sisteminde atomik rename ile yayımlar (`menu_font.glsl` de üretir). GLSL/UBO/descriptor arayüzü değişince oyun yeniden başlatılmalıdır.

## Semantik draw sınıfları

Her draw kayıt anında SKY / OPAQUE / ALPHA_TEST / WATER / TRANSPARENT / ADDITIVE / UI / UNKNOWN olarak sınıflandırılır (yalnız gözlem, `src/draw_semantics.*` kurallar, `src/draw_classifier.*` katman tarafı). Ayrıntı, kurallar, DXVK alfa testi bulgusu ve doğrulama durumu: [draw-classes.md](draw-classes.md).

## Hook maliyeti

Oyun kare başına on binlerce komut çağrısı yapar; her hook düşük maliyetli olmalıdır. `next<T>` downstream işlev göstericisini iş parçacığı yerel, 512 girişli doğrudan eşlemeli bir önbellekten alır (`cached_downstream`; cihaz oluşturma/yok etmede `invalidate_hook_cache`). Ölçüm (`hook_bench`): çağrı başına ≈ 100 ns. Dinamik state hook'ları tahsis yapmaz (düz veri kaydı). `vkGetDeviceProcAddr` sıcak yolda çağrılmaz.

**Draw anlık görüntüleri isteğe bağlıdır.** Her draw için `Draw` kopyası (record) ve her submit'te draw başına descriptor çözümleme (`submitted_frame`) WoW'un ~1700 draw'lık karesinde ≈ 6 ms CPU tutuyordu. Artık yalnız `draw_records_wanted` (Inspector pick/highlight, explorer capture/isolation/sayfası, `DEPTH_PROBE_PIPELINE_EXPLORER=1`) veya `water_wants_draw` (su seçili ve draw su shader çiftini kullanıyor / planar mod) doğruysa yapılır; layout olayları her zaman işlenir. Yeni bir tüketici `Command::events` içindeki draw'lara ihtiyaç duyuyorsa bu iki fonksiyona eklenmelidir (aksi halde draw'lar kaydedilmez). Ölçüm: `water_vulkan_smoke` ("recording…", "submitting…" satırları, gerçek sürücü).

## Scene depth seçimi

`vkCreateImage`'de eski sabit 1920×1080/D24S8 filtresi yoktur. Uygun depth adayları swapchain yaratılmadan önce hazırlanır: 2D, tek örnek/mip/layer, optimal tiling, 64–16384 boyut, DEPTH_STENCIL_ATTACHMENT usage (ek usage yalnız transfer/input/sampled), D16/X8_D24/D32/D16S8/D24S8/D32S8, create flags yalnız MUTABLE_FORMAT/EXTENDED_USAGE, `pNext` boş veya yalnız format listesi/stencil usage. Format ve usage birleşimi `vkGetPhysicalDeviceImageFormatProperties2` ile sorgulanır; başarılıysa SAMPLED eklenir (zaten varsa yamalanmaz). Yamalı create başarısızsa orijinal create-info ile yeniden denenir ve aday seçilemez işaretlenir. İlk başarılı app view'ından sonra native formatlı, identity swizzle'lı, DEPTH-only bir layer view'ı oluşturulur (app'in view'ı değişmez).

Seçim (`score_scene_depth_candidate`): aynı device, **tam swapchain extent eşleşmesi**, uygun format/usage, sampled-ready + custom view → 100, aksi -1. Eşitlikte en eski yaşayan generation, sonra handle. 2048×2048 gölge image'ı 1920×1080 swapchain için seçilmez. Resize/`oldSwapchain` retirement'ı seçimi ve renderer'ı temizler; yeni extent için yeniden seçilir (yeni aday gelene kadar overlay atlanır). `vkDestroyImage` candidate'ı, seçimleri ve bağlı renderer'ı temizler. Log: `[DEPTH-CANDIDATE-TRACK]`, `[SCENE-DEPTH-SELECTED]`, `[SCENE-DEPTH-CANDIDATES]`, `[SCENE-DEPTH-CHANGED]`, `[SCENE-DEPTH-NOT-FOUND]`. Bilinçli sınırlar: aynı extent'teki scene/shadow ayrımı kanıtlanmaz, render-scale farklı extent, MSAA, mip/array/cube/3D, linear, protected/sparse/alias/transient kaynaklar, external pNext desteklenmez; depth GENERAL layout, family 0 ve tek graphics/present queue varsayılır.

## Overlay / present senkronizasyonu

Kare başına `vkQueueWaitIdle` yoktur. Zincir:

```text
DXVK render --orijinal present semaphore'ları--> overlay submit
            --image'a ait finishedSemaphore--> vkQueuePresentKHR
```

Overlay submit orijinal wait listesinin tamamını `ALL_COMMANDS` aşamasıyla bekler ve image'a ait semaphore'u signal eder; present kopyası (`VkPresentInfoKHR copy`) yalnız bu semaphore'u bekler, uygulamanın yapıları değişmez. Overlay yalnız şunlarda çalışır (aksi halde original present aynen iletilir, nedeni `[OVERLAY-SYNC-SKIP]`): device'ta tek graphics queue (family 0/index 0) ve present aynı queue'da; güncel image'ın acquire wait'i bu queue'da gözlenmiş; tüm original present wait'leri aynı queue'da acquire'dan sonra üretilmiş; submit2 signal scope'u ALL_COMMANDS; exclusive sharing, tek layer, normal present mode, COLOR_ATTACHMENT usage; **`swapchainMaintenance1` present fence desteği** (uygulama açmışsa kullanılır, açmamışsa yerel create-info kopyalarına eklenir; uygulamanın fence'i ve pNext'i korunur, `vkResetFences/vkDestroyFence` hook'ları borrowed referansı bırakır). Image yeniden kullanımında önce o image'ın submit fence'i, sonra bekleyen present fence'i beklenir (CPU'yu bekletebilir; tamamen beklemesiz iddiası yoktur). Layout zinciri: depth GENERAL, swapchain PRESENT→COLOR_ATTACHMENT→PRESENT; sonda depth okumalarını sonraki depth yazılarından ayıran bir dependency vardır. Hata yolları: submit hatası, present hatası/OOM retry, çoklu swapchain (desteklenmez), `pResults`. Desteklenmeyen: maintenance fence yok, farklı graphics/present queue, çok graphics queue, sıfır present wait, incremental present, bilinmeyen pNext, protected/device-group kaynaklar.

Gerçek WoW'da bu protokol 4.900+ kare stabil doğrulandı (`overlayQueueWaitIdleCount=0`, present sonuçları 0 veya SUBOPTIMAL). Kabul listesi yeni sürümde: `[OVERLAY-SYNC-SUPPORT] maintenance=1 singleGraphicsQueueFamily0=1`, `[SCENE-DEPTH-SELECTED] extent=1920x1080 score=100`, ≥5.000 kare normal oyun, alt-tab/minimize ve 1920×1080→2560×1440→1280×720 resize, `SYNC-ERROR` yok.

## Overlay çizimi ve command kaydı

Her swapchain image'ının command buffer'ı ilk kullanımda bir kez kaydedilir (ONE_TIME_SUBMIT değil), sonraki karelerde fence beklemesinden sonra yalnız mapped UBO güncellenir; slider/toggle pipeline, descriptor veya command buffer'ı yeniden oluşturmaz (resize/depth generation değişimi hariç). Shader hot reload başarılıysa her image kendi fence'inden geçince yeni pipeline ile yeniden kaydedilir; başarısız reload eski pipeline'ı korur. Sıra (aynı render pass/submission): fullscreen efekt → menü → PRESENT barrier → present; efektin LOAD/STORE ve SRC_ALPHA blend'i oyunun renklerini korur.

## Ortam değişkenleri

README'de listelenmiştir (kullanıcıya dönük); yalnız kod için: `DEPTH_PROBE_WATER_FREEZE=1` testlerde sabit dalga zamanı, siyah varsayılan gökyüzü ve sıfır köpük saati verir; `DEPTH_PROBE_DRAW_PROBE_HASH` eski draw probe'unu tek shader hash'ine daraltır.

## Build

```bash
cmake -S . -B build32 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_SHARED_LINKER_FLAGS=-m32
cmake --build build32
file build32/libVkLayer_depth_probe.so      # ELF 32-bit
./tools/build-shaders.sh                           # GLSL -> SPIR-V (glslc gerekir)
```

Kök `VK_LAYER_depth_probe.json` CMake tarafından build dizinine kopyalanır; layer `VK_ADD_LAYER_PATH=<repo>/build32` ile bulunur. Depth overlay yalnız `DEPTH_PROBE_DEPTH_PRESENT=1` iken çizilir; bu image usage yamasını kapatmaz.

## Testler

```bash
cmake -S . -B build32 -DDEPTH_PROBE_BUILD_TESTS=ON && cmake --build build32 && ctest --test-dir build32 --output-on-failure
```

24 ctest (aralarında `draw_semantics`: semantik draw sınıflandırma kuralları, bkz. [draw-classes.md](draw-classes.md)): `overlay_settings`, `overlay_input_*` (sahte XCB), `layer_lifecycle`, `depth_diagnostics`, `pipeline_discovery`, `discovery_report`, `reflection_input_report`, `surface_inspector`, `water_spirv` ve GPU gerektirenler. Elle çalıştırılan gerçek GPU smoke'ları (pencere/oyun açmaz; 32-bit ICD gerekirse Lavapipe: `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.i686.json`): `vulkan_smoke` (loader, semaphore/fence döngüsü), `runtime_vulkan_smoke` (eski arşiv shader'ıyla piksel karşılaştırması, hot reload/rollback, scene copy, SSAO/SSR, menü), `discovery_vulkan_smoke`, `inspector_vulkan_smoke`, `water_vulkan_smoke` (64-bit + `DEPTH_PROBE_SMOKE_VALIDATION=1` ile Khronos validation). Bu makinede 32-bit validation layer yoktur. Testler oyun/WSI/F10 kabulünün yerine geçmez; `runtime_vulkan_smoke` `effects/rainbow-wow/shaders/depth.frag.spv`'yi karşılaştırma kaynağı olarak okur (silme).

## Gerçek oyun kabul kontrolü

En az 5.000 kare, alt-tab/resize, `overlayQueueWaitIdleCount=0`, aynı görünüm; Inspector için [inspector.md](inspector.md), su için [water.md](water.md).
