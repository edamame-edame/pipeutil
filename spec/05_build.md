# pipeutil — ビルドシステム仕様

## 1. 概要

ビルドシステムには **CMake ≥ 3.25** を使用します。
Windows（MSVC / Clang-cl）と Linux（GCC / Clang）の両方に対応します。

---

## 2. CMake ターゲット構成

```
pipeutil_core          [SHARED]  ... C++ コアライブラリ (DLL/SO)
_pipeutil              [MODULE]  ... Python C API 拡張モジュール (.pyd/.so)
```

テスト用:
```
pipeutil_tests         [EXECUTABLE]  ... Google Test ベースの C++ テスト（オプション）
```

---

## 3. ディレクトリ別 CMakeLists.txt

### 3.1 トップレベル `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.25)
project(pipeutil VERSION 0.1.0 LANGUAGES CXX)

# ─── グローバル設定 ──────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ビルドタイプのデフォルト設定
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# シンボル可視性のデフォルトを hidden に設定（Linux）
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ─── オプション ──────────────────────────────────────────────────────
option(PIPEUTIL_BUILD_PYTHON  "Build Python C API extension" ON)
option(PIPEUTIL_BUILD_TESTS   "Build unit tests (requires GTest)" OFF)
option(PIPEUTIL_BUILD_EXAMPLES "Build examples" OFF)

# ─── サブディレクトリ ─────────────────────────────────────────────────
add_subdirectory(source)

