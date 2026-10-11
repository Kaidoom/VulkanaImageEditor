# Vulkana — Bildeditor

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

Ein vorrangig für Linux entwickelter Bildeditor zum Malen, Retuschieren und
Gestalten mit Ebenen, mit einer Qt-Arbeitsoberfläche und einer Vulkan-beschleunigten Leinwand.

![Vulkana-Arbeitsoberfläche mit einem Bild mit mehreren Ebenen, Pinselvorgaben und Anpassungen](../../res/Preview.png)

## Funktionen

- **Bearbeitung mit Ebenen:** Rasterebenen, bearbeitbarer Text und Formen;
  Bitmap-Masken, Ordner, Schnittmaskengruppen, Mehrfachauswahl, Mischmodi,
  Duplizieren, Zusammenführen und ausdrückliches Rastern.
- **Malen und Retuschieren:** druckempfindliche Pinsel, prozedurale und texturierte
  Vorgaben, Radieren, Füllen, lokales Weichzeichnen, Kopierstempel, Reparaturpinsel
  und automatische Bereichsreparatur.
- **Auswahlen:** Rechteck, Ellipse, Freihand-, Polygon- und magnetisches Lasso,
  Farbauswahl und pinselgeführte Schnellauswahl, mit Vergrößern, Verkleinern,
  Umkehren und Wiederherstellen der Auswahl.
- **Objektauswahl und Verfeinerung:** Ein Objekt lässt sich mit einem groben
  Rechteck und dem mitgelieferten Offline-Modell auswählen. Auswahlen und
  Bitmap-Masken lassen sich mit „Auswahl verfeinern“, Live-Vorschauen und manuellen
  Korrekturen verbessern. Für die Objektauswahl muss weder ein Modell
  heruntergeladen noch Python installiert werden.
- **Transformationen:** Verschieben, Skalieren, Drehen, Spiegeln und Verzerren
  über die Eckpunkte; Auswahlgrenzen oder ausgewählte Pixel transformieren,
  mit Einrasten und Ausrichtungshilfen.
- **Nicht destruktive Anpassungen:** Korrekturen pro Ebene und maskierte
  Einstellungsebenen mit Geltungsbereich für alle darunterliegenden Ebenen oder
  die aktuelle Gruppe; Belichtung, Tonwertkorrektur, Gradationskurven,
  Farbton/Sättigung, Dynamik, Farbbalance, Wärme/Farbton, Monochromumwandlung
  und Umkehrung.
- **Filter und Ebeneneffekte:** Gaußscher Weichzeichner, Bewegungs- und
  Objektivunschärfe; Kontur, Schlag- und innerer Schatten, Schein nach außen/innen,
  Farbüberlagerung, Verlaufsüberlagerung sowie abgeflachte Kante und Relief.
- **Mehrere Dokumente:** Tabs mit jeweils eigenem Verlauf, eigenen Auswahlen
  und eigenem Ansichtsstatus; bearbeitbare Ebenen lassen sich zwischen Dokumenten kopieren.
- **PSD-Import:** Konvertierung mit Überprüfung in native Ebenen, formatierten
  Text, einfache Formen, Bitmap-Masken, Schnittmaskengruppen und unterstützte
  Stile, mit ausdrücklichem Rückgriff auf gespeicherte Rasterversionen bei Bedarf.
- **PSD-Export:** Ausgabe mit Ebenen oder auf eine Ebene reduziert, bearbeitbare
  Texte, Formen und Masken sowie Beschneidung durch Schnittmasken, soweit
  unterstützt, mit Überprüfung der Konvertierungen und einem neu berechneten Gesamtbild.
- **PDF-Import:** ausgewählte Seiten mit frei wählbarer PPI-Auflösung, als
  Rasterebenen oder separate Dokumente, mit transparentem oder weißem Hintergrund.
- **PDF-Export:** ein Gesamtbild oder Ebenen/Gruppen als Seiten, benutzerdefinierte
  Seitenauswahl, Leinwandgröße oder an den Inhalt angepasste Größen sowie
  auswählbarer Text, soweit unterstützt.
- **Arbeitsoberfläche und Ausgabe:** andockbare Bedienfelder mit Tabs, ein
  Navigator mit Schwenk- und Zoomsteuerung, Designs, bearbeitbare Tastenkürzel,
  Lineale, Pixelvorschau, native `.vulkana`-Projekte und PNG/JPEG/WebP-Export.
