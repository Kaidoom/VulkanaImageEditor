# Vulkana — Éditeur d’images

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

Un éditeur d’images conçu en priorité pour Linux, pour peindre, retoucher et
composer avec des calques, avec un espace de travail Qt et un canevas accéléré par Vulkan.

![Espace de travail Vulkana avec une image à plusieurs calques, des préréglages de pinceaux et des réglages](../../res/Preview.png)

## Fonctionnalités

- **Édition par calques :** calques matriciels, textes et formes modifiables ;
  masques bitmap, dossiers, groupes de masques d’écrêtage, sélection multiple,
  modes de fusion, duplication, fusion et pixellisation explicite.
- **Peinture et retouche :** pinceaux sensibles à la pression, préréglages
  procéduraux et texturés, gomme, remplissage, flou local, tampon de duplication,
  correcteur et correcteur localisé automatique.
- **Sélections :** rectangle, ellipse, lasso à main levée, polygonal ou magnétique,
  sélection par couleur et sélection rapide guidée au pinceau, avec agrandissement,
  réduction, inversion et rétablissement de la sélection.
- **Sélection d’objet et amélioration :** sélectionnez un objet à l’aide d’un
  rectangle approximatif et du modèle hors ligne inclus ; améliorez les sélections
  et les masques bitmap avec l’outil d’amélioration de la sélection, ses aperçus en
  direct et les corrections manuelles. Aucun téléchargement de modèle ni aucune
  installation de Python n’est nécessaire pour utiliser la sélection d’objet.
- **Transformations :** déplacement, mise à l’échelle, rotation, retournement et
  déformation par les coins ; transformez les limites d’une sélection ou les pixels
  sélectionnés, avec magnétisme et repères d’alignement.
- **Réglages non destructifs :** corrections par calque et calques de réglage
  masqués, appliqués à tous les calques inférieurs ou au groupe courant ; exposition,
  niveaux, courbes, teinte/saturation, vibrance, balance des couleurs, chaleur/teinte,
  conversion monochrome et inversion.
- **Filtres et effets de calque :** flou gaussien, directionnel et de l’objectif ;
  contour, ombre portée/interne, lueur externe/interne, incrustation de couleur,
  incrustation en dégradé, biseautage et estampage.
- **Documents multiples :** onglets avec historique, sélections et état d’affichage
  indépendants, et copie de calques modifiables entre documents.
- **Importation PSD :** conversion avec vérification en calques natifs, texte
  enrichi, formes simples, masques bitmap, groupes d’écrêtage et styles pris en
  charge, avec recours explicite aux versions matricielles enregistrées si nécessaire.
- **Exportation PSD :** sortie avec calques ou aplatie, textes, formes et masques
  modifiables et écrêtage lorsque le format le permet, avec vérification des
  conversions et une image composite recalculée.
- **Importation PDF :** pages sélectionnées à la résolution PPI de votre choix,
  sous forme de calques matriciels ou de documents séparés, avec fond transparent ou blanc.
- **Exportation PDF :** image composite ou calques/groupes sous forme de pages,
  sélection personnalisée des pages, dimensions du canevas ou ajustées au contenu,
  et texte sélectionnable lorsque cela est pris en charge.
- **Espace de travail et sortie :** panneaux ancrables à onglets, navigateur avec
  commandes de déplacement et de zoom, thèmes, raccourcis modifiables, règles,
  aperçu des pixels, projets natifs `.vulkana` et exportation PNG/JPEG/WebP.
- **Langues :** 16 langues d’interface, sélectionnables dans Préférences → Général →
  Langue sans redémarrage. Les nouvelles traductions sont une première version ;
  les corrections sont les bienvenues.

Version bêta en développement actif. Conservez des sauvegardes de vos projets importants.

## Compilation et lancement

Nécessite Linux, un compilateur C++20, Ninja, pkg-config et les bibliothèques de développement suivantes :

- CMake **3.28+** pour les préréglages fournis.
- Qt **6.8+** : Core, Gui, Widgets, Network, DBus et PDF ; Qt Test pour les tests.
- Qt Linguist `lrelease` et Python 3 pour compiler les packs de langues (outils de compilation uniquement).
- Bibliothèque de développement qpdf **11+** (importation/exportation PDF ; pas l’outil en ligne de commande).
- Bibliothèque de développement compatible zlib (canaux ZIP des fichiers PSD).
- En-têtes et chargeur Vulkan **1.2+**, pilote Vulkan fonctionnel et `glslc`.
- minizip-ng **4.x** (le module pkg-config `minizip`, pas l’ancien minizip).
- libwebp et libwebpmux **1.2+**.

Installez les greffons de plateforme Qt adaptés à votre bureau, la prise en charge
SVG et les greffons de formats d’image. Faites pointer `CMAKE_PREFIX_PATH` vers
votre installation Qt si elle se trouve en dehors des chemins de recherche du système.
Qt PDF est un paquet de développement séparé dans de nombreuses distributions
(par exemple `qt6-qtpdf-devel` sur Fedora, `qt6-pdf-dev` sur Ubuntu). Il ne nécessite
pas de navigateur WebEngine lors de l’exécution de Vulkana.
Les outils Qt Linguist sont fournis par `qt6-linguist` sur Fedora ou
`qt6-tools-dev-tools` sur Ubuntu. Définissez `-DVULKANA_LRELEASE=/path/to/lrelease`
si nécessaire. Les catalogues des boîtes de dialogue standard de Qt proviennent
de `qt6-qttranslations` sur Fedora ou de `qt6-translations-l10n` sur Ubuntu.
Consultez la [documentation de localisation](../LOCALIZATION.md) pour compiler
uniquement les packs et les vérifier.

Depuis la racine du dépôt :

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

Les projets et les images peuvent aussi être passés en arguments :

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

Qt choisit Wayland ou X11 en fonction de la session. Les préférences et les
préréglages personnalisés sont stockés séparément pour chaque utilisateur.
Les services en ligne sont facultatifs ; sans
[configuration des services](../../config/SERVICES.md), les mises à jour et
l’envoi de commentaires restent désactivés.

Pour le développement et les tests :

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

Les vérifications de rendu en mode debug ou natif nécessitent les couches de
validation Vulkan. Certains tests d’interaction nécessitent un bureau Wayland/X11 actif.

## Architecture

- `src/core` — documents, calques, géométrie, algorithmes d’image et transactions
  annulables indépendants de la plateforme.
- `src/render` — canevas Vulkan partagé, composition, surimpressions et caches GPU de taille limitée.
- `src/ui` — panneaux Qt, outils, onglets de documents, préparation du texte/des
  formes et entrées/sorties de fichiers.
- `src/platform` — recherche des ressources, intégration au bureau et services facultatifs.
- `src/app` — point d’entrée de l’application ; `shaders` — sources des shaders Vulkan.

Chaque document possède son contenu et son historique ; les onglets partagent
un espace de travail et un moteur de rendu Vulkan. Les données sources modifiables
restent séparées des caches de rendu. Le canevas, l’aperçu des pixels et
l’exportation suivent les mêmes règles de composition.

Consultez la [documentation de développement](../README.md) et le
[guide d’architecture](../ARCHITECTURE.md).

## Licence

Le code, la documentation et les ressources appartenant à Vulkana sont
[sous licence MIT](../../LICENSE). Les éléments tiers conservent leurs propres
licences ; consultez les [mentions relatives aux tiers](../../THIRD_PARTY.md).