if(PIPEUTIL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(PIPEUTIL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

### 3.2 `source/CMakeLists.txt`

```cmake
add_subdirectory(core)

if(PIPEUTIL_BUILD_PYTHON)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter Development.Module)
    add_subdirectory(python)
endif()
```

### 3.3 `source/core/CMakeLists.txt`

```cmake
# ─── ソースファイル ───────────────────────────────────────────────────
set(CORE_SOURCES
    src/pipe_server.cpp
    src/pipe_client.cpp
    src/message.cpp
    src/io_buffer.cpp
)

if(WIN32)
    list(APPEND CORE_SOURCES src/platform/win32_pipe.cpp)
else()
    list(APPEND CORE_SOURCES src/platform/posix_pipe.cpp)
endif()

# ─── ターゲット定義 ───────────────────────────────────────────────────
add_library(pipeutil_core SHARED ${CORE_SOURCES})

# エクスポートマクロ自動生成（CMake 組み込み）
include(GenerateExportHeader)
generate_export_header(pipeutil_core
    BASE_NAME         PIPEUTIL_CORE
    EXPORT_MACRO_NAME PIPEUTIL_API
    EXPORT_FILE_NAME  include/pipeutil/pipeutil_export.hpp
)

# ─── インクルードパス ─────────────────────────────────────────────────
target_include_directories(pipeutil_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>  # generated export header
        $<INSTALL_INTERFACE:include>
)

# ─── コンパイルオプション ─────────────────────────────────────────────
target_compile_options(pipeutil_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)

# ─── プラットフォーム依存リンク ───────────────────────────────────────
if(WIN32)
    # 追加リンクなし（Windows API は自動リンク）
else()
    # POSIX スレッド
    find_package(Threads REQUIRED)
    target_link_libraries(pipeutil_core PRIVATE Threads::Threads)
endif()

# ─── バージョン情報 ───────────────────────────────────────────────────
set_target_properties(pipeutil_core PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME pipeutil_core
)
```

### 3.4 `source/python/CMakeLists.txt`

```cmake
# ─── ソースファイル ───────────────────────────────────────────────────
set(PYTHON_EXT_SOURCES
    _pipeutil_module.cpp
    py_pipe_server.cpp
    py_pipe_client.cpp
    py_message.cpp
    py_exceptions.cpp
)

# ─── ターゲット定義（MODULE = Python 拡張モジュール）────────────────────
Python3_add_library(_pipeutil MODULE WITH_SOABI ${PYTHON_EXT_SOURCES})

# ─── インクルードパス ─────────────────────────────────────────────────
target_include_directories(_pipeutil PRIVATE
    ${Python3_INCLUDE_DIRS}
)

# ─── C++ コアへのリンク ───────────────────────────────────────────────
target_link_libraries(_pipeutil PRIVATE pipeutil_core)

# ─── コンパイルオプション ─────────────────────────────────────────────
target_compile_options(_pipeutil PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)

# ─── 出力先 ──────────────────────────────────────────────────────────
# Python パッケージディレクトリ直下に出力
set_target_properties(_pipeutil PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/python/pipeutil
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/python/pipeutil  # Windows .pyd
)
```

---

## 4. ビルド手順

### 4.1 Windows（MSVC 2022）

```powershell
# ビルドディレクトリ作成
cmake -S . -B build/windows `
      -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_BUILD_TYPE=Release `
      -DPIPEUTIL_BUILD_PYTHON=ON

# ビルド
cmake --build build/windows --config Release --parallel
```

### 4.2 Linux（GCC 13+）

```bash
cmake -S . -B build/linux \
      -DCMAKE_BUILD_TYPE=Release \
      -DPIPEUTIL_BUILD_PYTHON=ON

cmake --build build/linux --parallel $(nproc)
```

### 4.3 デバッグビルド

```bash
cmake -S . -B build/debug \
      -DCMAKE_BUILD_TYPE=Debug \
      -DPIPEUTIL_BUILD_TESTS=ON

cmake --build build/debug --parallel
ctest --test-dir build/debug -V
```

---

## 5. 出力成果物

| 成果物 | Windows パス | Linux パス |
|-------|-------------|-----------|
| `pipeutil_core.dll` | `build/windows/Release/pipeutil_core.dll` | — |
| `pipeutil_core.lib` | `build/windows/Release/pipeutil_core.lib` | — |
| `libpipeutil_core.so` | — | `build/linux/libpipeutil_core.so.0.1.0` |
| `_pipeutil.pyd` | `python/pipeutil/_pipeutil.pyd` | — |
| `_pipeutil.so` | — | `python/pipeutil/_pipeutil.cpython-313-x86_64-linux-gnu.so` |

---

## 6. Python パッケージのインストール

```bash
# 開発インストール (editable)
pip install -e .

# または直接インストール
pip install .
```

`pyproject.toml` で `scikit-build-core` または `setuptools` + `cmake` を使用する。

### `pyproject.toml` (概要)

```toml
[build-system]
requires = ["scikit-build-core>=0.9", "cmake>=3.25"]
build-backend = "scikit_build_core.build"

[project]
name = "pipeutil"
version = "0.1.0"
requires-python = ">=3.8,<3.15"
description = "High-speed IPC pipe communication library"

[tool.scikit-build]
cmake.build-type = "Release"
cmake.args = ["-DPIPEUTIL_BUILD_PYTHON=ON"]
wheel.packages = ["python/pipeutil"]
```

---

## 7. コンパイラ警告・サニタイザー設定

### 7.1 警告レベル

| コンパイラ | フラグ |
|-----------|--------|
| MSVC | `/W4 /WX /utf-8` |
| GCC / Clang | `-Wall -Wextra -Wpedantic -Werror` |

### 7.2 AddressSanitizer（開発用）

```bash
cmake -S . -B build/asan \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

cmake --build build/asan --parallel
```

---

## 8. CI/CD マトリクス（GitHub Actions 想定）

| OS | コンパイラ | Python | BUILD_TYPE |
|----|-----------|--------|------------|
| Windows Server 2022 | MSVC 2022 | 3.8 | Release |
| Ubuntu 24.04 | GCC 13 | 3.11 | Release |
| Ubuntu 24.04 | Clang 17 | 3.13 | Release |
| Ubuntu 24.04 | GCC 13 | 3.14 | Debug + ASan |

---

## 9. 依存ライブラリのバージョン固定

| 依存 | バージョン要件 | 取得方法 |
|------|------------|---------|
| CMake | ≥ 3.25 | システムパッケージ / 公式インストーラ |
| Python 3.8 - 3.14 | >=3.8, <3.15 | システム Python / pyenv |
| scikit-build-core | ≥ 0.9 | `pip install scikit-build-core` |
| Google Test | ≥ 1.14 | `FetchContent` (CMake) |
