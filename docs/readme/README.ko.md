# Vulkana 이미지 편집기

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

Linux를 우선 대상으로 하는 페인팅, 사진 보정, 레이어 합성용 이미지 편집기입니다.
네이티브 Windows 빌드를 지원하며 Qt 작업 공간과 Vulkan으로 가속하는 캔버스를 제공합니다.

![레이어가 있는 이미지, 브러시 사전 설정, 조정 항목이 표시된 Vulkana 작업 공간](../../res/Preview.png)

## 기능

- **레이어 편집:** 래스터, 편집 가능한 텍스트와 도형, 비트맵 마스크, 폴더,
  클리핑 마스크 그룹, 다중 선택, 혼합 모드, 복제, 병합, 명시적인 래스터화.
- **프로젝트 연결과 미러 레이어** 기능을 활용하면 같은 그림을 여러 곳에서 사용할 수 있습니다.
  [프로젝트 레이어](../PROJECT_LAYERS.md)를 두 번 클릭해 원본 프로젝트를 편집하고 상위 프로젝트로 돌아오면 변경 사항이 반영됩니다.
  [미러 레이어](../MIRRORED_LAYERS_AND_GUIDES.md)는 같은 문서 안의 원본 레이어를 따라 갱신되며, 변형과 효과는 따로 설정할 수 있습니다.
- **채널 그룹:** 패킹된 텍스처의 각 채널을 회색조 레이어로 편집합니다. 무손실 변환,
  수치 페인팅, 리터칭 및 조정을 지원합니다. [채널 그룹 가이드](../CHANNEL_GROUPS.md)를 참고하세요.
- **페인팅과 보정** 작업에는 필압 감지 브러시, 채우기, 부분 흐림, 복제 도장, 복구 도구를 쓸 수 있습니다.
  [브러시 대칭](../BRUSH_SYMMETRY.md)으로 원하는 위치에 대칭축을 놓고 획을 대칭으로 그릴 수 있습니다. 레이어 마스크에서도 사용할 수 있습니다.
  스팟 복구는 투명한 구멍을 메울 수 있으며, 복구 레이어 위아래에 있는 표시 중인 레이어에서도 샘플을 가져올 수 있습니다.
- **선택 영역:** 사각형, 타원, 자유형/다각형/자석 올가미, 색상별 선택, 브러시로 지정하는
  빠른 선택. 선택 영역 확대/축소, 반전, 다시 선택을 지원합니다.
- **개체 선택과 다듬기:** 함께 제공되는 오프라인 모델로 대략적인 사각형을 그려 개체를
  선택합니다. 선택 영역 다듬기, 실시간 미리 보기, 수동 수정으로 선택 영역과 비트맵 마스크를
  개선할 수 있습니다. 개체 선택을 사용하기 위해 모델을 다운로드하거나 Python을 설치할 필요가 없습니다.
- **로컬 AI 이미지 편집** 기능은 내 모델과 LoRA로 이미지 생성, 레이어 편집, 선택 영역 채우기를 지원합니다.
  여러 결과를 비교한 뒤 적용할 수 있습니다. Qwen-Image-Layered는 이미지 한 장을 편집 가능한 여러 레이어로 나눠 한 폴더에 담습니다.
  배포판에는 Vulkan 워커가 들어 있으며 NVIDIA GPU 사용자는 CUDA 워커를 별도로 추가할 수 있습니다.
  설정 방법과 GPU 요구 사항은 [Vulkana Diffusion](../../DIFFUSION.md)을 참고하세요.
- **변형** 기능으로 이동, 크기 조절, 회전, 뒤집기, 모서리 왜곡을 할 수 있습니다. 선택 영역의 경계나 선택한 픽셀도 변형할 수 있습니다.
  중앙에 안내선을 놓거나 여러 안내선을 일정한 간격으로 배치한 뒤, 스냅으로 레이어 위치를 맞출 수 있습니다.
- **비파괴 조정:** 레이어별 보정과 마스크가 있는 조정 레이어. 적용 범위는 All Below
  (아래 모든 레이어) / This Group(이 그룹) 중에서 선택합니다. 노출, 레벨, 곡선,
  색조/채도, 생동감, 색상 균형, 따뜻함/색조, 흑백 변환, 색상 반전을 지원합니다.
