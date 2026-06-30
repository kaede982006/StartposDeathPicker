# 개발자 문서

`Startpos Death Picker`는 Geode 기반 Geometry Dash 모드입니다. 로컬 Windows 빌드는 Visual Studio Build Tools/MSVC 기준으로 관리합니다.

## 기본 정보

- CMake 프로젝트명: `StartposDeathPicker`
- 모드 ID: `hyobeen.startpos-death-picker`
- 현재 버전: `1.0.0`
- C++ 표준: C++23
- Geode 버전: `5.7.1`
- Geometry Dash 버전: `2.2081`
- Geode 의존성: `geode.node-ids >= v1.23.3`

주요 파일:

- `CMakeLists.txt`: CMake 및 Geode 빌드 설정
- `mod.json`: 모드 메타데이터와 Geode 의존성
- `src/main.cpp`: 모드 구현
- `.github/workflows/release.yml`: GitHub Actions 릴리스 빌드
- `about.md`: 모드 설명

## 사전 준비

필요 도구:

- CMake 3.21 이상
- Visual Studio Build Tools 또는 Visual Studio C++ 도구chain
- Geode SDK
- Geode CLI

`CMakeLists.txt`는 `GEODE_SDK` 환경 변수를 사용합니다.

```powershell
$env:GEODE_SDK = "C:\Users\xisik\geode-sdk"
```

## Windows 로컬 빌드

이 환경에서는 Visual Studio 18 Build Tools가 설치되어 있으므로 다음 명령을 사용합니다.

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TLS_CAINFO="C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"
cmake --build build --config RelWithDebInfo
```

`CMAKE_TLS_CAINFO`는 Geode SDK가 CPM.cmake를 다운로드할 때 SSL 인증서 오류가 나는 경우 필요합니다. 오류가 없는 환경에서는 생략할 수 있습니다.

Visual Studio 2022 환경에서는 generator 이름만 바꿉니다.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```

빌드 성공 시 다음 파일이 생성됩니다.

```text
build\hyobeen.startpos-death-picker.geode
```

현재 확인된 성공 빌드 명령은 다음입니다.

```powershell
cmake --build build --config RelWithDebInfo
```

## 클린 빌드

가장 확실한 클린 빌드는 `build` 디렉터리를 삭제한 뒤 다시 configure/build 하는 방식입니다.

```powershell
Remove-Item -Recurse -Force build
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TLS_CAINFO="C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"
cmake --build build --config RelWithDebInfo
```

`cmake --build build --target clean`은 컴파일 산출물을 지우는 용도입니다. Geode 패키징 target의 완료 표시 파일이 남아 있으면 `.geode`가 삭제된 상태에서도 패키징 target이 최신으로 판단될 수 있습니다. `.geode` 파일까지 확실히 다시 만들려면 위처럼 `build` 디렉터리 전체를 삭제하는 방식을 권장합니다.

부분 클린 후 `.geode`만 다시 만들고 싶다면 패키징 완료 표시 파일과 기존 패키지를 지운 뒤 package target을 실행합니다.

```powershell
Remove-Item -Force build\hyobeen.startpos-death-picker.geode -ErrorAction SilentlyContinue
Remove-Item -Force build\CMakeFiles\StartposDeathPicker_PACKAGE -ErrorAction SilentlyContinue
cmake --build build --config RelWithDebInfo --target StartposDeathPicker_PACKAGE
```

## 산출물 확인

```powershell
Get-ChildItem -Recurse build -Filter *.geode
Get-ChildItem -Recurse build -Include *.dll,*.pdb,*.lib
```

일반적으로 확인해야 할 파일:

- `build\hyobeen.startpos-death-picker.geode`
- `build\RelWithDebInfo\hyobeen.startpos-death-picker.dll`

빌드 로그에 다음 줄이 나오면 패키징까지 완료된 것입니다.

```text
Successfully packaged hyobeen.startpos-death-picker.geode
Installed hyobeen.startpos-death-picker.geode
```

## 주의 사항

Ninja generator를 사용할 때는 `-A x64`를 붙이면 안 됩니다.

```text
Generator Ninja does not support platform specification, but platform x64 was specified.
```

이 오류가 나면 `build` 디렉터리를 삭제하고 Visual Studio generator로 다시 configure 합니다.

```powershell
Remove-Item -Recurse -Force build
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TLS_CAINFO="C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"
```

Windows에서 MinGW/GCC가 선택되면 Geode 의존성 빌드 중 `__except`, `std::stacktrace`, `duplicate inline` 같은 오류가 날 수 있습니다. Windows 로컬 빌드는 MSVC가 선택되도록 Visual Studio generator를 사용합니다.

## GitHub Actions 릴리스

릴리스는 `.github/workflows/release.yml`의 `workflow_dispatch`로 수동 실행합니다.

빌드 대상:

- Windows
- macOS
- iOS
- Android32
- Android64

워크플로는 `geode-sdk/build-geode-mod@main`으로 각 대상을 빌드하고, `geode-sdk/build-geode-mod/combine@main`으로 산출물을 합칩니다.

현재 릴리스 job은 `v1.0.0` 릴리스를 다시 생성합니다. 버전을 바꿀 때는 다음 파일을 함께 수정합니다.

- `CMakeLists.txt`의 `project(StartposDeathPicker VERSION ...)`
- `mod.json`의 `"version"`
- `.github/workflows/release.yml`의 태그, 제목, release notes

## 수동 검증

현재 자동 테스트는 없습니다. 빌드 후 게임 안에서 다음 항목을 확인합니다.

- 레벨 진입 전 `D`로 팝업 열기/닫기
- 팝업에서 `T`로 bar/line graph 전환
- 팝업에서 `H`/`L`로 전체 레벨 및 start position 구간 이동
- 사망 후 전체 히스토그램과 start position 히스토그램 증가 확인
- start position 추가, 삭제, 이동 후 기존 데이터 정렬 확인

## 런타임 데이터

모드는 Geode 모드 save directory 아래에 레벨별 사망 데이터를 저장합니다.

```text
death-data\<level-key>.txt
```

데이터 파일은 `SDP1` magic 문자열로 시작합니다. 파일 형식을 바꾸는 경우 기존 사용자 데이터 마이그레이션이나 하위 호환 처리가 필요합니다.
