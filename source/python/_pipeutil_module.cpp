// _pipeutil_module.cpp — Python 拡張モジュール初期化
// 仕様: spec/04_python_wrapper.md §8
//
// ADD_OBJECT_OR_FAIL マクロ: PyModule_AddObjectRef の戻り値を検査し、
//   失敗時にモジュールを DECREF して nullptr を返す（R-010 対応済み）。

#include "py_compat.hpp"  // must precede all Python includes; provides 3.8+ shims

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

// Python 3.8: PyMODINIT_FUNC = "extern \"C\" PyObject*" (no visibility attr).
// -fvisibility=hidden が CMake MODULE ターゲットに付くため pragma で明示エクスポート。
#ifdef __GNUC__
#  pragma GCC visibility push(default)
#endif
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

    // 5. PipeAcl 定数を SimpleNamespace として公開
    //    使用例: pipeutil.PipeAcl.Default / LocalSystem / Everyone / Custom
    {
        PyObject* types_mod = PyImport_ImportModule("types");
        if (!types_mod) { Py_DECREF(m); return nullptr; }
        PyObject* sns_cls = PyObject_GetAttrString(types_mod, "SimpleNamespace");
        Py_DECREF(types_mod);
        if (!sns_cls) { Py_DECREF(m); return nullptr; }

        PyObject* kwds = Py_BuildValue("{s:i,s:i,s:i,s:i}",
            "Default",     0,
            "LocalSystem", 1,
            "Everyone",    2,
            "Custom",      3);
        PyObject* empty_args = PyTuple_New(0);
        PyObject* acl_ns = (kwds && empty_args)
            ? PyObject_Call(sns_cls, empty_args, kwds)
            : nullptr;
        Py_DECREF(sns_cls);
        Py_XDECREF(kwds);
        Py_XDECREF(empty_args);
        if (!acl_ns) { Py_DECREF(m); return nullptr; }
        ADD_OBJECT_OR_FAIL(m, "PipeAcl", acl_ns);
        Py_DECREF(acl_ns);
    }

    // 5. バージョン定数
    if (PyModule_AddStringConstant(m, "__version__", "1.0.0") < 0) {
        Py_DECREF(m);
        return nullptr;
    }

    return m;
}
#ifdef __GNUC__
#  pragma GCC visibility pop
#endif

#undef ADD_OBJECT_OR_FAIL
