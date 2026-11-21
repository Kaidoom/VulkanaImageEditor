# Vulkana — Bildeditor

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

Ein vorrangig für Linux entwickelter Bildeditor zum Malen, Retuschieren und
Gestalten mit Ebenen, auch als nativer Windows-Build verfügbar, mit einer
Qt-Arbeitsoberfläche und einer Vulkan-beschleunigten Leinwand.

![Vulkana-Arbeitsoberfläche mit einem Bild mit mehreren Ebenen, Pinselvorgaben und Anpassungen](../../res/Preview.png)

## Funktionen

- **Bearbeitung mit Ebenen:** Rasterebenen, bearbeitbarer Text und Formen;
  Bitmap-Masken, Ordner, Schnittmaskengruppen, Mehrfachauswahl, Mischmodi,
  Duplizieren, Zusammenführen und ausdrückliches Rastern.
- **Verknüpfte Projekte und Spiegelebenen** machen Bildinhalte wiederverwendbar.
  Ein Doppelklick auf eine [Projektebene](../PROJECT_LAYERS.md) öffnet das
  Quellprojekt zur Bearbeitung. Bei der Rückkehr zum übergeordneten Projekt sind
  die Änderungen sichtbar. Eine [Spiegelebene](../MIRRORED_LAYERS_AND_GUIDES.md)
  folgt ihrer Quelle im selben Dokument und kann eigene Transformationen und
  Effekte erhalten.
- **Kanalgruppen:** einzelne Kanäle als Graustufenebenen für gepackte Texturen
  bearbeiten, mit verlustfreier Umwandlung, numerischem Malen, Retusche und
  Anpassungen. Siehe die [Anleitung zu Kanalgruppen](../CHANNEL_GROUPS.md).
- **Malen und Retuschieren** umfasst druckempfindliche Pinsel, Füllen, lokales
  Weichzeichnen, Kopierstempel und Reparaturwerkzeuge. Mit der
  [Pinselsymmetrie](../BRUSH_SYMMETRY.md) lassen sich Striche an einer frei
  platzierbaren Achse spiegeln, auch auf Ebenenmasken. Die Bereichsreparatur kann
  transparente Lücken füllen und sichtbare Ebenen ober- und unterhalb der
  Retuscheebene als Quelle nutzen.
- **Auswahlen:** Rechteck, Ellipse, Freihand-, Polygon- und magnetisches Lasso,
  Farbauswahl und pinselgeführte Schnellauswahl, mit Vergrößern, Verkleinern,
  Umkehren und Wiederherstellen der Auswahl.
- **Objektauswahl und Verfeinerung:** Ein Objekt lässt sich mit einem groben
  Rechteck und dem mitgelieferten Offline-Modell auswählen. Auswahlen und
  Bitmap-Masken lassen sich mit „Auswahl verfeinern“, Live-Vorschauen und manuellen
  Korrekturen verbessern. Für die Objektauswahl muss weder ein Modell
  heruntergeladen noch Python installiert werden.
- **Lokale KI-Bildbearbeitung** erzeugt Bilder, bearbeitet Ebenen und füllt
  Auswahlen mit eigenen Modellen und LoRAs. Varianten lassen sich vor dem
  Anwenden vergleichen. Qwen-Image-Layered kann ein Bild in einen Ordner mit
  bearbeitbaren Ebenen zerlegen. Die veröffentlichten Pakete enthalten Vulkan.
  Für NVIDIA-GPUs ist ein separater CUDA-Inferenzprozess verfügbar. Hinweise zur
  Einrichtung und zu den GPU-Anforderungen stehen unter
  [Vulkana Diffusion](../../DIFFUSION.md).
- **Transformationen** ermöglichen Verschieben, Skalieren, Drehen, Spiegeln und
  Verzerren über die Eckpunkte, auch für Auswahlgrenzen und ausgewählte Pixel.
  Ebenen lassen sich an einer mittigen Hilfslinie oder an mehreren gleichmäßig
  verteilten Hilfslinien einrasten.
