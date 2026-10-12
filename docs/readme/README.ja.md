# Vulkana 画像エディター

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

Linux を主な対象とした、ペイント、レタッチ、レイヤー合成のための画像エディターです。
Windows ネイティブビルドにも対応し、Qt のワークスペースと Vulkan で高速化したキャンバスを備えています。

![レイヤーを持つ画像、ブラシプリセット、調整項目を表示した Vulkana のワークスペース](../../res/Preview.png)

## 機能

- **レイヤー編集：** ラスター、編集可能なテキストと図形、ビットマップマスク、フォルダー、
  クリッピングマスクグループ、複数選択、描画モード、複製、結合、明示的なラスタライズ。
- **ペイントとレタッチ：** 筆圧対応ブラシ、手続き型およびテクスチャ付きプリセット、
  消しゴム、塗りつぶし、部分的なぼかし、コピースタンプ、修復、自動スポット修復。
- **選択範囲：** 長方形、楕円、フリーハンド/多角形/マグネットなげなわ、色による選択、
  ブラシで指定するクイック選択。拡張/縮小、反転、再選択にも対応。
- **オブジェクト選択と境界の調整：** 同梱のオフラインモデルを使い、大まかな長方形で囲んで
  オブジェクトを選択できます。選択範囲の調整、ライブプレビュー、手動修正で選択範囲や
  ビットマップマスクを改善できます。オブジェクト選択の利用にモデルのダウンロードや
  Python のインストールは不要です。
- **変形：** 移動、拡大縮小、回転、反転、角を動かす変形。選択範囲の境界または選択した
  ピクセルを変形でき、スナップと整列ガイドも利用できます。
- **非破壊調整：** レイヤーごとの補正と、マスク付き調整レイヤー。適用範囲は All Below
  （下のすべてのレイヤー）/ This Group（このグループ）から選べます。露出、レベル補正、
  トーンカーブ、色相/彩度、自然な彩度、カラーバランス、暖かさ/色かぶり、モノクロ化、階調反転に対応。
- **フィルターとレイヤー効果：** ガウスぼかし、移動ぼかし、レンズぼかし、境界線、
  ドロップシャドウ/シャドウ（内側）、光彩（外側/内側）、カラーオーバーレイ、
  グラデーションオーバーレイ、ベベルとエンボス。
- **複数ドキュメント：** 各タブに独立した履歴、選択範囲、表示状態があり、
  編集可能なレイヤーをドキュメント間でコピーできます。
- **PSD 読み込み：** 変換内容を確認しながら、ネイティブのレイヤー、リッチテキスト、
  単純な図形、ビットマップマスク、クリッピンググループ、対応するスタイルに変換します。
  保存済みのラスター画像に置き換える場合は、そのことが明示されます。
- **PSD 書き出し：** レイヤーを保持した出力と統合した出力に対応。対応する範囲でテキスト、
  図形、マスクの編集可能な状態とクリッピングを保持し、変換内容を確認できます。
  合成画像も新しく生成します。
- **PDF 読み込み：** 選んだページを指定した PPI で、ラスターレイヤーまたは個別の
  ドキュメントとして読み込めます。背景は透明または白を選べます。
- **PDF 書き出し：** 合成画像を書き出すか、レイヤー/グループをそれぞれページとして
  書き出せます。ページの個別指定、キャンバスサイズ/内容に合わせたサイズに対応し、
  対応する範囲で選択可能なテキストを保持します。
- **ワークスペースと出力：** タブ付きのドッキング可能なパネル、パン/ズーム操作付き
  ナビゲーター、テーマ、編集可能なショートカット、定規、ピクセルプレビュー、
  ネイティブの `.vulkana` プロジェクト、PNG/JPEG/WebP 書き出し。
- **言語：** 16 言語のインターフェースを用意しています。環境設定 → 一般 → 言語で、
  再起動せずに切り替えられます。すべての言語版は初稿です。レビューや修正の提案を歓迎します。

現在も開発を進めているベータ版です。重要なプロジェクトはバックアップを保存してください。

## ビルドと実行

Linux と Windows のビルドは x86-64 を対象としています。リポジトリをクローンし、
ルートディレクトリから各プラットフォームのコマンドを実行してください。

