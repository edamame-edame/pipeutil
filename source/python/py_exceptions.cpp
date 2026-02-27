// py_exceptions.cpp — Python 例外クラスの定義・登録・変換
// 仕様: spec/04_python_wrapper.md §3

#include "py_exceptions.hpp"
#include <string>

namespace pyutil {

// ─── グローバル例外オブジェクト（初期値 nullptr）───────────────────────
PyObject* g_PipeError            = nullptr;
PyObject* g_TimeoutError         = nullptr;
PyObject* g_ConnectionResetError = nullptr;
PyObject* g_BrokenPipeError      = nullptr;
PyObject* g_NotConnectedError    = nullptr;
PyObject* g_InvalidMessageError  = nullptr;

// ─── 例外クラス階層登録 ────────────────────────────────────────────────

/// 例外クラス 1 つを作成してモジュールに登録するヘルパー
/// 戻り値: 成功 0 / 失敗 -1（Python 例外はすでにセット済み）
static int register_exception(
    PyObject* module,
    const char* full_name,   // "_pipeutil.FooError"
    const char* short_name,  // "FooError"
    PyObject* base,          // 基底例外
    PyObject** dest) noexcept
{
    *dest = PyErr_NewException(full_name, base, nullptr);
    if (!(*dest)) return -1;

    if (PyModule_AddObjectRef(module, short_name, *dest) < 0) {
        Py_DECREF(*dest);
        *dest = nullptr;
        return -1;
    }
    return 0;
}

int init_exceptions(PyObject* module) noexcept {
    // PipeError — ルート例外
    if (register_exception(module,
                           "_pipeutil.PipeError", "PipeError",
                           PyExc_Exception, &g_PipeError) < 0) return -1;

    // PipeError の派生クラス群
    if (register_exception(module,
                           "_pipeutil.TimeoutError", "TimeoutError",
                           g_PipeError, &g_TimeoutError) < 0) return -1;

    if (register_exception(module,
                           "_pipeutil.ConnectionResetError", "ConnectionResetError",
                           g_PipeError, &g_ConnectionResetError) < 0) return -1;

    if (register_exception(module,
                           "_pipeutil.BrokenPipeError", "BrokenPipeError",
                           g_PipeError, &g_BrokenPipeError) < 0) return -1;

    if (register_exception(module,
                           "_pipeutil.NotConnectedError", "NotConnectedError",
                           g_PipeError, &g_NotConnectedError) < 0) return -1;

    if (register_exception(module,
                           "_pipeutil.InvalidMessageError", "InvalidMessageError",
                           g_PipeError, &g_InvalidMessageError) < 0) return -1;

    return 0;
}

// ─── C++ 例外 → Python 例外 変換 ──────────────────────────────────────

void set_python_exception(const pipeutil::PipeException& e) noexcept {
    PyObject* exc_type = nullptr;

    switch (e.pipe_code()) {
        case pipeutil::PipeErrorCode::Timeout:
            exc_type = g_TimeoutError;        break;
        case pipeutil::PipeErrorCode::ConnectionReset:
            exc_type = g_ConnectionResetError; break;
        case pipeutil::PipeErrorCode::BrokenPipe:
            exc_type = g_BrokenPipeError;      break;
        case pipeutil::PipeErrorCode::NotConnected:
            exc_type = g_NotConnectedError;    break;
        case pipeutil::PipeErrorCode::InvalidMessage:
            exc_type = g_InvalidMessageError;  break;
        default:
            exc_type = g_PipeError;            break;
    }

    // グローバルが未初期化（モジュール初期化失敗等）の場合のフォールバック
    if (!exc_type) exc_type = PyExc_RuntimeError;

    PyErr_SetString(exc_type, e.what());
}

} // namespace pyutil
