# Su: referans uygulamalar ve görsel boşluk analizi

29 Eylül 2026. Üç açık kaynak su uygulamasının **kaynak kodu** okundu (depolar `git clone --depth 1` ile indirildi; README'lerden değil, shader'lardan). Amaç, WoW suyumuzun görsel olarak neden "eski düz ayna" gibi durduğunu, bu uygulamaların aynı sorunu nasıl çözdüğünü ve neyin bizim mimarimize (oyunun çizdiği suyun üstüne tam ekran composite, piksel başına analitik yüzey) taşınabileceğini belirlemek.

| Depo | Commit | Ne | Kapsam |
|---|---|---|---|
| [wave-harmonic/crest](https://github.com/wave-harmonic/crest) | `db0658f` (2026-06-18) | Unity okyanus sistemi, üretim kalitesi | FFT/Gerstner, LOD kademeleri, köpük simülasyonu, dinamik dalga simülasyonu (etkileşim), SSS, kostik, kırılma, su altı |
| [AlphaMistral/Mistral-Water](https://github.com/AlphaMistral/Mistral-Water) | `71b21c3` (2018-04-14) | Unity su, tessellation + FFT | FFT (Stockham), Gerstner/sinüs, Jacobian whitecap, planar/cubemap yansıma, grab-pass kırılma |
| [LesusX/Water-Shader](https://github.com/LesusX/Water-Shader) | `b2f1185` (2025-09-30) | Godot 4 tek shader | 8 Gerstner, iki normal haritası, Beer derinlik rengi, kenar köpüğü, tepe köpüğü, kostik flipbook |

Kod okunarak doğrulanan referans ayrıntıları dosya:satır olarak değil, davranış olarak yazıldı; ilgili dosyalar her başlıkta anılır.

## Özet: bizim suyun en büyük görsel boşlukları (etkiye göre sıralı)

Bizim composite'in şu anki hali (`shaders/water_composite.frag`) ile karşılaştırıldığında:

1. **Yansıma her yerde en az %30** — `mix(color, reflected, a·strength·(.30 + .70·fresnel))`. Aşağı bakınca bile suyun %27'si (strength .9) aynadır; üstüne yansımanın doygunluğu ×1.25 ve parlak kısımları ×1.9'a kadar artırılıyor. Üç referansın hepsinde yansıma yalnız Fresnel ile gelir (Crest: kırılma indisinden R0 = 0.02). **"Eski düz ayna" görüntüsünün ana nedeni bu.**
2. **Gövde rengi / soğurma zayıf** — tek renk (.03,.20,.24), en fazla %30, dikey derinlikle. Crest ve LesusX RGB başına Beer–Lambert (`1 − exp(−σ·mesafe)`) ile sahneyi su rengine karıştırır; Crest ayrıca bakış açısına göre değişen gövde rengi + sığ su rengi kullanır.
3. **Işık yanıtı yok** — güneş spekülerı yok (yalnız gökyüzü belleğinin parlak lekesine çarpan "parıltı"), SSS (güneşe karşı dalga tepelerinin içinden geçen ışık) yok. Crest'in "modern" görüntüsünün büyük kısmı bu ikisi.
4. **Pürüzlülük kavramı yok** — yansıma bulanıklığı yalnız uzaklığa bağlı; güneş vurgusu genişliği, SSR güveni pürüzlülükle değişmiyor.
5. **Köpük** — kalıcılığı yok (dalga geçince iz kalmıyor), alt-yüzey kabarcık katmanı ve ışıklandırması yok; whitecap yükseklik eşiğine bakıyor (Crest/Mistral: Jacobian).
6. **Kırılma** — yalnız uzun dalga eğimiyle başka su piksellerini örnekliyor, derinlikle ölçeklenmiyor, öndeki nesneye düşen örneği derinlikle reddetmiyor.
7. **Kostik** — güneş yönüne projekte edilmiyor, odak derinliği yok, sahneyi ortalamayı koruyarak değil hep aydınlatarak değiştiriyor.
8. **Etkileşim** — dalgacık/iz yok.

Güçlü yanlarımız (referanslarda olmayan): oyunun karışımını geri alıp deniz tabanını kurtarma (`clarity`), dünyaya sabit gökyüzü belleği, piksel başına Newton kesişimli analitik Gerstner yüzeyi (tessellation/yoğun mesh gerektirmez), piksel izine göre dalga LOD'u.

---

## Crest

Kaynaklar: `Ocean.shader` (fragment akışı), `OceanNormalMapping.hlsl`, `OceanReflection.hlsl`, `OceanEmission.hlsl`, `OceanFoam.hlsl`, `Resources/UpdateFoam.compute`, `Resources/UpdateDynWaves.compute`, `OceanInputs/Resources/DynWavesSphereWaterInteraction.shader`, `OceanHelpersNew.hlsl`.

**Yüzey yer değiştirmesi / dalgalar.**
1. *Sorun:* suyun büyük ölçekli şekli. 2. *Crest:* FFT veya Gerstner dalgaları, kameranın etrafındaki dünya-uzayı doku kademelerine (cascade) yazılır; mesh LOD halkaları bu dokulardan vertex'te yer değiştirir; normal, komşu texel'lerin çapraz çarpımından. 3. *Bizde:* var (piksel başına analitik Gerstner, aşama 1–3). 4. *Zayıf mı:* ölçek olarak evet — 4 dalga ve toplam 0.24 yd genlik sakin liman için seçildi; Crest'in spektrumu çok daha fazla ölçek taşır. 5. *Taşınacak:* dalga sayısını artırmak yerine orta/mikro ölçekleri normal katmanına vermek (aşağıda). 6. *Taşınmayacak:* cascade doku altyapısı ve mesh LOD halkaları (oyunun mesh'ini değiştirmiyoruz). 7. *Etki:* orta. 8. *GPU:* kademeler bize gereksiz; dalga sayısı piksel başı maliyet (dalga başına ~0.025 ms/1080p ölçüldü).

**Çok ölçekli normal.**
1. Yüzeyin zenginliği, yansımanın kırılması. 2. Tek normal dokusu **iki zıt yönde** (0.94,0.34) ve (−0.85,−0.53) kaydırılarak iki kez örneklenir; ölçek geometrinin LOD ölçeğine bağlıdır ve bir üst ölçekle `lodAlpha` ile yumuşakça karıştırılır (desen ekrana göre büyüklüğünü korur); akış varsa iki fazlı akış-harita karışımı. Normal = dalga normali + detay normali (xz toplamı), sonra genel güç çarpanı. 3. Var ama zayıf: 3 kısa sinüs + 2 dokusal eğim (17 ve 6.3 yd), mikro ölçek yalnız parıltıda. 4. Evet: kaydırma yönleri benzer, mikro ölçek yansımayı kırmıyor, ölçekler ekrana göre değil sabit. 5. İki zıt yönlü örnekleme, ölçek başına ayrı yön/hız, piksel izine göre ölçek karışımı (bizde LOD altyapısı var). 6. Flow-map (WoW akış vermiyor). 7. **Yüksek** — yansımanın "kum/ayna" yerine canlı kırılması doğrudan buna bağlı. 8. Düşük (doku örneği başına ~µs).

**Fresnel.**
1. Dik bakışta su gövdesi, yatık bakışta yansıma. 2. `R = R0 + (1−R0)(1−cosθ)^güç`, R0 = ((1−1.33)/(1+1.33))² ≈ 0.02; `renk = lerp(renk, gökyüzü, R·specular·ağırlık)`. 3. Schlick var ama %30 taban ve güç çarpanı ile bozulmuş. 4. **Evet, en büyük fark.** 5. Doğrudan (R0 kırılma indisinden: ÖLÇÜLMÜŞ fizik sabiti). 6. — 7. **Çok yüksek.** 8. Sıfır.

**Pürüzlülük / spekülerlik.**
1. Mükemmel olmayan yansıma, güneş vurgusunun genişliği. 2. Ortam yansıması pürüzlülüğe göre mip'li (Unity glossy environment); güneş: `yansıyan_gökyüzü += pow(max(0, R·L), falloff)·boost·ışıkRengi·gölge`, falloff yakında ve uzakta farklı (uzakta daha geniş vurgu). 3. Yok (parıltı dışında). 4. Evet. 5. Pürüzlülükten gökyüzü belleği LOD'u / SSR bulanıklığı; uzaklıkla genişleyen güneş lobu; gölge = oyunun kaskadları (sis tarafında var, suya bağlanabilir). 6. Unity probe altyapısı. 7. Yüksek. 8. Düşük.

**Yansıma karışımı.**
1. Ekran-uzayı yansımanın sınırlarında dikişler. 2. Gökyüzü/probe temel; planar yansıma kendi alfasıyla üstüne `lerp`; SSR yok (URP/HDRP'ye bırakılmış). 3. SSR → gökyüzü belleği (kenarda güven), planar → gökyüzü; ikisi birlikte değil. 4. Kısmen: SSR ile planar birbirinin yedeği değil. 5. Güven ağırlıklı katmanlama, pürüzlülükle bulanıklaşma. 6. — 7. Orta. 8. SSR+planar birlikte her pikselde çalışırsa maliyet artar; planar zaten ayrı pass.

**Derinlik rengi / soğurma / saçılma.**
1. Sığ/derin ayrımı. 2. `alpha = 1 − exp(−σ_rgb · (sahneZ − suZ))` (kırılmış örneğin mesafesiyle), `renk = lerp(sahne, saçılmaRengi, alpha)`; saçılma rengi = `lerp(yatıkRenk, dikRenk, |görüş.y|)`, sığlıkta `SubSurfaceShallowCol` karışır, ortam ışığıyla çarpılır. 3. Tek renk, %30 üst sınır, dikey derinlik. 4. **Evet.** 5. RGB Beer–Lambert (görüş ışını boyunca su içindeki mesafe), bakış açısına bağlı gövde rengi, sığ renk, bölge ortam ışığı (sis tarafında oyunun ambient'i var). 6. — 7. **Yüksek.** 8. Düşük.

**SSS (alt-yüzey saçılması).**
1. Güneşe karşı bakınca dalga tepelerinin içinden parlayan turkuaz. 2. `(taban + güneş·pow(max(0, L·−V), falloff))·SSSrengi·ışık·gölge·(1−v²)·sss`, sss dalga verisinden (tepeler). 3. Yok. 4. Evet. 5. Gerstner yüksekliği/Jacobian'dan tepe ağırlığı + güneş yönü. 6. — 7. **Yüksek** (gün batımı ve kıyıda). 8. Düşük.

**Kırılma.**
1. Suyun altındakinin dalgayla kırılması. 2. `ofset = güç·n.xz·min(1, 0.5·(sahneZ − pikselZ))/sahneZ` (kalınlık ve uzaklıkla ölçekli); kırılmış noktanın derinliği suyun önündeyse ofset iptal. 3. Yalnız uzun dalga eğimi, yalnız başka su pikselleri, sabit ölçek. 4. Evet. 5. Kalınlık/uzaklık ölçeği ve derinlikle reddetme. 6. Grab-pass (biz sahne kopyası + taban geri kazanımı kullanıyoruz). 7. Orta. 8. Düşük.

**Kıyı / kesişim köpüğü.**
1. Suyun kıyıya, direğe, kayaya değdiği yer. 2. Köpük bir **simülasyon dokusu**: her karede önceki köpük akışla taşınır, `köpük *= 1 − sönme·dt`, kıyıda `+= güç·dt·saturate(1 − su_derinliği/maxDerinlik)` (deniz tabanı derinlik önbelleğinden). Çizimde: köpük miktarı dokuyu `smoothstep(1−köpük, 1−köpük+yumuşaklık, doku)` ile eşikler ("siyah nokta solması"), çok sığda `saturate((sahneZ−pikselZ)/minDerinlik)` ile yumuşatılır. 3. Kapsama eşiği tekniği **bizde de var**; kıyı zarfı, swash cepheleri, yama alanı var; kalıcılık yok. 4. Kısmen: köpük anlık, iz bırakmıyor, ışıklandırılmıyor. 5. Dünyaya sabit toroidal köpük hafızası (sisin yükseklik hafızası altyapısı hazır), köpük normali ile basit ışık, kabarcık katmanı. 6. Deniz tabanı önbellek sistemi (biz derinliği sahne derinliğinden alıyoruz). 7. Yüksek. 8. Orta: bir compute pass + doku.

**Whitecap / tepe köpüğü.**
1. Dik dalgaların tepesinde köpük. 2. Yer değiştirme Jacobian'ının determinantı: `köpük += 5·dt·güç·saturate(kapsama − det + tabanKöpük·0.7)`; köpük dokusunda kalır ve söner (tepe geçince iz). 3. Gerstner yüksekliği eşiği, anlık (zamanda geri 3 örnek). 4. Evet. 5. Jacobian (analitik olarak zaten hesaplıyoruz) + kalıcılık için köpük hafızası. 6. — 7. Orta (sakin suda az; fırtınalı ayarda yüksek). 8. Düşük (Jacobian hazır).

**Köpük görünümü (kabarcık + beyaz).**
1. Köpüğün derinliği. 2. Beyaz köpük + altında parallax ofsetli, mip 3'te (bulanık) örneklenmiş kabarcık katmanı (ortam ışığıyla), isteğe bağlı köpük normaliyle NdotL. 3. Tek katman beyaz. 4. Evet. 5. Kabarcık katmanı ve köpük normali. 6. — 7. Orta. 8. Düşük.

**Kostik.**
1. Sığ tabanda odaklanan ışık desenleri. 2. Sahne dünya konumu, `ışıkProjeksiyonu = L.xz·derinlik/(4·L.y)` ile güneşe projekte; iki kaydırılan örnek + bozulma dokusu; mip = `log2(sahneZ) + |derinlik − odak|/alanDerinliği` (odak derinliğinde keskin); gölgeyle çarpılır; `sahne *= 1 + güç·(örnek − doku_ortalaması)` (ortalama parlaklık korunur). 3. İki ağın çarpımı, güneşe projeksiyon yok, hep aydınlatır. 4. Evet. 5. Güneşe projeksiyon, odak/mip, ortalamayı koruyan modülasyon, oyunun gölge kaskadları. 6. — 7. Orta. 8. Düşük.

**Etkileşim dalgacıkları ve iz.**
1. Oyuncu/nesne suyu hareket ettirir. 2. Dünyaya sabit 2B dalga denklemi simülasyonu (yükseklik + hız, CFL sınırlı hız, sönüm, sığda zayıflama); girdi: hareket eden küre, `kuvvet = −v.y·düşüş(sdf) + sign(sdf)·(sdfNormal·v.xz)·düşüş`. **İz ayrıca modellenmez**: hareketli küre dalga denkleminde doğal olarak Kelvin benzeri iz bırakır. 3. Yok. 4. Evet. 5. Küçük bir toroidal dalga simülasyonu (compute), oyuncu konumu doğrulandıktan sonra girdi. Başlangıç için analitik yayıcılar da olur ama iz için simülasyon daha doğru ve piksel başına döngü gerektirmez. 6. Unity fizik/örnekleme altyapısı. 7. Yüksek (oyuncu etrafında). 8. Orta: 256² compute ~0.05–0.1 ms tahmini (ÇIKARIM).

**LOD / performans.**
Kademeli dünya dokuları + mesh halkaları; normal ölçeği LOD ile büyür. Bizde piksel izine göre dalga ve mip LOD'u var; eksik olan normal detayının ölçek karışımı.

## Mistral-Water

Kaynaklar: `Shaders/MistralWaterBasic.shader`, `MistralWaterCommon.cginc`, `FFT/WhiteCap.shader`, `FFT/OceanNormal.shader`, `FFT/*.shader` (spektrum, Stockham FFT).

- **Yer değiştirme:** FFT (Phillips spektrumu, parça shader'larında Stockham FFT) ya da Gerstner/sinüs; seyrek mesh'i **kenar uzunluğuna göre tessellation** ile yoğunlaştırır. *Bizde:* analitik Gerstner, piksel başına. *Taşınacak:* yok (tessellation oyun mesh'ine uygulanamaz; FFT şimdilik gereksiz). *Etki/GPU:* FFT yüksek maliyet, sakin WoW suyu için gereksiz.
- **Normal:** tek bump dokusunun iki kaydırılan örneği harmanlanır; FFT modunda normal komşu yer değiştirmelerin çapraz çarpımından. *Bizde benzer, daha fazla ölçek var.*
- **Kırılma:** grab-pass, `ofset = n.xz·texel·güç·derinlik`; kırılmış örneğin derinliği suyun önündeyse temiz örneğe döner (`step(ekranZ, suDerinliği)`). *Taşınacak:* bu **sızıntı reddi** (bizde yok). *Etki:* orta, *GPU:* düşük.
- **Derinlik rengi:** `alpha = saturate(DerinlikMiktarı / |sahneZ − suZ|)`, sığ/derin renk karışımı × kırılma. Hiperbolik, Beer değil. *Taşınmayacak:* Crest/LesusX'in üstel modeli daha doğru.
- **Yansıma:** gerçek zamanlı planar doku (ekran UV'si normalle bozulur) veya cubemap; `fresnel = pow(1 − N·V, FresnelAçısı)`, **toplamsal** (`renk + yansıma`). *Bizde:* planar + SSR + gökyüzü belleği daha güçlü; Mistral'in toplamsal karışımı enerjiyi korumaz, taşınmayacak.
- **Speküler:** Phong `pow(R·V, düzgünlük·128)·ışıkRengi`. *Taşınacak:* güneş vurgusu kavramı (Crest'in lobu daha iyi).
- **Kıyı köpüğü:** `1 − saturate(Boyut·(sahneZ − suZ))` × normal-bozulmalı köpük dokusu. *Bizde daha gelişmiş.*
- **Kenar yumuşatma:** `alpha = saturate(KenarSolma·(sahneZ − suZ))` ile kıyıda suyun kendisi saydamlaşır, temiz arka plana karışır. *Taşınacak:* kıyı çizgisinde sert kenarı yumuşatmak (bizde `clarity` ile kısmen var).
- **Whitecap:** `J = (1+∂Dx/∂x)(1+∂Dz/∂z) − ∂Dx/∂z·∂Dz/∂x`, `türbülans = max(0, 1 − J + |gürültü|)`, `smoothstep(0,1,türbülans)`. **Not:** ana shader'da bu değer son renge `0 *` ile ekleniyor — yani depoda fiilen kapalı; kavram doğru, uygulama bitmemiş. *Taşınacak:* Jacobian + gürültü kavramı (bizde analitik Jacobian zaten var).

## LesusX Water-Shader

Kaynak: `water_shader.gdshader` (tek dosya), README.

- **Gerstner:** 8 dalga, parametre (yön, dikiklik, dalga boyu), genlik = dikiklik/k (GPU Gems normalizasyonu), faz hızı `sqrt(9.81/k)`; dikiklik `1 + 0.5·sin(t + |konum|·0.1)` ile **orijine göre radyal** değiştirilir (dünyada sabit olmayan, orijin merkezli bir desen); üstüne fbm yükseklik gürültüsü. Tessellation yok, yoğun alt bölümlü düzlem ister. *Taşınmayacak:* radyal modülasyon ve QkA-oran dikiklik tanımı (yatay kaymayı abartır; aşama 1–3'te bu yüzden reddettik).
- **Normal:** iki normal haritası (farklı ölçek/hız) `mix` ile, UV'ler bir UV-ofset dokusu + katmanlı değer gürültüsüyle **tekrar kırılır** (detiling), Gerstner TBN'iyle uygulanır. *Taşınacak:* **detiling** — kaydırılan dokunun tekrarını dünya gürültüsüyle bozmak. *Etki:* orta, *GPU:* düşük.
- **Derinlik rengi:** Beer `1 − exp(−(derinlik)·beers)`, `pow(…, 2.5)`; derin suda sahne dokusu **mip'li örneklenir** (derinlik arttıkça bulanıklaşan taban). *Taşınacak:* derinlikle bulanıklaşan taban (bizde yumuşak örnekleme yardımcısı var). *Etki:* orta.
- **Kostik:** sahne derinliğinden dünya konumu (ters MVP), 16 katmanlı doku dizisi flipbook (26 fps), normalle bozulma, `(1 − derinlikKarışımı)` ile derinlikte söner. *Taşınacak:* derinlikle sönme (bizde var); flipbook yerine bizim iki-ağ yöntemi yeterli.
- **Fresnel:** `pow(1 − N·V, 5)` ile taban rengi → fresnel rengi (albedo karışımı); asıl yansımayı Godot PBR (metallic .6, roughness .045) yapar. *Taşınmayacak:* motorun PBR'ına dayalı.
- **Kenar köpüğü:** doğrusallaştırılmış sahne derinliği − yüzey derinliği farkı, `smoothstep(başla, bitir, fark)` ile kaydırılan köpük dokusu. *Bizde daha gelişmiş.*
- **Tepe köpüğü:** yükseklik eşiği × fbm × köpük dokusu (bizim şu anki yöntemle aynı sınıf).
- **Etkileşim, LOD, zamansal kararlılık:** yok.

## Taşınmayacaklar (motor/altyapı bağımlı)

Crest'in kademeli LOD doku ve mesh sistemi, deniz tabanı önbelleği, Unity probe/SH ışıklandırması, su altı hacmi; Mistral'in tessellation'ı ve toplamsal Fresnel'i; LesusX'in radyal dikiklik modülasyonu ve motor PBR'ı. FFT: sakin WoW suyu için maliyetine değmez (şartname de önce Gerstner diyor).

## Uygulama planı (görsel etkiye göre)

Durum (29 Eylül 2026): A/B ve 5–10 MODERN stilde yapıldı (docs/water.md "MODERN stil"); 11–14 sırada.

| Aşama | Ne | Kaynak kavram | Beklenen etki | GPU riski |
|---|---|---|---|---|
| A/B | CLASSIC ↔ MODERN geçişi (mevcut görünüm korunur) — **yapıldı** | — | karşılaştırma | yok |
| 5 | Çok ölçekli normal: büyük (Gerstner) + orta (iki zıt yönlü doku, detiling) + mikro (pürüzlülüğü besler), piksel izine göre ölçek karışımı | Crest, LesusX | yüksek | düşük |
| 6 | Fresnel (R0 = 0.02, taban yok, doygunluk artırması yok) + pürüzlülük (mikro normal varyansı + ayar) | Crest | **çok yüksek** | yok |
| 7 | Yansıma karışımı: SSR güveni → gökyüzü, pürüzlülükle bulanıklık | Crest | orta | düşük |
| 8 | Güneş vurgusu + parıltı: oyunun güneşi (`GameSun`), mikro normallerle dağınık, uzaklıkla genişleyen lob | Crest | yüksek | düşük |
| 9 | Gövde rengi: RGB Beer–Lambert, bakış açısına bağlı derin renk, sığ renk, bölge ortam ışığı; SSS | Crest | **yüksek** | düşük |
| 10 | Kırılma: kalınlık/uzaklık ölçekli, sızıntı reddi | Crest, Mistral | orta | düşük |
| 11 | Köpük: kalıcı dünya köpük hafızası (kıyı + Jacobian), kabarcık katmanı, köpük ışığı | Crest | yüksek | orta |
| 12 | Kostik: güneşe projeksiyon, odak, ortalamayı koruyan modülasyon | Crest | orta | düşük |
| 13–14 | Dalga denklemi simülasyonu + oyuncu girdisi (iz kendiliğinden) | Crest | yüksek | orta |