```sh
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

### Linux

Linux、C++20 コンパイラー、Ninja、pkg-config、および次の開発ライブラリが必要です。

- CMake **3.28+**：付属のプリセットに必要です。
- Qt **6.8+**：Core、Gui、Widgets、Network、DBus、PDF。テストには Qt Test も必要です。
- Qt Linguist の `lrelease` と Python 3：言語パックのビルドに使用します（ビルド時のみ必要）。
- qpdf **11+** 開発ライブラリ：PDF の読み込み/書き出し用です。コマンドラインツールではありません。
- zlib 互換の開発ライブラリ：PSD の ZIP チャンネル用です。
- Vulkan **1.2+** ヘッダー/ローダー、正常に動作する Vulkan ドライバー、および `glslc`。
- minizip-ng **4.x**：`minizip` pkg-config モジュールです。旧来の minizip ではありません。
- libwebp と libwebpmux **1.2+**。

デスクトップ環境に合った Qt プラットフォームプラグイン、SVG サポート、画像形式プラグインを
インストールしてください。Qt がシステムの検索パス外にある場合は、`CMAKE_PREFIX_PATH` に
Qt のインストール先を指定してください。
多くのディストリビューションでは Qt PDF は独立した開発パッケージです。たとえば Fedora では
`qt6-qtpdf-devel`、Ubuntu では `qt6-pdf-dev` です。Vulkana の実行時に WebEngine ブラウザーは不要です。
Qt Linguist ツールは Fedora の `qt6-linguist` または Ubuntu の `qt6-tools-dev-tools` に含まれます。
必要に応じて `-DVULKANA_LRELEASE=/path/to/lrelease` を設定してください。
Qt の標準ダイアログ用翻訳カタログは Fedora の `qt6-qttranslations` または Ubuntu の
`qt6-translations-l10n` に含まれます。言語パックのみのビルドと翻訳の確認については、
[ローカライズのドキュメント](../LOCALIZATION.md)を参照してください。

リポジトリのルートディレクトリから実行します。

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

プロジェクトや画像を引数として渡すこともできます。

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

Qt はセッションに応じて Wayland または X11 を選択します。環境設定とカスタムプリセットは
ユーザーごとの保存領域に格納されます。オンラインサービスは任意です。
[サービス設定](../../config/SERVICES.md)がない場合、更新とフィードバック機能は無効のままです。

Release テストを実行するには、テストターゲットを有効にして再ビルドします。

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=ON
cmake --build --preset release --parallel 6
ctest --preset release
```

デバッグプリセットも利用できます。ネイティブ描画のチェックには Vulkan 検証レイヤーが必要です。一部の操作テストには、
実行中の Wayland/X11 デスクトップが必要です。

### Windows

[Windows ビルドガイド](../../tools/windows/README.md#prerequisites)に記載された必要なツールをインストールします。
リポジトリのルートで Windows PowerShell を開き、ビルドして起動します：

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Build.ps1
C:\Dev\Vulkana\stage\bin\imageeditor.exe
```

スクリプトは依存関係を準備し、言語パックを含む Release ビルドを作成します。
[ビルドパスの変更](../../tools/windows/README.md#configuration)、
[テスト](../../tools/windows/README.md#testing)、
[ポータブルフォルダーの作成](../../tools/windows/README.md#portable-folder)については、ガイドを参照してください。

## アーキテクチャ

- `src/core` — プラットフォームに依存しないドキュメント、レイヤー、幾何処理、
  画像アルゴリズム、取り消し可能なトランザクション。
- `src/render` — 共有の Vulkan キャンバス、合成、オーバーレイ、容量に上限のある GPU キャッシュ。
- `src/ui` — Qt パネル、ツール、ドキュメントタブ、テキスト/図形の準備処理、ファイル I/O。
- `src/platform` — リソース検索、デスクトップ統合、任意のサービス。
- `src/app` — アプリケーションのエントリーポイント。`shaders` — Vulkan シェーダーのソース。

各ドキュメントはそれぞれの内容と履歴を保持します。タブは一つのワークスペースと Vulkan
レンダラーを共有します。編集可能なソースデータは描画キャッシュから分離されています。
キャンバス、ピクセルプレビュー、書き出しは同じ合成規則を使用します。

[開発者向けドキュメント](../README.md)と[アーキテクチャガイド](../ARCHITECTURE.md)を参照してください。

## ライセンス

Vulkana が所有するコード、ドキュメント、アセットには [MIT ライセンス](../../LICENSE)が適用されます。
第三者の素材にはそれぞれのライセンスが引き続き適用されます。[第三者に関する通知](../../THIRD_PARTY.md)を参照してください。