- **Nicht destruktive Anpassungen:** Korrekturen pro Ebene und maskierte
  Einstellungsebenen mit Geltungsbereich für alle darunterliegenden Ebenen oder
  die aktuelle Gruppe; Belichtung, Tonwertkorrektur, Gradationskurven,
  Farbton/Sättigung, Dynamik, Farbbalance, Wärme/Farbton, Monochromumwandlung
  und Umkehrung.
- **Filter und Ebeneneffekte:** Gaußscher Weichzeichner, Bewegungs- und
  Objektivunschärfe; Kontur, Schlag- und innerer Schatten, Schein nach außen/innen,
  Farbüberlagerung, Verlaufsüberlagerung sowie abgeflachte Kante und Relief.
- **Dokument-Tabs** haben einen eigenen Verlauf, eigene Auswahlen und eine
  eigene Zoomstufe. Ein Tab lässt sich duplizieren, um Änderungen in einer
  ungespeicherten Kopie zu testen. Ebenen lassen sich zwischen Dokumenten
  kopieren und Bilder direkt in den Stapel im Ebenenbedienfeld ziehen.
- **Beim Bereinigen von Ebenen** zeigt die zoombare Transparenzvorschau feine
  Details an Haaren und weichen Kanten. „Umgekehrte Auswahl löschen“ löscht Pixel
  außerhalb der Auswahl und lässt sie aktiv. „Auf Leinwandgrenzen beschneiden“
  entfernt aus den ausgewählten Ebenen alle Pixel außerhalb der Leinwand.
- **PSD-Import** wandelt Inhalte nach Prüfung in native Ebenen, formatierten
  Text, einfache Formen, Bitmap-Masken, Schnittmaskengruppen und unterstützte
  Stile um. Im Importdialog ist ersichtlich, warum eine Konvertierung nötig ist.
  Die Entscheidung lässt sich in einem Schritt auf gleichartige Ebenen anwenden.
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
  Ausgewählte Ebenen lassen sich auf ihren gemeinsamen sichtbaren Bereich
  zugeschnitten exportieren. Die Leinwandgröße lässt sich an die ausgewählte
  Ebene anpassen.
- **Sprachen:** 16 Oberflächensprachen, auswählbar unter Einstellungen → Allgemein →
  Sprache ohne Neustart. Alle Sprachfassungen sind erste Entwürfe; alle
  Interessierten sind zum Prüfen und Korrigieren eingeladen.

Beta-Version in aktiver Entwicklung. Bitte Sicherungskopien wichtiger Projekte aufbewahren.

## Kompilieren und starten

Linux- und Windows-Builds sind für x86-64 ausgelegt. Das Repository klonen und
anschließend die Befehle für die jeweilige Plattform in dessen Stammverzeichnis ausführen:

```sh
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

### Linux

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

Für Release-Tests die Testziele aktivieren und erneut kompilieren:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=ON
cmake --build --preset release --parallel 6
ctest --preset release
```

Das Debug-Preset ist ebenfalls verfügbar. Prüfungen des nativen Renderings
benötigen die Vulkan-Validierungsschichten. Einige Interaktionstests erfordern
eine laufende Wayland/X11-Desktopsitzung.

### Windows

Installiere die in der [Windows-Build-Anleitung](../../tools/windows/README.md#prerequisites) aufgeführten Werkzeuge.
Öffne Windows PowerShell im Stammverzeichnis des Repositorys und baue und starte die Anwendung:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Build.ps1
C:\Dev\Vulkana\stage\bin\imageeditor.exe
```

Das Skript richtet die Abhängigkeiten ein und erstellt einen Release-Build mit Sprachpaketen.
Die Anleitung beschreibt [eigene Build-Pfade](../../tools/windows/README.md#configuration),
[Tests](../../tools/windows/README.md#testing) und
[portable Ordner](../../tools/windows/README.md#portable-folder).

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

Siehe die [Entwicklerdokumentation](../README.md), den [Architekturleitfaden](../ARCHITECTURE.md)
und die [Anleitung zur Einrichtung und Nutzung der MCP-Werkzeuge](../MCP.md).

## Lizenz

Code, Dokumentation und Ressourcen im Besitz von Vulkana stehen unter der
[MIT-Lizenz](../../LICENSE). Material von Drittanbietern behält seine jeweiligen
Lizenzen; siehe die [Drittanbieterhinweise](../../THIRD_PARTY.md).
