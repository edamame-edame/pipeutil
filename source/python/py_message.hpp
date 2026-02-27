// py_message.hpp — PyMessage 型宣言
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pipeutil/message.hpp"

namespace pyutil {

// ─── PyMessage 構造体 ──────────────────────────────────────────────
// Python ヒープ上に確保される。msg は new で所有、dealloc で delete する。

struct PyMessage {
    PyObject_HEAD
    pipeutil::Message* msg;  // nullptr = 未初期化（通常の使用では起きない）
};

extern PyTypeObject PyMessage_Type;

/// C++ Message から PyMessage を生成するファクトリ（参照カウント済みオブジェクトを返す）
/// 失敗時は nullptr を返し Python 例外をセットする。
PyObject* PyMessage_from_message(pipeutil::Message msg) noexcept;

/// PyObject* から pipeutil::Message を取り出すユーティリティ。
/// Message / bytes / bytearray / str をすべて受け付ける（send の引数変換に使用）。
/// 失敗時は nullptr を返し Python 例外をセットする。
/// out_msg: 成功時に値がセットされる。
bool PyMessage_convert(PyObject* obj, pipeutil::Message& out_msg) noexcept;

} // namespace pyutil
