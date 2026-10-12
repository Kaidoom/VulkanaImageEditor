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
- **페인팅과 보정:** 필압 감지 브러시, 절차적으로 생성된 사전 설정과 텍스처 사전 설정,
  지우개, 채우기, 부분 흐림, 복제 도장, 복구, 자동 스팟 복구.
- **선택 영역:** 사각형, 타원, 자유형/다각형/자석 올가미, 색상별 선택, 브러시로 지정하는
  빠른 선택. 선택 영역 확대/축소, 반전, 다시 선택을 지원합니다.
- **개체 선택과 다듬기:** 함께 제공되는 오프라인 모델로 대략적인 사각형을 그려 개체를
  선택합니다. 선택 영역 다듬기, 실시간 미리 보기, 수동 수정으로 선택 영역과 비트맵 마스크를
  개선할 수 있습니다. 개체 선택을 사용하기 위해 모델을 다운로드하거나 Python을 설치할 필요가 없습니다.
- **변형:** 이동, 크기 조절, 회전, 뒤집기, 모서리 왜곡. 선택 영역의 경계 또는 선택한 픽셀을
  변형할 수 있으며 스냅과 정렬 안내선을 지원합니다.
- **비파괴 조정:** 레이어별 보정과 마스크가 있는 조정 레이어. 적용 범위는 All Below
  (아래 모든 레이어) / This Group(이 그룹) 중에서 선택합니다. 노출, 레벨, 곡선,
  색조/채도, 생동감, 색상 균형, 따뜻함/색조, 흑백 변환, 색상 반전을 지원합니다.
- **필터와 레이어 효과:** 가우시안 흐림, 동작 흐림, 렌즈 흐림, 획, 그림자/내부 그림자,
  외부 광선/내부 광선, 색상 오버레이, 그레이디언트 오버레이, 경사와 엠보스.
- **여러 문서:** 탭마다 독립적인 작업 기록, 선택 영역, 보기 상태를 유지하며,
  문서 사이에서 편집 가능한 레이어를 복사할 수 있습니다.
- **PSD 가져오기:** 변환 내용을 검토하면서 기본 레이어, 서식 있는 텍스트, 단순 도형,
  비트맵 마스크, 클리핑 그룹, 지원되는 스타일로 변환합니다. 저장된 래스터 이미지로
  대체하는 경우에는 이를 명확하게 표시합니다.
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

PowerShell 7, Git, CMake, Ninja, Python 3, Visual Studio의 x64 C++ 도구,
MSVC x64용 Qt, Vulkan SDK, vcpkg를 설치하세요.
버전과 구성 요소는 [Windows 빌드 가이드](../../tools/windows/README.md)의 지정 목록을 따르세요.

저장소 루트에서 PowerShell 7로 실행하세요.

```powershell
pwsh -NoProfile -File tools/windows/Setup.ps1 -BuildRoot C:\Dev\VulkanaClean
pwsh -NoProfile -File tools/windows/Build.ps1 -BuildRoot C:\Dev\VulkanaClean -Tests
C:\Dev\VulkanaClean\stage\bin\imageeditor.exe
```

새로 복제한 저장소를 테스트할 때는 새 빌드 디렉터리를 사용하세요. 다른 체크아웃에 맞춰
설정한 디렉터리를 재사용하지 마세요. Setup은 의존성을 검증하고 사전 검사를 수행합니다.
Build는 언어 팩을 컴파일하고 앱을 빌드·테스트한 뒤 실행에 필요한 파일을 배치합니다.
초기 설정에는 빌드 의존성을 받기 위한 인터넷 연결이 필요하지만, 앱과 함께 제공되는
개체 선택은 로컬에서 실행되며 사용 시 Python 설치나 모델 다운로드가 필요하지 않습니다.

의존성 경로 변경, 네이티브 스모크 테스트, 깨끗한 포터블 폴더 생성 방법은 Windows 가이드를
참조하세요. 환경 설정은 앱 폴더와 별도의 사용자별 저장 공간에 보관됩니다.

## 아키텍처

- `src/core` — 플랫폼에 독립적인 문서, 레이어, 기하 처리, 이미지 알고리즘, 실행 취소 가능한 트랜잭션.
- `src/render` — 공유 Vulkan 캔버스, 합성, 오버레이, 용량이 제한된 GPU 캐시.
- `src/ui` — Qt 패널, 도구, 문서 탭, 텍스트/도형 준비 처리, 파일 I/O.
- `src/platform` — 리소스 검색, 데스크톱 통합, 선택적 서비스.
- `src/app` — 애플리케이션 진입점. `shaders` — Vulkan 셰이더 소스.

각 문서는 자신의 내용과 작업 기록을 소유하며, 탭들은 하나의 작업 공간과 Vulkan 렌더러를
공유합니다. 편집 가능한 원본 데이터는 렌더링 캐시와 분리되어 있습니다.
캔버스, 픽셀 미리 보기, 내보내기는 같은 합성 규칙을 사용합니다.

[개발자 문서](../README.md)와 [아키텍처 가이드](../ARCHITECTURE.md)를 참조하세요.

## 라이선스

Vulkana가 소유한 코드, 문서, 자산에는 [MIT 라이선스](../../LICENSE)가 적용됩니다.
제3자 자료에는 각각의 라이선스가 계속 적용됩니다. [제3자 고지](../../THIRD_PARTY.md)를 참조하세요.