- **Sprachen:** 16 Oberflächensprachen, auswählbar unter Einstellungen → Allgemein →
  Sprache ohne Neustart. Die neuen Übersetzungen sind eine erste Fassung;
  Korrekturen sind willkommen.

Beta-Version in aktiver Entwicklung. Bitte Sicherungskopien wichtiger Projekte aufbewahren.

## Kompilieren und starten

Benötigt werden Linux, ein C++20-Compiler, Ninja, pkg-config und diese Entwicklungsbibliotheken:

- CMake **3.28+** für die mitgelieferten Presets.
- Qt **6.8+**: Core, Gui, Widgets, Network, DBus und PDF; Qt Test für Tests.
- Qt Linguist `lrelease` und Python 3 zum Erstellen der Sprachpakete (nur beim Kompilieren benötigt).
- qpdf-Entwicklungsbibliothek **11+** (PDF-Import/-Export; nicht das Kommandozeilenprogramm).
- zlib-kompatible Entwicklungsbibliothek (PSD-ZIP-Kanäle).
- Vulkan-Header und -Loader **1.2+**, ein funktionierender Vulkan-Treiber und `glslc`.
- minizip-ng **4.x** (das pkg-config-Modul `minizip`, nicht das alte minizip).
- libwebp und libwebpmux **1.2+**.

Die Qt-Plattformplugins für den verwendeten Desktop, SVG-Unterstützung und
Bildformatplugins müssen installiert sein. `CMAKE_PREFIX_PATH` muss auf die
Qt-Installation zeigen, falls sie außerhalb der Suchpfade des Systems liegt.
Qt PDF ist in vielen Distributionen ein separates Entwicklungspaket (zum Beispiel
`qt6-qtpdf-devel` unter Fedora, `qt6-pdf-dev` unter Ubuntu). Vulkana benötigt dafür
zur Laufzeit keinen WebEngine-Browser.
Die Qt-Linguist-Werkzeuge sind in `qt6-linguist` unter Fedora beziehungsweise
`qt6-tools-dev-tools` unter Ubuntu enthalten. Bei Bedarf
`-DVULKANA_LRELEASE=/path/to/lrelease` setzen. Die Übersetzungskataloge für die
Standarddialoge von Qt stammen aus `qt6-qttranslations` unter Fedora oder
`qt6-translations-l10n` unter Ubuntu. Hinweise zum separaten Erstellen und Prüfen
der Sprachpakete stehen in der [Lokalisierungsdokumentation](../LOCALIZATION.md).

Im Stammverzeichnis des Repositorys:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

Projekte und Bilder können auch als Argumente übergeben werden:

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

Qt wählt je nach Sitzung Wayland oder X11. Einstellungen und benutzerdefinierte
Vorgaben werden pro Benutzer gespeichert. Onlinedienste sind optional; ohne
[Dienstkonfiguration](../../config/SERVICES.md) bleiben Updates und das Senden
von Feedback deaktiviert.

Für Entwicklung und Tests:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

Prüfungen des Debug-/nativen Renderings benötigen die Vulkan-Validierungsschichten.
Einige Interaktionstests erfordern eine laufende Wayland/X11-Desktopsitzung.

## Architektur

- `src/core` — plattformunabhängige Dokumente, Ebenen, Geometrie, Bildalgorithmen
  und rückgängig machbare Transaktionen.
- `src/render` — gemeinsame Vulkan-Leinwand, Komposition, Überlagerungen und
  größenbegrenzte GPU-Caches.
- `src/ui` — Qt-Bedienfelder, Werkzeuge, Dokument-Tabs, Vorbereitung von Text und
  Formen sowie Datei-Ein-/Ausgabe.
- `src/platform` — Ressourcensuche, Desktopintegration und optionale Dienste.
- `src/app` — Einstiegspunkt der Anwendung; `shaders` — Vulkan-Shader-Quellcode.

Jedes Dokument verwaltet seinen eigenen Inhalt und Verlauf; die Tabs teilen sich
eine Arbeitsoberfläche und einen Vulkan-Renderer. Bearbeitbare Quelldaten bleiben
von den Render-Caches getrennt. Leinwand, Pixelvorschau und Export verwenden
dieselben Kompositionsregeln.

Siehe die [Entwicklerdokumentation](../README.md) und den
[Architekturleitfaden](../ARCHITECTURE.md).

## Lizenz

Code, Dokumentation und Ressourcen im Besitz von Vulkana stehen unter der
[MIT-Lizenz](../../LICENSE). Material von Drittanbietern behält seine jeweiligen
Lizenzen; siehe die [Drittanbieterhinweise](../../THIRD_PARTY.md).
