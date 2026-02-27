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

# シンボル可視性のデフォルトを hidden に設定（Linux 向け最小公開面）
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ─── ビルドオプション ─────────────────────────────────────────────────
option(PIPEUTIL_BUILD_PYTHON   "Build Python C API extension (_pipeutil)"   ON)
option(PIPEUTIL_BUILD_TESTS    "Build unit tests (requires GTest)"          OFF)
option(PIPEUTIL_BUILD_EXAMPLES "Build usage examples"                       OFF)

# ─── サブディレクトリ ─────────────────────────────────────────────────
add_subdirectory(source)

if(PIPEUTIL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(PIPEUTIL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
