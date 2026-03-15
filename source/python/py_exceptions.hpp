// py_exceptions.hpp — Python 例外グローバル変数・変換関数の宣言
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pipeutil/pipe_error.hpp"

namespace pyutil {

// ─── 例外 PyObject* グローバル変数 ────────────────────────────────
// _pipeutil_module.cpp の PyInit__pipeutil で初期化される。
// 各 .cpp ファイルは extern でこれらを参照する。

extern PyObject* g_PipeError;
extern PyObject* g_TimeoutError;
extern PyObject* g_ConnectionResetError;
extern PyObject* g_BrokenPipeError;
extern PyObject* g_NotConnectedError;
extern PyObject* g_InvalidMessageError;
extern PyObject* g_ConnectionRejectedError;  // v1.1.0 A-001 HELLO 拒否
extern PyObject* g_QueueFullError;            // v1.1.0 A-002 事前予約

// ─── C++ 例外 → Python 例外 変換 ──────────────────────────────────
/// PipeException を受け取り、対応する Python 例外をセットする。
/// catch ブロックで呼び出すこと。
/// 例:
///   catch (const pipeutil::PipeException& e) {
///       pyutil::set_python_exception(e);
///       return nullptr;
///   }
void set_python_exception(const pipeutil::PipeException& e) noexcept;

/// モジュール初期化時に呼ぶ。成功時 0, 失敗時 -1 を返す。
int init_exceptions(PyObject* module) noexcept;

} // namespace pyutil
