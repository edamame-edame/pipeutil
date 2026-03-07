# ─── ソースファイル ───────────────────────────────────────────────────
set(CORE_SOURCES
    src/pipe_error.cpp
    src/message.cpp
    src/io_buffer.cpp
    src/pipe_server.cpp
    src/pipe_client.cpp
    src/multi_pipe_server.cpp
    src/rpc_pipe_client.cpp
    src/rpc_pipe_server.cpp
    src/detail/crc32c.cpp
)

if(WIN32)
    list(APPEND CORE_SOURCES src/platform/win32_pipe.cpp)
else()
    list(APPEND CORE_SOURCES src/platform/posix_pipe.cpp)
endif()

# ─── ターゲット定義 ───────────────────────────────────────────────────
add_library(pipeutil_core SHARED ${CORE_SOURCES})

# DLL エクスポートマクロの自動生成（CMAKE 組み込み機能）
# → include/pipeutil/pipeutil_export.hpp を生成する
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
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src        # detail/crc32c.hpp, detail/platform_factory.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform  # win32_pipe.hpp / posix_pipe.hpp
)

# ─── コンパイルオプション ─────────────────────────────────────────────
target_compile_options(pipeutil_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /utf-8 /wd4251 /wd4275>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)

# ─── プラットフォーム依存リンク ───────────────────────────────────────
# std::thread / std::counting_semaphore が pthread を使う環境（Linux 等）に対応
find_package(Threads REQUIRED)
target_link_libraries(pipeutil_core PRIVATE Threads::Threads)

# ─── バージョン情報 ───────────────────────────────────────────────────
set_target_properties(pipeutil_core PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME pipeutil_core
)

# ─── エクスポート定義シンボル ─────────────────────────────────────────
# Windows: DLL エクスポートを有効化
target_compile_definitions(pipeutil_core PRIVATE PIPEUTIL_CORE_EXPORTS)
