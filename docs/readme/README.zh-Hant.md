# Vulkana 影像編輯器

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

一款優先以 Linux 為目標平台的影像編輯器，用於繪畫、修圖和圖層合成，
提供原生 Windows 建置，採用 Qt 工作區與 Vulkan 加速畫布。

![Vulkana 工作區，顯示含有圖層的影像、筆刷預設和調整選項](../../res/Preview.png)

## 功能

- **圖層編輯：** 點陣圖、可編輯文字與形狀；點陣圖遮色片、資料夾、剪裁遮色片群組、
  多選、混合模式、複製、合併和手動點陣化。
- **連結專案與鏡像圖層** 可以重複使用原有作品。按兩下[專案圖層](../PROJECT_LAYERS.md)即可編輯來源專案，
  回到上層專案就能看到更新。在同一份文件中，[鏡像圖層](../MIRRORED_LAYERS_AND_GUIDES.md)會隨來源圖層更新，
  也可以個別設定變形與效果。
- **色版群組：** 將封裝紋理的各個色版作為灰階圖層編輯，支援無損轉換、數值繪畫、
  修圖和調整。請參閱[色版群組指南](../CHANNEL_GROUPS.md)。
- **繪畫與修圖** 支援壓感筆刷、填色、局部模糊、仿製印章與修復工具。
  [筆刷對稱](../BRUSH_SYMMETRY.md)能沿著可移動的對稱軸鏡射筆畫，在圖層遮色片上也能使用。
  污點修復能填補透明孔洞，並可從修復圖層上方與下方的可見圖層取樣。
- **選取：** 矩形、橢圓、徒手/多邊形/磁性套索、依顏色選取和筆刷引導的快速選取，
  支援擴張/收縮、反向選取和重新選取。
- **物件選取與細修：** 使用隨附的離線模型，以粗略的矩形框選物件；透過選取範圍細修、
  即時預覽和手動修正改善選取範圍與點陣圖遮色片。使用物件選取無需下載模型或安裝 Python。
- **本機 AI 影像編輯** 支援用自己的模型與 LoRA 生成影像、編輯圖層或填滿選取範圍，還能比較不同結果後再套用。
  Qwen-Image-Layered 可將一張影像拆成多個可編輯的圖層，並收在同一個資料夾中。
  發行版內建 Vulkan 推論程序，使用 NVIDIA GPU 時可另行加入 CUDA 推論程序。
  設定方式與 GPU 需求請參閱 [Vulkana Diffusion](../../DIFFUSION.md)。
- **變形** 包含移動、縮放、旋轉、翻轉與角點扭曲，可套用至選取範圍的邊界或選取的像素。
  在畫布中央放置參考線，或以等距方式放置多條參考線，即可利用吸附功能定位圖層。
- **非破壞性調整：** 各圖層獨立校正，以及帶遮色片的調整圖層，作用範圍可選 All Below
  （下方所有圖層）或 This Group（目前群組）；包括曝光、色階、曲線、色相/飽和度、
  自然飽和度、色彩平衡、冷暖/色調、黑白轉換和負片效果。
- **濾鏡與圖層效果：** 高斯模糊、動態模糊和鏡頭模糊；筆畫、陰影/內陰影、外光暈/內光暈、
  顏色覆蓋、漸層覆蓋以及斜角和浮雕。
- **文件分頁** 各自保留歷程記錄、選取範圍與縮放比例。複製分頁後，即可在尚未儲存的副本中嘗試修改。
  也可以在文件間複製圖層，或將影像拖曳到圖層面板，直接插入圖層清單。
- **圖層清理** 提供可縮放的透明度預覽，方便檢查髮絲或柔和的邊緣。
  「清除反向選取範圍」會清除選取範圍外的像素，並保留原來的選取範圍。
  「裁切至畫布邊界」會移除所選圖層中位於畫布外的像素。
- **PSD 匯入** 可將內容轉換為原生圖層，保留格式化文字、簡單形狀、點陣圖遮色片、剪裁群組與支援的樣式。
  匯入時可查看圖層需要轉換的原因，並將相同的處理方式一次套用至所有符合條件的圖層。
- **PSD 匯出：** 輸出分層檔案或平面化的檔案；在支援的情況下保留可編輯文字、形狀、遮色片
  和剪裁關係，並可檢閱轉換情況，同時重新產生合成影像。
- **PDF 匯入：** 以指定的 PPI 將選定頁面匯入為點陣圖圖層或獨立文件，
  可使用透明或白色背景。
- **PDF 匯出：** 匯出合成影像，或將圖層/群組分別作為頁面匯出；支援自訂頁面選擇、
  畫布尺寸或配合內容的尺寸，並在支援的情況下保留可選取的文字。
