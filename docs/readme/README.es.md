# Vulkana — Editor de imágenes

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

Un editor de imágenes diseñado principalmente para Linux, para pintar, retocar
y componer con capas, con un espacio de trabajo Qt y un lienzo acelerado por Vulkan.

![Espacio de trabajo de Vulkana con una imagen con capas, preajustes de pincel y ajustes](../../res/Preview.png)

## Funciones

- **Edición con capas:** capas ráster, texto y formas editables; máscaras de mapa
  de bits, carpetas, grupos de máscaras de recorte, selección múltiple, modos de
  fusión, duplicación, combinación y rasterización explícita.
- **Pintura y retoque:** pinceles sensibles a la presión, preajustes procedurales
  y con textura, borrado, relleno, desenfoque local, tampón de clonar, corrector
  y corrector puntual automático.
- **Selecciones:** rectángulo, elipse, lazo a mano alzada/poligonal/magnético,
  selección por color y selección rápida guiada por pincel, con ampliación,
  reducción, inversión y recuperación de la selección.
- **Selección de objetos y perfeccionamiento:** selecciona un objeto con un
  rectángulo aproximado mediante el modelo sin conexión incluido; mejora las
  selecciones y las máscaras de mapa de bits con Perfeccionar selección, vistas
  previas en directo y correcciones manuales. La selección de objetos no requiere
  descargar un modelo ni instalar Python.
- **Transformaciones:** mover, escalar, rotar, voltear y distorsionar desde las
  esquinas; transforma los límites de la selección o los píxeles seleccionados,
  con ajuste automático y guías de alineación.
- **Ajustes no destructivos:** correcciones por capa y capas de ajuste con máscara,
  con alcance a todas las capas inferiores o al grupo actual; exposición, niveles,
  curvas, tono/saturación, intensidad, equilibrio de color, calidez/tinte,
  conversión monocroma e inversión.
- **Filtros y efectos de capa:** desenfoque gaussiano, de movimiento y de lente;
  trazo, sombra paralela/interior, resplandor exterior/interior, superposición de
  color, superposición de degradado y bisel y relieve.
- **Varios documentos:** pestañas con historial, selecciones y estado de vista
  independientes, además de copia de capas editables entre documentos.
- **Importación PSD:** conversión con revisión a capas nativas, texto enriquecido,
  formas simples, máscaras de mapa de bits, grupos de recorte y estilos compatibles,
  con uso explícito de las versiones ráster guardadas cuando es necesario.
- **Exportación PSD:** salida con capas o acoplada, texto/formas/máscaras editables
  y recorte cuando se admite, con revisión de las conversiones y una imagen
  compuesta recién generada.
- **Importación PDF:** páginas seleccionadas a los PPI que elijas, como capas
  ráster o documentos separados, con fondo transparente o blanco.
- **Exportación PDF:** una imagen compuesta o capas/grupos como páginas, selección
  personalizada de páginas, tamaños del lienzo o ajustados al contenido y texto
  seleccionable cuando se admite.
- **Espacio de trabajo y salida:** paneles acoplables con pestañas, un navegador
  con controles de desplazamiento y zoom, temas, atajos editables, reglas, vista
  previa de píxeles, proyectos nativos `.vulkana` y exportación PNG/JPEG/WebP.
- **Idiomas:** 16 idiomas de interfaz, seleccionables en Preferencias → General →
  Idioma sin reiniciar. Las nuevas traducciones son una primera versión; las
  correcciones son bienvenidas.

Versión beta en desarrollo activo. Conserva copias de seguridad de los proyectos importantes.

## Compilar y ejecutar

Requiere Linux, un compilador C++20, Ninja, pkg-config y estas bibliotecas de desarrollo:

- CMake **3.28+** para los preajustes incluidos.
- Qt **6.8+**: Core, Gui, Widgets, Network, DBus y PDF; Qt Test para las pruebas.
- Qt Linguist `lrelease` y Python 3 para compilar paquetes de idiomas (solo herramientas de compilación).
- Biblioteca de desarrollo qpdf **11+** (importación/exportación PDF; no la herramienta de línea de comandos).
- Biblioteca de desarrollo compatible con zlib (canales ZIP de PSD).
- Cabeceras y cargador de Vulkan **1.2+**, un controlador Vulkan funcional y `glslc`.
- minizip-ng **4.x** (el módulo pkg-config `minizip`, no el antiguo minizip).
- libwebp y libwebpmux **1.2+**.

Instala los complementos de plataforma de Qt para tu escritorio, la compatibilidad
con SVG y los complementos de formatos de imagen. Apunta `CMAKE_PREFIX_PATH` a tu
instalación de Qt si está fuera de las rutas de búsqueda del sistema.
Qt PDF es un paquete de desarrollo separado en muchas distribuciones (por ejemplo,
`qt6-qtpdf-devel` en Fedora, `qt6-pdf-dev` en Ubuntu). No requiere un navegador
WebEngine durante la ejecución de Vulkana.
Las herramientas de Qt Linguist se incluyen en `qt6-linguist` en Fedora o
`qt6-tools-dev-tools` en Ubuntu. Configura `-DVULKANA_LRELEASE=/path/to/lrelease`
si es necesario. Los catálogos de los diálogos estándar de Qt proceden de
`qt6-qttranslations` en Fedora o `qt6-translations-l10n` en Ubuntu. Consulta la
[documentación de localización](../LOCALIZATION.md) para compilar solo los paquetes
y revisarlos.

Desde la raíz del repositorio:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

También se pueden pasar proyectos e imágenes como argumentos:

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

Qt elige Wayland o X11 según la sesión. Las preferencias y los preajustes
personalizados se guardan en el almacenamiento de cada usuario. Los servicios en
línea son opcionales; sin la [configuración de servicios](../../config/SERVICES.md),
las actualizaciones y el envío de comentarios permanecen desactivados.

Para desarrollo y pruebas:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

Las comprobaciones de renderizado en modo debug o nativo necesitan las capas de
validación de Vulkan. Algunas pruebas de interacción requieren un escritorio
Wayland/X11 en ejecución.

## Arquitectura

- `src/core` — documentos, capas, geometría, algoritmos de imagen y transacciones
  que se pueden deshacer, independientes de la plataforma.
- `src/render` — lienzo Vulkan compartido, composición, superposiciones y cachés
  de GPU de tamaño limitado.
- `src/ui` — paneles Qt, herramientas, pestañas de documentos, preparación de
  texto/formas y entrada/salida de archivos.
- `src/platform` — búsqueda de recursos, integración con el escritorio y servicios opcionales.
- `src/app` — punto de entrada de la aplicación; `shaders` — código fuente de los shaders de Vulkan.

Cada documento posee su contenido e historial; las pestañas comparten un espacio
de trabajo y un renderizador Vulkan. Los datos de origen editables se mantienen
separados de las cachés de renderizado. El lienzo, la vista previa de píxeles y la
exportación comparten las mismas reglas de composición.

Consulta la [documentación para desarrolladores](../README.md) y la
[guía de arquitectura](../ARCHITECTURE.md).

## Licencia

El código, la documentación y los recursos propiedad de Vulkana se distribuyen
[bajo la licencia MIT](../../LICENSE). El material de terceros conserva sus propias
licencias; consulta los [avisos de terceros](../../THIRD_PARTY.md).
