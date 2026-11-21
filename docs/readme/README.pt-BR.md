# Vulkana — Editor de imagens

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

Um editor de imagens desenvolvido principalmente para Linux, para pintura,
retoque e composição em camadas, também disponível em versões nativas para Windows,
com uma área de trabalho Qt e uma tela acelerada por Vulkan.

![Área de trabalho do Vulkana com uma imagem em camadas, predefinições de pincel e ajustes](../../res/Preview.png)

## Recursos

- **Edição em camadas:** camadas raster, texto e formas editáveis; máscaras bitmap,
  pastas, grupos de máscaras de corte, seleção múltipla, modos de mesclagem,
  duplicação, mesclagem e rasterização explícita.
- **Projetos vinculados e camadas espelhadas** permitem reutilizar um desenho.
  Clique duas vezes em uma [camada de projeto](../PROJECT_LAYERS.md) para editar
  a origem e volte ao projeto principal para ver as alterações. Uma
  [camada espelhada](../MIRRORED_LAYERS_AND_GUIDES.md) acompanha as mudanças na
  camada de origem dentro do documento e pode ter transformações e efeitos próprios.
- **Grupos de canais:** edite canais individuais como camadas em tons de cinza
  para texturas com dados empacotados, com conversão sem perdas, pintura numérica,
  retoque e ajustes. Consulte o [guia de grupos de canais](../CHANNEL_GROUPS.md).
- **As ferramentas de pintura e retoque** incluem pincéis sensíveis à pressão,
  preenchimento, desfoque local, carimbo de clonagem e ferramentas de correção.
  A [simetria do pincel](../BRUSH_SYMMETRY.md) espelha as pinceladas em torno
  de um eixo móvel, inclusive em máscaras de camada. A correção pontual pode
  preencher lacunas transparentes e coletar amostras das camadas visíveis acima
  e abaixo da camada de retoque.
- **Seleções:** retângulo, elipse, laço à mão livre/poligonal/magnético, seleção
  por cor e seleção rápida guiada por pincel, com expansão, contração, inversão
  e recuperação da seleção.
- **Seleção de objetos e refinamento:** selecione um objeto com um retângulo
  aproximado usando o modelo offline incluído; melhore seleções e máscaras bitmap
  com Refinar seleção, prévias em tempo real e correções manuais. A seleção de
  objetos não exige baixar um modelo nem instalar Python.
- **A edição local com IA** permite gerar imagens, editar camadas e preencher
  seleções com seus próprios modelos e LoRAs. Compare os resultados antes de
  aplicá-los. O Qwen-Image-Layered pode decompor uma imagem em uma pasta de
  camadas editáveis. As versões distribuídas incluem o Vulkan. Para GPUs NVIDIA,
  há um processo de inferência CUDA disponível à parte. Consulte
  [Vulkana Diffusion](../../DIFFUSION.md) para ver as instruções de configuração
  e os requisitos de GPU.
- **As transformações** permitem mover, redimensionar, girar, espelhar e distorcer
  pelos cantos, inclusive os limites da seleção ou os pixels selecionados.
  Posicione uma guia no centro ou várias em intervalos regulares e use o encaixe
  para alinhar as camadas.
- **Ajustes não destrutivos:** correções por camada e camadas de ajuste com máscara,
  aplicadas a todas as camadas abaixo ou ao grupo atual; exposição, níveis, curvas,
  matiz/saturação, vibração, equilíbrio de cores, calor/matiz, conversão
  monocromática e inversão.
- **Filtros e efeitos de camada:** desfoque gaussiano, de movimento e de lente;
  traçado, sombra projetada/interna, brilho externo/interno, sobreposição de cor,
  sobreposição de degradê e chanfro e entalhe.
- **As abas de documentos** mantêm seu próprio histórico, suas seleções e seu
  nível de zoom. Duplique uma aba para testar alterações em uma cópia ainda não
  salva, copie camadas entre documentos ou arraste imagens para o painel Camadas
  para inseri-las na pilha.
- **A limpeza de camadas** inclui uma prévia de transparência que você pode
  ampliar para examinar fios de cabelo e bordas suaves. “Apagar seleção inversa”
  apaga os pixels fora da seleção e a mantém ativa. “Aparar aos limites da tela”
  remove das camadas selecionadas os pixels que ficam fora da tela.
- **A importação de PSD** permite revisar a conversão em camadas nativas, texto
  formatado, formas simples, máscaras bitmap, grupos de corte e estilos
  compatíveis. Veja por que uma conversão é necessária e aplique a mesma opção a
  todas as camadas do mesmo tipo de uma só vez.
- **Exportação PSD:** saída em camadas ou achatada, texto/formas/máscaras editáveis
  e recorte quando compatível, com revisão das conversões e uma imagem composta
  gerada novamente.
- **Importação PDF:** páginas selecionadas no PPI desejado, como camadas raster
  ou documentos separados, com fundo transparente ou branco.
- **Exportação PDF:** uma imagem composta ou camadas/grupos como páginas, seleção
  personalizada de páginas, dimensões da tela ou ajustadas ao conteúdo e texto
  selecionável quando compatível.
- **Área de trabalho e saída:** painéis acopláveis com abas, um navegador com
  controles de deslocamento e zoom, temas, atalhos editáveis, réguas, prévia de
  pixels, projetos nativos `.vulkana` e exportação PNG/JPEG/WebP.
  Exporte as camadas selecionadas com um recorte que abrange todo o conteúdo
  visível dessas camadas ou ajuste o tamanho da tela à camada selecionada.
