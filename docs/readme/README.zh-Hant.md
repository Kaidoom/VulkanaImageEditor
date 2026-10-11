# Vulkana 影像編輯器

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

一款優先以 Linux 為目標平台的影像編輯器，用於繪畫、修圖和圖層合成，
採用 Qt 工作區與 Vulkan 加速畫布。

![Vulkana 工作區，顯示含有圖層的影像、筆刷預設和調整選項](../../res/Preview.png)

## 功能

- **圖層編輯：** 點陣圖、可編輯文字與形狀；點陣圖遮色片、資料夾、剪裁遮色片群組、
  多選、混合模式、複製、合併和手動點陣化。
- **繪畫與修圖：** 支援壓感的筆刷、程序生成與紋理預設、擦除、填色、局部模糊、
  仿製印章、修復和自動污點修復。
- **選取：** 矩形、橢圓、徒手/多邊形/磁性套索、依顏色選取和筆刷引導的快速選取，
  支援擴張/收縮、反向選取和重新選取。
- **物件選取與細修：** 使用隨附的離線模型，以粗略的矩形框選物件；透過選取範圍細修、
  即時預覽和手動修正改善選取範圍與點陣圖遮色片。使用物件選取無需下載模型或安裝 Python。
- **變形：** 移動、縮放、旋轉、翻轉和角點扭曲；可變形選取範圍邊界或選取的像素，
  並提供吸附和對齊參考線。
- **非破壞性調整：** 各圖層獨立校正，以及帶遮色片的調整圖層，作用範圍可選 All Below
  （下方所有圖層）或 This Group（目前群組）；包括曝光、色階、曲線、色相/飽和度、
  自然飽和度、色彩平衡、冷暖/色調、黑白轉換和負片效果。
- **濾鏡與圖層效果：** 高斯模糊、動態模糊和鏡頭模糊；筆畫、陰影/內陰影、外光暈/內光暈、
  顏色覆蓋、漸層覆蓋以及斜角和浮雕。
- **多份文件：** 每個分頁都有獨立的歷程記錄、選取範圍和檢視狀態，
  並支援在文件之間複製可編輯的圖層。
- **PSD 匯入：** 轉換為原生圖層、格式化文字、簡單形狀、點陣圖遮色片、剪裁群組和支援的樣式，
  並可檢閱轉換情況；需要改用已儲存的點陣圖時會明確說明。
- **PSD 匯出：** 輸出分層檔案或平面化的檔案；在支援的情況下保留可編輯文字、形狀、遮色片
  和剪裁關係，並可檢閱轉換情況，同時重新產生合成影像。
- **PDF 匯入：** 以指定的 PPI 將選定頁面匯入為點陣圖圖層或獨立文件，
  可使用透明或白色背景。
- **PDF 匯出：** 匯出合成影像，或將圖層/群組分別作為頁面匯出；支援自訂頁面選擇、
  畫布尺寸或配合內容的尺寸，並在支援的情況下保留可選取的文字。
- **工作區與輸出：** 具備分頁的可停駐面板、提供平移/縮放控制的導覽器、佈景主題、
  可編輯快捷鍵、尺規、像素預覽、原生 `.vulkana` 專案以及 PNG/JPEG/WebP 匯出。
- **語言：** 提供 16 種介面語言，可在「偏好設定 → 一般 → 語言」中切換，無需重新啟動。
  新增翻譯為初稿，歡迎提出修正。

目前處於積極開發中的測試階段。請為重要專案保留備份。

## 建置與執行

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

用於開發與測試：

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

偵錯/原生算繪檢查需要 Vulkan 驗證層。部分互動測試需要正在執行的 Wayland/X11 桌面。

## 架構

- `src/core` — 與平台無關的文件、圖層、幾何、影像演算法和可復原的交易操作。
- `src/render` — 共用的 Vulkan 畫布、合成、覆蓋層和有容量限制的 GPU 快取。
- `src/ui` — Qt 面板、工具、文件分頁、文字/形狀預處理和檔案 I/O。
- `src/platform` — 資源查找、桌面整合和選用服務。
- `src/app` — 應用程式進入點；`shaders` — Vulkan 著色器原始碼。

每份文件都有自己的內容與歷程記錄；所有分頁共用一個工作區和 Vulkan 算繪器。
可編輯的來源資料與算繪快取分開儲存。畫布、像素預覽和匯出使用相同的合成規則。

請參閱[開發者文件](../README.md)和[架構指南](../ARCHITECTURE.md)。

## 授權

Vulkana 自有的程式碼、文件和資源採用 [MIT 授權條款](../../LICENSE)。
第三方材料保留各自的授權條款；請參閱[第三方聲明](../../THIRD_PARTY.md)。
