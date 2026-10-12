# Vulkana Görüntü Düzenleyici

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

Öncelikle Linux için geliştirilen; boyama, rötuş ve katmanlı kompozisyon için
yerel Windows derlemeleri, Qt çalışma alanı ve Vulkan ile hızlandırılan tuval
sunan bir görüntü düzenleyici.

![Katmanlı görüntü, fırça hazır ayarları ve ayarlamalar içeren Vulkana çalışma alanı](../../res/Preview.png)

## Özellikler

- **Katmanlı düzenleme:** raster katmanlar, düzenlenebilir metin ve şekiller; bitmap
  maskeleri, klasörler, kırpma maskesi grupları, çoklu seçim, karıştırma modları,
  çoğaltma, birleştirme ve isteğe bağlı rasterleştirme.
- **Boyama ve rötuş:** basınca duyarlı fırçalar, işlemsel ve dokulu hazır ayarlar,
  silgi, dolgu, yerel bulanıklaştırma, Klonlama Damgası, İyileştirme ve otomatik Nokta İyileştirme.
- **Seçimler:** dikdörtgen, elips, serbest/poligonal/manyetik kement, renge göre seçim
  ve fırçayla yönlendirilen Hızlı Seçim; büyütme/küçültme, ters çevirme ve yeniden seçme.
- **Nesne Seçimi ve iyileştirme:** birlikte gelen çevrimdışı modeli kullanarak bir
  nesneyi kabaca çizilmiş bir dikdörtgenle seçme; Seçimi İyileştir, canlı önizlemeler
  ve elle düzeltmelerle seçimleri ve bitmap maskelerini iyileştirme. Nesne Seçimi
  için model indirmeniz veya Python yüklemeniz gerekmez.
- **Dönüşümler:** taşıma, ölçekleme, döndürme, çevirme ve köşeleri çarpıtma;
  yakalama ve hizalama kılavuzlarıyla seçim sınırlarını veya seçili pikselleri dönüştürme.
- **Tahribatsız ayarlamalar:** katman başına düzeltmeler ve Aşağıdakilerin Tümü / Bu Grup
  kapsamına sahip maskeli ayarlama katmanları; pozlama, düzeyler, eğriler, ton/doygunluk,
  canlılık, renk dengesi, sıcaklık/renk tonu, tek renkli dönüşüm ve renkleri tersine çevirme.
- **Filtreler ve katman efektleri:** Gauss, Hareket ve Lens Bulanıklığı; Kontur,
  Dış/İç Gölge, Dış/İç Işıma, Renk Kaplama, Degrade Kaplama ve Eğim ve Kabartma.
- **Birden fazla belge:** bağımsız geçmişe, seçimlere ve görünüm durumuna sahip
  sekmeler; belgeler arasında düzenlenebilir katman kopyalama.
- **PSD içe aktarma:** dönüşümleri gözden geçirerek yerel katmanlara, zengin metne,
  basit şekillere, bitmap maskelerine, kırpma gruplarına ve desteklenen stillere aktarma;
  gerektiğinde kayıtlı raster verisinin kullanılacağını açıkça belirtme.
- **PSD dışa aktarma:** katmanlı veya düzleştirilmiş çıktı; desteklendiği durumlarda
  düzenlenebilir metin/şekil/maskeler ve kırpma, gözden geçirilebilir dönüşümler
  ve yeniden oluşturulan birleşik görüntü.
- **PDF içe aktarma:** seçilen sayfaları belirlediğiniz PPI değerinde, raster katmanlar
  veya ayrı belgeler olarak, saydam ya da beyaz arka planlarla açma.
- **PDF dışa aktarma:** birleşik görüntüyü veya katmanları/grupları sayfa olarak aktarma,
  özel sayfa seçimi, tuval boyutu veya içeriğe uygun boyutlar ve desteklendiği durumlarda
  seçilebilir metin.
- **Çalışma alanı ve çıktı:** sekmeli, kenetlenebilir paneller; kaydırma/yakınlaştırma
  denetimli Gezgin, temalar, düzenlenebilir kısayollar, cetveller, Piksel Önizleme,
  yerel `.vulkana` projeleri ve PNG/JPEG/WebP dışa aktarma.
- **Diller:** Tercihler → Genel → Dil bölümünden yeniden başlatmadan seçilebilen
  16 arayüz dili. Tüm dil sürümleri ilk taslaktır; inceleme ve düzeltmeler
  memnuniyetle karşılanır.

Aktif olarak geliştirilen bir beta sürümüdür. Önemli projelerinizin yedeklerini saklayın.

## Derleme ve çalıştırma

Linux ve Windows derlemeleri x86-64 içindir. Depoyu klonlayın, ardından
kök dizininden platformunuza uygun komutları çalıştırın:

