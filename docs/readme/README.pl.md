# Edytor obrazów Vulkana

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

Edytor obrazów tworzony przede wszystkim dla Linuksa, przeznaczony do malowania,
retuszu i kompozycji warstwowych, z natywnymi kompilacjami dla Windows,
obszarem roboczym Qt i płótnem przyspieszanym przez Vulkan.

![Obszar roboczy Vulkana z obrazem warstwowym, ustawieniami pędzli i korektami](../../res/Preview.png)

## Funkcje

- **Edycja warstwowa:** warstwy rastrowe, edytowalny tekst i kształty; maski bitmapowe,
  foldery, grupy masek przycinających, zaznaczanie wielu warstw, tryby mieszania,
  powielanie, scalanie i rasteryzacja na żądanie.
- **Malowanie i retusz:** pędzle reagujące na nacisk, ustawienia proceduralne i teksturowane,
  gumka, wypełnienie, miejscowe rozmycie, stempel klonujący, pędzel korygujący
  i automatyczny punktowy pędzel korygujący.
- **Zaznaczenia:** prostokąt, elipsa, lasso odręczne/wielokątne/magnetyczne,
  zaznaczanie według koloru i szybkie zaznaczanie prowadzone pędzlem,
  z powiększaniem/pomniejszaniem, odwracaniem i przywracaniem zaznaczenia.
- **Zaznaczanie obiektów i dopracowywanie:** zaznaczanie obiektu za pomocą przybliżonego
  prostokąta i dołączonego modelu działającego offline; dopracowywanie zaznaczeń i masek
  bitmapowych z podglądem na żywo i ręcznymi poprawkami. Zaznaczanie obiektów nie wymaga
  pobierania modelu ani instalacji Pythona.
- **Przekształcenia:** przesuwanie, skalowanie, obracanie, odbijanie i zniekształcanie
  narożników; przekształcanie granic zaznaczenia lub zaznaczonych pikseli,
  z przyciąganiem i prowadnicami wyrównania.
- **Niedestrukcyjne korekty:** korekty poszczególnych warstw i maskowane warstwy korekcyjne
  o zasięgu Wszystkie poniżej / Ta grupa; ekspozycja, poziomy, krzywe, odcień/nasycenie,
  intensywność kolorów, balans kolorów, ciepło/zabarwienie, konwersja do monochromatyczności
  i odwracanie kolorów.
- **Filtry i efekty warstw:** rozmycie gaussowskie, ruchowe i soczewkowe; obrys,
  cień zewnętrzny/wewnętrzny, blask zewnętrzny/wewnętrzny, nałożenie koloru,
  nałożenie gradientu oraz fazowanie i płaskorzeźba.
- **Wiele dokumentów:** karty z niezależną historią, zaznaczeniami i stanem widoku
  oraz kopiowanie edytowalnych warstw między dokumentami.
- **Import PSD:** konwersja z możliwością sprawdzenia do rodzimych warstw, tekstu
  sformatowanego, prostych kształtów, masek bitmapowych, grup przycinających
  i obsługiwanych stylów, z wyraźnie wskazanym użyciem zapisanego rastra w razie potrzeby.
- **Eksport PSD:** wynik warstwowy lub spłaszczony, edytowalny tekst/kształty/maski
  i przycinanie tam, gdzie są obsługiwane, z możliwością sprawdzenia konwersji
  i nowo wygenerowanym obrazem złożonym.
- **Import PDF:** wybrane strony z wybraną rozdzielczością PPI, jako warstwy rastrowe
  lub osobne dokumenty, z przezroczystym albo białym tłem.
- **Eksport PDF:** obraz złożony lub warstwy/grupy jako strony, własny wybór stron,
  rozmiary płótna lub dopasowane oraz tekst możliwy do zaznaczenia tam, gdzie jest obsługiwany.
- **Obszar roboczy i zapis:** dokowalne panele z kartami, Nawigator z przesuwaniem
  i powiększaniem, motywy, edytowalne skróty, linijki, podgląd pikseli, rodzime
  projekty `.vulkana` i eksport PNG/JPEG/WebP.
- **Języki:** 16 języków interfejsu, wybieranych w Preferencje → Ogólne → Język
  bez ponownego uruchamiania. Wszystkie wersje językowe są wstępne;
  zachęcamy do ich sprawdzania i zgłaszania poprawek.

Aktywnie rozwijana wersja beta. Zachowuj kopie zapasowe ważnych projektów.

## Budowanie i uruchamianie

Kompilacje dla Linuksa i Windows są przeznaczone dla architektury x86-64.
Sklonuj repozytorium, a następnie uruchom polecenia dla swojej platformy
z jego katalogu głównego:

