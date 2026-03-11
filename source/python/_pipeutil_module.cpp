// _pipeutil_module.cpp — Python 拡張モジュール初期化
// 仕様: spec/04_python_wrapper.md §8
//
// ADD_OBJECT_OR_FAIL マクロ: PyModule_AddObjectRef の戻り値を検査し、
//   失敗時にモジュールを DECREF して nullptr を返す（R-010 対応済み）。

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "py_message.hpp"
#include "py_pipe_server.hpp"
#include "py_pipe_client.hpp"
#include "py_multi_pipe_server.hpp"
#include "py_rpc_pipe_client.hpp"
#include "py_rpc_pipe_server.hpp"
#include "py_pipe_stats.hpp"
#include "py_exceptions.hpp"

// ─── モジュール定義 ───────────────────────────────────────────────────

static PyModuleDef pipeutil_module_def = {
    PyModuleDef_HEAD_INIT,
    "_pipeutil",          // m_name
    "pipeutil C extension — high-speed IPC pipe communication",  // m_doc
    -1,                   // m_size (no per-interpreter state)
    nullptr,              // m_methods (型は型オブジェクトで登録)
    nullptr,              // m_slots
    nullptr,              // m_traverse
    nullptr,              // m_clear
    nullptr               // m_free
};

// ─── モジュール初期化 ─────────────────────────────────────────────────

/// ADD_OBJECT_OR_FAIL:
/// PyModule_AddObjectRef は参照を盗まない（非 steal, Python 3.10+）。
/// 戻り値チェックを必須とし、失敗時はモジュールを DECREF して nullptr を返す。
#define ADD_OBJECT_OR_FAIL(mod, name, obj)                        \
    do {                                                          \
        if (PyModule_AddObjectRef((mod), (name), (obj)) < 0) {   \
            Py_DECREF(mod);                                       \
            return nullptr;                                       \
        }                                                         \
    } while (0)

PyMODINIT_FUNC PyInit__pipeutil(void) {
    // 1. 型オブジェクトの準備（継承ツリーを設定）
    if (PyType_Ready(&pyutil::PyMessage_Type)         < 0) return nullptr;
    if (PyType_Ready(&pyutil::PyPipeServer_Type)      < 0) return nullptr;
    if (PyType_Ready(&pyutil::PyPipeClient_Type)      < 0) return nullptr;
    if (PyType_Ready(&pyutil::PyMultiPipeServer_Type) < 0) return nullptr;
    if (PyType_Ready(&pyutil::PyRpcPipeClient_Type)   < 0) return nullptr;
    if (PyType_Ready(&pyutil::PyRpcPipeServer_Type)   < 0) return nullptr;
    if (PyType_Ready(&pyutil::PyPipeStats_Type)       < 0) return nullptr;

    // 2. モジュール作成
    PyObject* m = PyModule_Create(&pipeutil_module_def);
    if (!m) return nullptr;

    // 3. 型の登録（ADD_OBJECT_OR_FAIL = 非 steal + 戻り値チェック）
    ADD_OBJECT_OR_FAIL(m, "Message",
        reinterpret_cast<PyObject*>(&pyutil::PyMessage_Type));
    ADD_OBJECT_OR_FAIL(m, "PipeServer",
        reinterpret_cast<PyObject*>(&pyutil::PyPipeServer_Type));
    ADD_OBJECT_OR_FAIL(m, "PipeClient",
        reinterpret_cast<PyObject*>(&pyutil::PyPipeClient_Type));
    ADD_OBJECT_OR_FAIL(m, "MultiPipeServer",
        reinterpret_cast<PyObject*>(&pyutil::PyMultiPipeServer_Type));
    ADD_OBJECT_OR_FAIL(m, "RpcPipeClient",
        reinterpret_cast<PyObject*>(&pyutil::PyRpcPipeClient_Type));
    ADD_OBJECT_OR_FAIL(m, "RpcPipeServer",
        reinterpret_cast<PyObject*>(&pyutil::PyRpcPipeServer_Type));
    ADD_OBJECT_OR_FAIL(m, "PipeStats",
        reinterpret_cast<PyObject*>(&pyutil::PyPipeStats_Type));

    // 4. 例外の登録（init_exceptions 内で g_* グローバルを初期化）
    if (pyutil::init_exceptions(m) < 0) {
        Py_DECREF(m);
        return nullptr;
    }

    // 5. バージョン定数
    if (PyModule_AddStringConstant(m, "__version__", "0.1.0") < 0) {
        Py_DECREF(m);
        return nullptr;
    }

    return m;
}

#undef ADD_OBJECT_OR_FAIL