- **工作區與輸出：** 具備分頁的可停駐面板、提供平移/縮放控制的導覽器、佈景主題、
  可編輯快捷鍵、尺規、像素預覽、原生 `.vulkana` 專案以及 PNG/JPEG/WebP 匯出。
  可只匯出選取的圖層，範圍取這些圖層所有可見內容的外接矩形，也可以調整畫布大小以符合選取的圖層。
- **語言：** 提供 16 種介面語言，可在「偏好設定 → 一般 → 語言」中切換，無需重新啟動。
  所有語言版本均為初稿，歡迎檢閱並提出修正。

目前處於積極開發中的測試階段。請為重要專案保留備份。

## 建置與執行

Linux 和 Windows 建置均以 x86-64 為目標。複製儲存庫後，在根目錄執行對應平台的指令：

```sh
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

### Linux

需要 Linux、C++20 編譯器、Ninja、pkg-config 和以下開發函式庫：

- CMake **3.28+**，用於隨附的預設。
- Qt **6.8+**：Core、Gui、Widgets、Network、DBus 和 PDF；測試還需要 Qt Test。
- Qt Linguist `lrelease` 和 Python 3，用於建置語言套件（僅在建置時使用）。
- qpdf **11+** 開發函式庫（用於 PDF 匯入/匯出；不是命令列工具）。
- 相容於 zlib 的開發函式庫（用於 PSD ZIP 色版）。
- Vulkan **1.2+** 標頭檔/載入器、可正常運作的 Vulkan 驅動程式以及 `glslc`。
- minizip-ng **4.x**（`minizip` pkg-config 模組，不是舊版 minizip）。
- libwebp 和 libwebpmux **1.2+**。

請安裝適用於桌面環境的 Qt 平台外掛、SVG 支援和影像格式外掛。
如果 Qt 安裝於系統搜尋路徑之外，請將 `CMAKE_PREFIX_PATH` 指向 Qt 安裝目錄。
許多發行版將 Qt PDF 作為獨立的開發套件提供，例如 Fedora 的 `qt6-qtpdf-devel`
和 Ubuntu 的 `qt6-pdf-dev`。Vulkana 執行時無需使用 WebEngine 瀏覽器。
Qt Linguist 工具由 Fedora 的 `qt6-linguist` 或 Ubuntu 的 `qt6-tools-dev-tools`
提供。如有需要，可設定 `-DVULKANA_LRELEASE=/path/to/lrelease`。
Qt 標準對話方塊的翻譯目錄由 Fedora 的 `qt6-qttranslations` 或 Ubuntu 的
`qt6-translations-l10n` 提供。僅建置語言套件和檢閱翻譯的說明，請參閱
[在地化文件](../LOCALIZATION.md)。

在儲存庫根目錄執行：

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

也可以透過引數傳入專案和影像：

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

Qt 依據工作階段選擇 Wayland 或 X11。偏好設定與自訂預設儲存在各使用者自己的儲存位置。
線上服務為選用功能；未完成[服務設定](../../config/SERVICES.md)時，更新與意見回饋功能會維持停用。

執行 Release 測試前，請啟用測試目標並重新建置：

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=ON
cmake --build --preset release --parallel 6
ctest --preset release
```

也可使用偵錯預設。原生算繪檢查需要 Vulkan 驗證層。部分互動測試需要正在執行的 Wayland/X11 桌面。

### Windows

安裝 [Windows 建置指南](../../tools/windows/README.md#prerequisites)中列出的必要元件。
在儲存庫根目錄開啟 Windows PowerShell，然後建置並啟動應用程式：

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Build.ps1
C:\Dev\Vulkana\stage\bin\imageeditor.exe
```

指令碼會準備相依套件，並建立包含語言套件的 Release 版本。
關於[自訂建置路徑](../../tools/windows/README.md#configuration)、
[測試](../../tools/windows/README.md#testing)和
[可攜式資料夾](../../tools/windows/README.md#portable-folder)的說明，請參閱指南。

## 架構

- `src/core` — 與平台無關的文件、圖層、幾何、影像演算法和可復原的交易操作。
- `src/render` — 共用的 Vulkan 畫布、合成、覆蓋層和有容量限制的 GPU 快取。
- `src/ui` — Qt 面板、工具、文件分頁、文字/形狀預處理和檔案 I/O。
- `src/platform` — 資源查找、桌面整合和選用服務。
- `src/app` — 應用程式進入點；`shaders` — Vulkan 著色器原始碼。

每份文件都有自己的內容與歷程記錄；所有分頁共用一個工作區和 Vulkan 算繪器。
可編輯的來源資料與算繪快取分開儲存。畫布、像素預覽和匯出使用相同的合成規則。

請參閱[開發者文件](../README.md)、[架構指南](../ARCHITECTURE.md)和 [MCP 設定與工具參考](../MCP.md)。

## 授權

Vulkana 自有的程式碼、文件和資源採用 [MIT 授權條款](../../LICENSE)。
第三方材料保留各自的授權條款；請參閱[第三方聲明](../../THIRD_PARTY.md)。
