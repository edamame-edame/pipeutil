# ─── ソースファイル ───────────────────────────────────────────────────
set(PYTHON_EXT_SOURCES
    _pipeutil_module.cpp
    py_pipe_server.cpp
    py_pipe_client.cpp
    py_multi_pipe_server.cpp
    py_rpc_pipe_client.cpp
    py_rpc_pipe_server.cpp
    py_message.cpp
    py_exceptions.cpp
)

# ─── ターゲット定義（MODULE = Python 拡張モジュール）────────────────────
# WITH_SOABI: Python バージョン・アーキテクチャをファイル名に付与
#   Windows: _pipeutil.pyd
#   Linux:   _pipeutil.cpython-313-x86_64-linux-gnu.so
Python3_add_library(_pipeutil MODULE WITH_SOABI ${PYTHON_EXT_SOURCES})

# ─── インクルードパス ─────────────────────────────────────────────────
target_include_directories(_pipeutil PRIVATE
    ${Python3_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}   # py_message.hpp 等の自モジュールヘッダ
)

# ─── C++ コアへのリンク ───────────────────────────────────────────────
target_link_libraries(_pipeutil PRIVATE pipeutil_core)

# ─── コンパイルオプション ─────────────────────────────────────────────
target_compile_options(_pipeutil PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /utf-8 /wd4251 /wd4275>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)

# ─── 出力先: Python パッケージディレクトリ直下 ────────────────────────
set(_PIPEUTIL_PY_PACKAGE_DIR "${CMAKE_SOURCE_DIR}/python/pipeutil")

set_target_properties(_pipeutil PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${_PIPEUTIL_PY_PACKAGE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${_PIPEUTIL_PY_PACKAGE_DIR}"  # Windows .pyd
)

# マルチコンフィグ生成器（Visual Studio 等）で Release/Debug サブディレクトリへ
# 分岐しないよう、各構成ごとの出力先も明示的に package 直下へ固定する。
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" CFG_UPPER)
    set_target_properties(_pipeutil PROPERTIES
        "LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER}" "${_PIPEUTIL_PY_PACKAGE_DIR}"
        "RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER}" "${_PIPEUTIL_PY_PACKAGE_DIR}"
    )
endforeach()

# Windows では拡張モジュール読み込み時に依存 DLL が同ディレクトリに必要。
# wheel.packages で収集される package 直下へ core DLL をコピーして同梱する。
if(WIN32)
    add_custom_command(TARGET _pipeutil POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:pipeutil_core>
            "${_PIPEUTIL_PY_PACKAGE_DIR}/$<TARGET_FILE_NAME:pipeutil_core>"
    )
endif()