```sh
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

### Linux

Wymagane są Linux, kompilator C++20, Ninja, pkg-config i następujące biblioteki deweloperskie:

- CMake **3.28+** dla dołączonych presetów.
- Qt **6.8+**: Core, Gui, Widgets, Network, DBus i PDF; Qt Test do testów.
- Qt Linguist `lrelease` i Python 3 do budowania pakietów językowych (tylko narzędzia budowania).
- Biblioteka deweloperska qpdf **11+** (import/eksport PDF; nie narzędzie wiersza poleceń).
- Biblioteka deweloperska zgodna z zlib (kanały ZIP w PSD).
- Nagłówki i moduł ładujący Vulkan **1.2+**, działający sterownik Vulkan oraz `glslc`.
- minizip-ng **4.x** (moduł pkg-config `minizip`, nie starszy minizip).
- libwebp i libwebpmux **1.2+**.

Zainstaluj wtyczki platformowe Qt odpowiednie dla swojego pulpitu, obsługę SVG i wtyczki
formatów obrazów. Ustaw `CMAKE_PREFIX_PATH` na instalację Qt, jeśli znajduje się poza
systemową ścieżką wyszukiwania.
W wielu dystrybucjach Qt PDF jest osobnym pakietem deweloperskim (np. `qt6-qtpdf-devel`
w Fedorze, `qt6-pdf-dev` w Ubuntu). Nie wymaga przeglądarki WebEngine do uruchamiania Vulkany.
Narzędzia Qt Linguist są dostępne w pakiecie `qt6-linguist` w Fedorze lub
`qt6-tools-dev-tools` w Ubuntu. W razie potrzeby ustaw `-DVULKANA_LRELEASE=/path/to/lrelease`.
Katalogi tłumaczeń standardowych okien dialogowych Qt pochodzą z pakietu `qt6-qttranslations`
w Fedorze lub `qt6-translations-l10n` w Ubuntu. Informacje o budowaniu samych pakietów
i ich sprawdzaniu znajdziesz w [dokumentacji lokalizacji](../LOCALIZATION.md).

W katalogu głównym repozytorium:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

Projekty i obrazy można też przekazywać jako argumenty:

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

Qt wybiera Wayland lub X11 na podstawie sesji. Preferencje i własne ustawienia wstępne
są przechowywane osobno dla każdego użytkownika. Usługi online są opcjonalne;
bez [konfiguracji usług](../../config/SERVICES.md) aktualizacje i przesyłanie opinii
pozostają wyłączone.

Do testów wersji Release włącz cele testowe i ponownie zbuduj projekt:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=ON
cmake --build --preset release --parallel 6
ctest --preset release
```

Preset debug jest również dostępny. Kontrole renderowania natywnego wymagają warstw walidacyjnych Vulkan.
Niektóre testy interakcji wymagają uruchomionego pulpitu Wayland/X11.

### Windows

Zainstaluj PowerShell 7, Git, CMake, Ninja, Python 3, narzędzia C++ x64 programu
Visual Studio, Qt dla MSVC x64, Vulkan SDK i vcpkg. Użyj wersji i składników
wymienionych w [przewodniku budowania dla Windows](../../tools/windows/README.md).

W PowerShell 7, z katalogu głównego repozytorium:

```powershell
pwsh -NoProfile -File tools/windows/Setup.ps1 -BuildRoot C:\Dev\VulkanaClean
pwsh -NoProfile -File tools/windows/Build.ps1 -BuildRoot C:\Dev\VulkanaClean -Tests
C:\Dev\VulkanaClean\stage\bin\imageeditor.exe
```

Do testu świeżego klonu wybierz nowy katalog budowania; nie używaj katalogu
skonfigurowanego dla innej kopii repozytorium. Setup sprawdza zależności
i wymagania wstępne; Build kompiluje pakiety językowe, buduje i testuje aplikację
oraz przygotowuje pliki potrzebne do jej uruchomienia. Pierwsza konfiguracja wymaga
internetu do pobrania zależności budowania, ale aplikacja i dołączone Zaznaczanie
obiektów działają lokalnie, bez Pythona i pobierania modeli.

Przewodnik dla Windows opisuje własne ścieżki zależności, natywne testy podstawowe
i tworzenie czystego folderu wersji przenośnej. Preferencje pozostają w przestrzeni
użytkownika, niezależnie od folderu aplikacji.

## Architektura

- `src/core` — niezależne od platformy dokumenty, warstwy, geometria, algorytmy obrazów
  i transakcje z możliwością cofania.
- `src/render` — wspólne płótno Vulkan, kompozycja, nakładki i ograniczone pamięci podręczne GPU.
- `src/ui` — panele Qt, narzędzia, karty dokumentów, przygotowanie tekstu/kształtów oraz odczyt i zapis plików.
- `src/platform` — wyszukiwanie zasobów, integracja z pulpitem i opcjonalne usługi.
- `src/app` — punkt wejścia aplikacji; `shaders` — kod źródłowy shaderów Vulkan.

Każdy dokument ma własną zawartość i historię; karty współdzielą obszar roboczy
i renderer Vulkan. Edytowalne dane źródłowe są oddzielone od pamięci podręcznych
renderowania. Płótno, podgląd pikseli i eksport korzystają z tych samych reguł kompozycji.

Zobacz [dokumentację dla programistów](../README.md) i [przewodnik po architekturze](../ARCHITECTURE.md).

## Licencja

Kod, dokumentacja i zasoby należące do Vulkany są objęte [licencją MIT](../../LICENSE).
Materiały stron trzecich zachowują własne licencje; zobacz [informacje o materiałach stron trzecich](../../THIRD_PARTY.md).