- **Idiomas:** 16 idiomas de interface, selecionáveis em Preferências → Geral →
  Idioma sem reiniciar. Todas as versões de idioma são rascunhos iniciais;
  revisões e correções de qualquer pessoa interessada são bem-vindas.

Versão beta em desenvolvimento ativo. Mantenha cópias de segurança de projetos importantes.

## Compilar e executar

As compilações para Linux e Windows são destinadas a x86-64. Clone o repositório
e execute os comandos da sua plataforma a partir da raiz dele:

```sh
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

### Linux

Requer Linux, um compilador C++20, Ninja, pkg-config e estas bibliotecas de desenvolvimento:

- CMake **3.28+** para as predefinições fornecidas.
- Qt **6.8+**: Core, Gui, Widgets, Network, DBus e PDF; Qt Test para os testes.
- Qt Linguist `lrelease` e Python 3 para compilar pacotes de idiomas (apenas ferramentas de compilação).
- Biblioteca de desenvolvimento qpdf **11+** (importação/exportação PDF; não a ferramenta de linha de comando).
- Biblioteca de desenvolvimento compatível com zlib (canais ZIP de PSD).
- Cabeçalhos e carregador Vulkan **1.2+**, um driver Vulkan funcional e `glslc`.
- minizip-ng **4.x** (o módulo pkg-config `minizip`, não o minizip antigo).
- libwebp e libwebpmux **1.2+**.

Instale os plugins de plataforma do Qt para o seu ambiente gráfico, o suporte
a SVG e os plugins de formatos de imagem. Aponte `CMAKE_PREFIX_PATH` para sua
instalação do Qt se ela estiver fora dos caminhos de busca do sistema.
O Qt PDF é um pacote de desenvolvimento separado em muitas distribuições (por
exemplo, `qt6-qtpdf-devel` no Fedora e `qt6-pdf-dev` no Ubuntu). Ele não exige
um navegador WebEngine durante a execução do Vulkana.
As ferramentas do Qt Linguist são fornecidas por `qt6-linguist` no Fedora ou
`qt6-tools-dev-tools` no Ubuntu. Defina `-DVULKANA_LRELEASE=/path/to/lrelease`
se necessário. Os catálogos das caixas de diálogo padrão do Qt vêm de
`qt6-qttranslations` no Fedora ou `qt6-translations-l10n` no Ubuntu. Consulte a
[documentação de localização](../LOCALIZATION.md) para compilar somente os pacotes
de idiomas e revisá-los.

Na raiz do repositório:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

Projetos e imagens também podem ser passados como argumentos:

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

O Qt escolhe Wayland ou X11 de acordo com a sessão. As preferências e predefinições
personalizadas ficam no armazenamento de cada usuário. Os serviços online são
opcionais; sem a [configuração dos serviços](../../config/SERVICES.md), as
atualizações e o envio de comentários permanecem desativados.

Para os testes Release, ative os alvos de teste e recompile:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=ON
cmake --build --preset release --parallel 6
ctest --preset release
```

O preset debug também está disponível. As verificações de renderização nativa
precisam das camadas de validação do Vulkan. Alguns testes de interação exigem
um ambiente gráfico Wayland/X11 em execução.

### Windows

Instale os pré-requisitos indicados no [guia de compilação para Windows](../../tools/windows/README.md#prerequisites).
Abra o Windows PowerShell na raiz do repositório, compile e inicie o aplicativo:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Build.ps1
C:\Dev\Vulkana\stage\bin\imageeditor.exe
```

O script prepara as dependências e gera uma compilação Release com os pacotes de idiomas.
Consulte o guia para [personalizar os caminhos de compilação](../../tools/windows/README.md#configuration),
[executar testes](../../tools/windows/README.md#testing) e
[criar pastas portáteis](../../tools/windows/README.md#portable-folder).

## Arquitetura

- `src/core` — documentos, camadas, geometria, algoritmos de imagem e transações
  que podem ser desfeitas, independentes de plataforma.
- `src/render` — tela Vulkan compartilhada, composição, sobreposições e caches
  de GPU com tamanho limitado.
- `src/ui` — painéis Qt, ferramentas, abas de documentos, preparação de texto
  e formas e entrada/saída de arquivos.
- `src/platform` — busca de recursos, integração com o ambiente gráfico e serviços opcionais.
- `src/app` — ponto de entrada do aplicativo; `shaders` — código-fonte dos shaders Vulkan.

Cada documento possui seu próprio conteúdo e histórico; as abas compartilham
uma área de trabalho e um renderizador Vulkan. Os dados de origem editáveis
permanecem separados dos caches de renderização. A tela, a prévia de pixels e
a exportação compartilham as mesmas regras de composição.

Consulte a [documentação para desenvolvedores](../README.md), o [guia de arquitetura](../ARCHITECTURE.md)
e o [guia de configuração e ferramentas do MCP](../MCP.md).

## Licença

O código, a documentação e os recursos de propriedade do Vulkana estão
[sob a licença MIT](../../LICENSE). O material de terceiros mantém suas próprias
licenças; consulte os [avisos de terceiros](../../THIRD_PARTY.md).