```sh
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

### Linux

Linux, C++20 derleyicisi, Ninja, pkg-config ve şu geliştirme kitaplıkları gereklidir:

- Sağlanan hazır yapılandırmalar için CMake **3.28+**.
- Qt **6.8+**: Core, Gui, Widgets, Network, DBus ve PDF; testler için Qt Test.
- Dil paketlerini derlemek için Qt Linguist `lrelease` ve Python 3 (yalnızca derleme araçları).
- qpdf **11+** geliştirme kitaplığı (PDF içe/dışa aktarma için; komut satırı aracı değil).
- zlib uyumlu geliştirme kitaplığı (PSD ZIP kanalları).
- Vulkan **1.2+** başlıkları/yükleyicisi, çalışan bir Vulkan sürücüsü ve `glslc`.
- minizip-ng **4.x** (pkg-config modülü `minizip`; eski minizip değil).
- libwebp ve libwebpmux **1.2+**.

Masaüstünüz için Qt platform eklentilerini, SVG desteğini ve görüntü biçimi
eklentilerini kurun. Qt kurulumu sistemin arama yolu dışındaysa `CMAKE_PREFIX_PATH`
değerini bu kuruluma yönlendirin.
Qt PDF birçok dağıtımda ayrı bir geliştirme paketidir (örneğin Fedora'da
`qt6-qtpdf-devel`, Ubuntu'da `qt6-pdf-dev`). Vulkana'nın çalışması için WebEngine
tarayıcısı gerektirmez.
Qt Linguist araçları Fedora'da `qt6-linguist`, Ubuntu'da `qt6-tools-dev-tools`
paketinde bulunur. Gerekirse `-DVULKANA_LRELEASE=/path/to/lrelease` ayarlayın.
Qt'nin standart iletişim kutusu çeviri katalogları Fedora'da `qt6-qttranslations`,
Ubuntu'da `qt6-translations-l10n` paketinden gelir. Yalnızca dil paketlerini derleme
ve gözden geçirme için [yerelleştirme belgesine](../LOCALIZATION.md) bakın.

Deponun kök dizininden:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

Projeler ve görüntüler argüman olarak da verilebilir:

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

Qt, oturuma göre Wayland veya X11'i seçer. Tercihler ve özel hazır ayarlar kullanıcıya
özel depolamada tutulur. Çevrimiçi hizmetler isteğe bağlıdır;
[hizmet yapılandırması](../../config/SERVICES.md) olmadan güncellemeler ve geri bildirim
devre dışı kalır.

Release testleri için test hedeflerini etkinleştirip yeniden derleyin:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=ON
cmake --build --preset release --parallel 6
ctest --preset release
```

Debug hazır yapılandırması da kullanılabilir. Yerel işleme kontrolleri Vulkan doğrulama katmanlarını gerektirir.
Bazı etkileşim testleri çalışan bir Wayland/X11 masaüstü gerektirir.

### Windows

[Windows derleme kılavuzunda](../../tools/windows/README.md#prerequisites) belirtilen gerekli bileşenleri kurun.
Deponun kök dizininde Windows PowerShell’i açın, ardından uygulamayı derleyip başlatın:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Build.ps1
C:\Dev\Vulkana\stage\bin\imageeditor.exe
```

Betik, bağımlılıkları hazırlar ve dil paketlerini içeren bir Release derlemesi oluşturur.
[Özel derleme yolları](../../tools/windows/README.md#configuration),
[testler](../../tools/windows/README.md#testing) ve
[taşınabilir klasörler](../../tools/windows/README.md#portable-folder) için kılavuza bakın.

## Mimari

- `src/core` — platformdan bağımsız belgeler, katmanlar, geometri, görüntü algoritmaları
  ve geri alınabilir işlemler.
- `src/render` — paylaşılan Vulkan tuvali, birleştirme, kaplamalar ve boyutu sınırlı GPU önbellekleri.
- `src/ui` — Qt panelleri, araçlar, belge sekmeleri, metin/şekil hazırlığı ve dosya giriş/çıkışı.
- `src/platform` — kaynak arama, masaüstü bütünleştirmesi ve isteğe bağlı hizmetler.
- `src/app` — uygulama giriş noktası; `shaders` — Vulkan gölgelendirici kaynakları.

Her belgenin içeriği ve geçmişi kendisine aittir; sekmeler tek bir çalışma alanını
ve Vulkan işleyicisini paylaşır. Düzenlenebilir kaynak verileri işleme önbelleklerinden
ayrı tutulur. Tuval, Piksel Önizleme ve dışa aktarma aynı birleştirme kurallarını kullanır.

[Geliştirici belgelerine](../README.md) ve [mimari kılavuzuna](../ARCHITECTURE.md) bakın.

## Lisans

Vulkana'ya ait kod, belgeler ve varlıklar [MIT lisanslıdır](../../LICENSE).
Üçüncü taraf materyaller kendi lisanslarını korur; [üçüncü taraf bildirimlerine](../../THIRD_PARTY.md) bakın.
