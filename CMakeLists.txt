cmake_minimum_required(VERSION 3.25)
project(pipeutil VERSION 1.0.0 LANGUAGES CXX)

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
# デバッグログ: cmake -DPIPEUTIL_DEBUG_LOG=ON で有効化。リリースでは OFF を維持すること。
option(PIPEUTIL_DEBUG_LOG      "Enable verbose stderr debug logging in C extension" OFF)

# ─── サブディレクトリ ─────────────────────────────────────────────────
add_subdirectory(source)

# デバッグログ定義の伝搬: pipeutil_core と _pipeutil に同時に適用する
if(PIPEUTIL_DEBUG_LOG)
    message(STATUS "PIPEUTIL_DEBUG_LOG: enabled (verbose stderr logging)")
    target_compile_definitions(pipeutil_core PRIVATE PIPEUTIL_DEBUG_LOG)
    if(PIPEUTIL_BUILD_PYTHON AND TARGET _pipeutil)
        target_compile_definitions(_pipeutil PRIVATE PIPEUTIL_DEBUG_LOG)
    endif()
    if(PIPEUTIL_BUILD_PYTHON AND PIPEUTIL_WITH_ASYNC AND TARGET _pipeutil_async)
        target_compile_definitions(_pipeutil_async PRIVATE PIPEUTIL_DEBUG_LOG)
    endif()
endif()

if(PIPEUTIL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(PIPEUTIL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
