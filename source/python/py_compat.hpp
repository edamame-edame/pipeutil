// py_compat.hpp — Python C API backward‑compatibility shims
// 仕様: Python 3.8+ 対応（pyproject.toml requires-python = ">=3.8,<3.15"）
//
// 以下の API は Python バージョンによって追加されたため、古い Python で
// ビルドするときの代替実装をここで提供する。
//
// ・PyModule_AddObjectRef  — Python 3.10 で追加（非 steal 版 AddObject）
// ・PyObject_CallOneArg    — Python 3.9  で追加（単引数呼び出しのショートハンド）
//
// 使い方:
//   このヘッダを各 .cpp ファイルの先頭 include として使用すること。
//   PY_SSIZE_T_CLEAN を定義し <Python.h> を最初にインクルードする。

#pragma once

// PY_SSIZE_T_CLEAN を Python.h より先に定義する必要がある
#define PY_SSIZE_T_CLEAN
#include <Python.h>

// ─── PyModule_AddObjectRef (Python 3.10+) ────────────────────────────────────
// 非 steal 版 PyModule_AddObject。obj の参照カウントを変更しない。
// Python 3.10 未満では PyModule_AddObject（steal 版）を使って同等の動作を実現する。
#if PY_VERSION_HEX < 0x030a0000
static inline int PyModule_AddObjectRef(PyObject* mod, const char* name, PyObject* obj) noexcept {
    Py_INCREF(obj);                        // steal 対策で先に +1
    if (PyModule_AddObject(mod, name, obj) < 0) {
        Py_DECREF(obj);                    // 失敗時は +1 を戻す
        return -1;
    }
    return 0;
}
#endif

// ─── PyObject_CallOneArg (Python 3.9+) ───────────────────────────────────────
// callable(arg) を呼び出す。Python 3.9 未満は PyObject_CallFunctionObjArgs で代替。
// 注: 呼び出し箇所を直接 PyObject_CallFunctionObjArgs に書き換えているため、
//     このシムは主に新規コードが py_compat.hpp 経由でコンパイルされる場合向け。
#if PY_VERSION_HEX < 0x030900
static inline PyObject* PyObject_CallOneArg(PyObject* func, PyObject* arg) noexcept(false) {
    return PyObject_CallFunctionObjArgs(func, arg, (PyObject*)NULL);
}
#endif

// ─── PyMODINIT_FUNC 可視性修正 (Python 3.8) ─────────────────────────────────
// Python 3.8 の pyport.h では PyMODINIT_FUNC = "extern 'C' PyObject*"
// （__attribute__((visibility("default"))) が欠如している）。
// CMake の Python3_add_library MODULE は -fvisibility=hidden を付与するため、
// このままでは PyInit 関数が hidden になりインポート不能になる。
// Python 3.9 以降のヘッダーは visibility 属性を自ら付与するため影響なし。
#if PY_VERSION_HEX < 0x030900 && !defined(_WIN32)
#  undef  PyMODINIT_FUNC
#  define PyMODINIT_FUNC \
    extern "C" __attribute__((visibility("default"))) PyObject*
#endif