- **필터와 레이어 효과:** 가우시안 흐림, 동작 흐림, 렌즈 흐림, 획, 그림자/내부 그림자,
  외부 광선/내부 광선, 색상 오버레이, 그레이디언트 오버레이, 경사와 엠보스.
- **문서 탭** 사이를 전환해도 각 탭의 작업 기록, 선택 영역, 확대·축소 배율이 유지됩니다. 탭을 복제하면 아직 저장하지 않은 사본에서 수정을 시험할 수 있습니다.
  문서 간에 레이어를 복사하거나 이미지를 레이어 패널로 끌어다 놓아 목록에 추가할 수도 있습니다.
- **레이어 정리** 기능에서는 투명도 미리보기를 확대해 머리카락이나 부드러운 가장자리를 확인할 수 있습니다.
  ‘반전된 선택 영역 지우기’는 현재 선택을 유지하면서 영역 밖을 지웁니다.
  ‘캔버스 경계에 맞춰 자르기’는 선택한 레이어에서 캔버스 밖의 픽셀을 제거합니다.
- **PSD 가져오기** 기능은 서식 있는 텍스트, 단순 도형, 비트맵 마스크, 클리핑 그룹, 지원되는 스타일을 Vulkana의 기본 레이어로 변환합니다.
  가져올 때 레이어를 변환해야 하는 이유를 확인하고, 같은 조건에 해당하는 레이어에 동일한 변환 방식을 한꺼번에 적용할 수 있습니다.
- **PSD 내보내기:** 레이어를 유지하거나 하나로 합친 형태로 출력합니다. 지원되는 범위에서
  편집 가능한 텍스트/도형/마스크와 클리핑을 유지하고, 변환 내용을 검토할 수 있으며,
  합성 이미지를 새로 생성합니다.
- **PDF 가져오기:** 선택한 페이지를 원하는 PPI로 래스터 레이어 또는 별도 문서로 가져옵니다.
  배경은 투명 또는 흰색으로 설정할 수 있습니다.
- **PDF 내보내기:** 합성 이미지를 내보내거나 레이어/그룹을 각각 페이지로 내보냅니다.
  페이지 직접 선택, 캔버스 크기/내용에 맞춘 크기, 지원되는 경우 선택 가능한 텍스트를 제공합니다.
- **작업 공간과 출력:** 탭이 있는 도킹 가능한 패널, 이동/확대·축소 제어가 있는 탐색기,
  테마, 편집 가능한 단축키, 눈금자, 픽셀 미리 보기, 기본 `.vulkana` 프로젝트,
  PNG/JPEG/WebP 내보내기.
  선택한 레이어의 보이는 부분을 모두 포함하는 최소 영역만 내보내거나, 선택한 레이어에 맞게 캔버스 크기를 조절할 수 있습니다.
- **언어:** 인터페이스 언어 16개를 제공합니다. 환경 설정 → 일반 → 언어에서 다시 시작하지
  않고 변경할 수 있습니다. 모든 언어 버전은 초안이며, 검토와 수정 제안을 환영합니다.

활발히 개발 중인 베타 버전입니다. 중요한 프로젝트는 백업해 두세요.

## 빌드 및 실행

Linux와 Windows 빌드는 x86-64를 대상으로 합니다. 저장소를 복제한 뒤,
루트 디렉터리에서 해당 플랫폼의 명령을 실행하세요.

