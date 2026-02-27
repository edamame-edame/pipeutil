# tests/CMakeLists.txt — テストスイートのルート
# PIPEUTIL_BUILD_TESTS=ON 時に親 CMakeLists.txt から add_subdirectory される。

cmake_minimum_required(VERSION 3.25)

# ─── Google Test の取得 ───────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)
# Windows: ランタイムライブラリを強制的に /MT に揃える
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# GTest のインクルードパスを各テストターゲットで使えるようにする
include(GoogleTest)

# ─── サブディレクトリ ─────────────────────────────────────────────────
add_subdirectory(cpp)