```sh
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

### Linux

Linux, C++20 컴파일러, Ninja, pkg-config와 다음 개발 라이브러리가 필요합니다.

- 제공되는 사전 설정을 위한 CMake **3.28+**.
- Qt **6.8+**: Core, Gui, Widgets, Network, DBus, PDF. 테스트에는 Qt Test도 필요합니다.
- 언어 팩 빌드를 위한 Qt Linguist `lrelease`와 Python 3(빌드 도구로만 사용).
- qpdf **11+** 개발 라이브러리(PDF 가져오기/내보내기용이며, 명령줄 도구가 아닙니다).
- zlib 호환 개발 라이브러리(PSD ZIP 채널용).
- Vulkan **1.2+** 헤더/로더, 정상적으로 작동하는 Vulkan 드라이버, `glslc`.
- minizip-ng **4.x**(`minizip` pkg-config 모듈이며, 기존 minizip이 아닙니다).
- libwebp와 libwebpmux **1.2+**.

데스크톱 환경에 맞는 Qt 플랫폼 플러그인, SVG 지원, 이미지 형식 플러그인을 설치하세요.
Qt가 시스템 검색 경로 밖에 설치되어 있다면 `CMAKE_PREFIX_PATH`에 Qt 설치 경로를 지정하세요.
많은 배포판에서 Qt PDF는 별도의 개발 패키지로 제공됩니다. 예를 들어 Fedora에서는
`qt6-qtpdf-devel`, Ubuntu에서는 `qt6-pdf-dev`입니다. Vulkana 실행 시 WebEngine 브라우저는 필요하지 않습니다.
Qt Linguist 도구는 Fedora의 `qt6-linguist` 또는 Ubuntu의 `qt6-tools-dev-tools`에서 제공합니다.
필요하면 `-DVULKANA_LRELEASE=/path/to/lrelease`를 설정하세요.
Qt 표준 대화 상자 번역 카탈로그는 Fedora의 `qt6-qttranslations` 또는 Ubuntu의
`qt6-translations-l10n`에서 제공합니다. 언어 팩만 빌드하거나 번역을 검토하는 방법은
[현지화 문서](../LOCALIZATION.md)를 참조하세요.

저장소 루트에서 실행하세요.

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

프로젝트와 이미지를 인수로 전달할 수도 있습니다.

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

Qt는 세션에 따라 Wayland 또는 X11을 선택합니다. 환경 설정과 사용자 지정 사전 설정은
사용자별 저장 공간에 보관됩니다. 온라인 서비스는 선택 사항입니다.
[서비스 설정](../../config/SERVICES.md)이 없으면 업데이트와 피드백 기능은 비활성 상태로 유지됩니다.

Release 테스트를 실행하려면 테스트 대상을 활성화하고 다시 빌드하세요.

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=ON
cmake --build --preset release --parallel 6
ctest --preset release
```

디버그 사전 설정도 사용할 수 있습니다. 네이티브 렌더링 검사에는 Vulkan 검증 레이어가 필요합니다. 일부 상호 작용 테스트에는
실행 중인 Wayland/X11 데스크톱이 필요합니다.

### Windows

[Windows 빌드 가이드](../../tools/windows/README.md#prerequisites)에 안내된 필수 도구를 설치하세요.
저장소 루트에서 Windows PowerShell을 열고 빌드한 뒤 실행하세요:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Build.ps1
C:\Dev\Vulkana\stage\bin\imageeditor.exe
```

스크립트가 의존성을 준비하고 언어 팩이 포함된 Release 빌드를 생성합니다.
[빌드 경로 설정](../../tools/windows/README.md#configuration),
[테스트](../../tools/windows/README.md#testing),
[포터블 폴더](../../tools/windows/README.md#portable-folder)에 대한 자세한 내용은 가이드를 참조하세요.

## 아키텍처

- `src/core` — 플랫폼에 독립적인 문서, 레이어, 기하 처리, 이미지 알고리즘, 실행 취소 가능한 트랜잭션.
- `src/render` — 공유 Vulkan 캔버스, 합성, 오버레이, 용량이 제한된 GPU 캐시.
- `src/ui` — Qt 패널, 도구, 문서 탭, 텍스트/도형 준비 처리, 파일 I/O.
- `src/platform` — 리소스 검색, 데스크톱 통합, 선택적 서비스.
- `src/app` — 애플리케이션 진입점. `shaders` — Vulkan 셰이더 소스.

각 문서는 자신의 내용과 작업 기록을 소유하며, 탭들은 하나의 작업 공간과 Vulkan 렌더러를
공유합니다. 편집 가능한 원본 데이터는 렌더링 캐시와 분리되어 있습니다.
캔버스, 픽셀 미리 보기, 내보내기는 같은 합성 규칙을 사용합니다.

[개발자 문서](../README.md), [아키텍처 가이드](../ARCHITECTURE.md), [MCP 설정 및 도구 안내](../MCP.md)를 참조하세요.

## 라이선스

Vulkana가 소유한 코드, 문서, 자산에는 [MIT 라이선스](../../LICENSE)가 적용됩니다.
제3자 자료에는 각각의 라이선스가 계속 적용됩니다. [제3자 고지](../../THIRD_PARTY.md)를 참조하세요.
