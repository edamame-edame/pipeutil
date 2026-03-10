// source/python/py_async_module.cpp
// _pipeutil_async 拡張モジュール定義
// 仕様: spec/F004p2_async_native.md §4
//
// 公開型: AsyncPipeHandle
// モジュール関数: _fire_future, _on_linux_readable, _on_linux_writable

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "py_async_pipe.hpp"
#include "py_debug_log.hpp"

#include <cstddef>
#include <memory>

using namespace pipeutil::async;

// PyAsyncPipeHandle_Type の前方宣言 (anonymous namespace 内から tp_alloc を呼ぶため)
extern PyTypeObject PyAsyncPipeHandle_Type;

// ─────────────────────────────────────────────────────────────────────────────
// PyAsyncPipeHandle 型
// ──────────────────────────────────────────────────────────────────────────────
namespace {

struct PyAsyncPipeHandle {
    PyObject_HEAD
    AsyncPlatformPipe* pipe;  // owned
};

// ─── tp_new ─────────────────────────────────────────────────────────────────

static PyObject*
PyAsyncPipeHandle_new(PyTypeObject* type, PyObject* /*args*/, PyObject* /*kwds*/) {
    auto* self = reinterpret_cast<PyAsyncPipeHandle*>(type->tp_alloc(type, 0));
    if (self) self->pipe = nullptr;
    return reinterpret_cast<PyObject*>(self);
}

// ─── tp_init ────────────────────────────────────────────────────────────────

static int
PyAsyncPipeHandle_init(PyAsyncPipeHandle* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"buf_size", nullptr};
    Py_ssize_t buf_size = 65536;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|n",
                                     const_cast<char**>(kwlist), &buf_size)) {
        return -1;
    }
    if (buf_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "buf_size must be positive");
        return -1;
    }
    try {
        delete self->pipe;
        self->pipe = new AsyncPlatformPipe(static_cast<std::size_t>(buf_size));
    } catch (const pipeutil::PipeException& e) {
        set_async_pipe_exception(e);
        return -1;
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return -1;
    }
    return 0;
}

// ─── tp_dealloc ─────────────────────────────────────────────────────────────

static void
PyAsyncPipeHandle_dealloc(PyAsyncPipeHandle* self) {
    if (self->pipe) {
        AsyncPlatformPipe* pipe = self->pipe;
        self->pipe = nullptr;
        // dispatch thread の join は GIL を解放して実行 (R-046: デッドロック予防)
        Py_BEGIN_ALLOW_THREADS
        pipe->close();
        Py_END_ALLOW_THREADS
        delete pipe;
    }
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// ─── guard: パイプが初期化済みかつ接続済みかを確認 ─────────────────────────

static bool check_pipe(PyAsyncPipeHandle* self) noexcept {
    if (!self->pipe) {
        PyErr_SetString(PyExc_RuntimeError, "AsyncPipeHandle is closed or not initialized");
        return false;
    }
    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// メソッド実装
// ──────────────────────────────────────────────────────────────────────────────

// ─── connect ─────────────────────────────────────────────────────────────────

static PyObject*
PyAsyncPipeHandle_connect(PyAsyncPipeHandle* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"pipe_name", "timeout_ms", nullptr};
    const char* pipe_name = nullptr;
    long long   timeout_ms = 5000;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|L",
                                     const_cast<char**>(kwlist),
                                     &pipe_name, &timeout_ms)) {
        return nullptr;
    }
    if (!check_pipe(self)) return nullptr;

    pipeutil::PipeException* pending = nullptr;

    Py_BEGIN_ALLOW_THREADS
    try {
        self->pipe->client_connect(std::string{pipe_name},
                                   static_cast<int64_t>(timeout_ms));
    } catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) {
        set_async_pipe_exception(*pending);
        delete pending;
        return nullptr;
    }
    Py_RETURN_NONE;
}

// ─── server_create_and_accept ────────────────────────────────────────────────

static PyObject*
PyAsyncPipeHandle_server_create_and_accept(
    PyAsyncPipeHandle* self, PyObject* args, PyObject* kwds)
{
    static const char* kwlist[] = {"pipe_name", "timeout_ms", nullptr};
    const char* pipe_name  = nullptr;
    long long   timeout_ms = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|L",
                                     const_cast<char**>(kwlist),
                                     &pipe_name, &timeout_ms)) {
        return nullptr;
    }
    if (!check_pipe(self)) return nullptr;

    std::unique_ptr<AsyncPlatformPipe> forked;
    pipeutil::PipeException* pending = nullptr;

    Py_BEGIN_ALLOW_THREADS
    try {
        forked = self->pipe->server_create_and_accept(
            std::string{pipe_name}, static_cast<int64_t>(timeout_ms));
    } catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) {
        set_async_pipe_exception(*pending);
        delete pending;
        return nullptr;
    }

    // forked インスタンスを PyAsyncPipeHandle へラップ (R-040 対応)
    PyObject* new_handle = PyAsyncPipeHandle_Type.tp_alloc(&PyAsyncPipeHandle_Type, 0);
    if (!new_handle) return nullptr;  // forked は unique_ptr 解放で自動 delete

    reinterpret_cast<PyAsyncPipeHandle*>(new_handle)->pipe = forked.release();
    return new_handle;
}

// ─── async_read_frame ────────────────────────────────────────────────────────

static PyObject*
PyAsyncPipeHandle_async_read_frame(
    PyAsyncPipeHandle* self, PyObject* args, PyObject* kwds)
{
    static const char* kwlist[] = {"loop", "future", nullptr};
    PyObject* loop   = nullptr;
    PyObject* future = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO",
                                     const_cast<char**>(kwlist),
                                     &loop, &future)) {
        return nullptr;
    }
    if (!check_pipe(self)) return nullptr;

    try {
        self->pipe->async_read_frame(loop, future);
    } catch (const pipeutil::PipeException& e) {
        set_async_pipe_exception(e);
        return nullptr;
    }
    Py_RETURN_NONE;
}

// ─── async_write_frame ───────────────────────────────────────────────────────

static PyObject*
PyAsyncPipeHandle_async_write_frame(
    PyAsyncPipeHandle* self, PyObject* args, PyObject* kwds)
{
    static const char* kwlist[] = {"payload", "message_id", "loop", "future", nullptr};
    Py_buffer  payload_buf;
    long long  message_id = 0;
    PyObject*  loop       = nullptr;
    PyObject*  future     = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "y*LOO",
                                     const_cast<char**>(kwlist),
                                     &payload_buf, &message_id, &loop, &future)) {
        return nullptr;
    }
    if (!check_pipe(self)) {
        PyBuffer_Release(&payload_buf);
        return nullptr;
    }

    // (R-055) message_id は uint32_t フィールドに格納されるため事前に範囲を検証する。
    // 負値や 2^32 超値はラップし、照合不整合を引き起こす可能性がある。
    if (message_id < 0 || message_id > static_cast<long long>(0xFFFFFFFFLL)) {
        PyErr_SetString(PyExc_ValueError,
                        "message_id must be in range [0, 4294967295]");
        PyBuffer_Release(&payload_buf);
        return nullptr;
    }

    try {
        self->pipe->async_write_frame(
            static_cast<const std::byte*>(payload_buf.buf),
            static_cast<std::size_t>(payload_buf.len),
            static_cast<uint32_t>(message_id),
            loop, future);
    } catch (const pipeutil::PipeException& e) {
        PyBuffer_Release(&payload_buf);
        set_async_pipe_exception(e);
        return nullptr;
    }
    PyBuffer_Release(&payload_buf);
    Py_RETURN_NONE;
}

// ─── cancel ──────────────────────────────────────────────────────────────────

static PyObject*
PyAsyncPipeHandle_cancel(PyAsyncPipeHandle* self, PyObject* /*args*/) {
    if (self->pipe) {
#ifdef _WIN32
        // (R-056) Windows: cancel() は CancelIoEx() のみを呼ぶ（Python C-API 不使用）。
        // カーネル呼び出しのため GIL を解放して実行する。
        Py_BEGIN_ALLOW_THREADS
        self->pipe->cancel();
        Py_END_ALLOW_THREADS
#else
        // (R-056) Linux: cancel() は PyCapsule_GetPointer / PyObject_CallMethodObjArgs /
        // Py_CLEAR 等の Python C-API を呼ぶため GIL を保持したまま実行する。
        // GIL を解放すると未定義動作（クラッシュ / メモリ破壊）になる。
        self->pipe->cancel();
#endif
    }
    Py_RETURN_NONE;
}

// ─── close ───────────────────────────────────────────────────────────────────

static PyObject*
PyAsyncPipeHandle_close(PyAsyncPipeHandle* self, PyObject* /*args*/) {
    if (self->pipe) {
        // dispatch thread の join は GIL を解放して実行 (R-046: デッドロック予防)
        Py_BEGIN_ALLOW_THREADS
        self->pipe->close();
        Py_END_ALLOW_THREADS
    }
    Py_RETURN_NONE;
}

// ─── is_connected (getter) ───────────────────────────────────────────────────

static PyObject*
PyAsyncPipeHandle_get_is_connected(PyAsyncPipeHandle* self, void* /*closure*/) {
    if (!self->pipe) Py_RETURN_FALSE;
    return PyBool_FromLong(self->pipe->is_connected() ? 1 : 0);
}

// ─── __enter__ / __exit__ ────────────────────────────────────────────────────

static PyObject*
PyAsyncPipeHandle_enter(PyAsyncPipeHandle* self, PyObject* /*args*/) {
    Py_INCREF(self);
    return reinterpret_cast<PyObject*>(self);
}

static PyObject*
PyAsyncPipeHandle_exit(PyAsyncPipeHandle* self, PyObject* /*args*/) {
    if (self->pipe) {
        Py_BEGIN_ALLOW_THREADS
        self->pipe->close();
        Py_END_ALLOW_THREADS
    }
    Py_RETURN_NONE;
}

// ─── メソッドテーブル ─────────────────────────────────────────────────────────

static PyMethodDef PyAsyncPipeHandle_methods[] = {
    {"connect",
     reinterpret_cast<PyCFunction>(PyAsyncPipeHandle_connect),
     METH_VARARGS | METH_KEYWORDS, "クライアント接続（同期）"},
    {"server_create_and_accept",
     reinterpret_cast<PyCFunction>(PyAsyncPipeHandle_server_create_and_accept),
     METH_VARARGS | METH_KEYWORDS, "サーバー接続受付（同期）→ 接続済み AsyncPipeHandle を返す"},
    {"async_read_frame",
     reinterpret_cast<PyCFunction>(PyAsyncPipeHandle_async_read_frame),
     METH_VARARGS | METH_KEYWORDS, "非同期フレーム受信"},
    {"async_write_frame",
     reinterpret_cast<PyCFunction>(PyAsyncPipeHandle_async_write_frame),
     METH_VARARGS | METH_KEYWORDS, "非同期フレーム送信"},
    {"cancel",
     reinterpret_cast<PyCFunction>(PyAsyncPipeHandle_cancel),
     METH_NOARGS, "進行中の I/O をキャンセル"},
    {"close",
     reinterpret_cast<PyCFunction>(PyAsyncPipeHandle_close),
     METH_NOARGS, "ハンドルをクローズ"},
    {"__enter__",
     reinterpret_cast<PyCFunction>(PyAsyncPipeHandle_enter),
     METH_NOARGS, nullptr},
    {"__exit__",
     reinterpret_cast<PyCFunction>(PyAsyncPipeHandle_exit),
     METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

static PyGetSetDef PyAsyncPipeHandle_getset[] = {
    {"is_connected",
     reinterpret_cast<getter>(PyAsyncPipeHandle_get_is_connected),
     nullptr, "接続中かどうか", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

} // anonymous namespace

// ─── PyTypeObject 定義 ───────────────────────────────────────────────────────

PyTypeObject PyAsyncPipeHandle_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "_pipeutil_async.AsyncPipeHandle",         // tp_name
    sizeof(PyAsyncPipeHandle),                 // tp_basicsize
    0,                                         // tp_itemsize
    reinterpret_cast<destructor>(PyAsyncPipeHandle_dealloc), // tp_dealloc
    0,                                         // tp_vectorcall_offset
    nullptr,                                   // tp_getattr
    nullptr,                                   // tp_setattr
    nullptr,                                   // tp_as_async
    nullptr,                                   // tp_repr
    nullptr,                                   // tp_as_number
    nullptr,                                   // tp_as_sequence
    nullptr,                                   // tp_as_mapping
    nullptr,                                   // tp_hash
    nullptr,                                   // tp_call
    nullptr,                                   // tp_str
    nullptr,                                   // tp_getattro
    nullptr,                                   // tp_setattro
    nullptr,                                   // tp_as_buffer
    Py_TPFLAGS_DEFAULT,                        // tp_flags
    "非同期パイプ I/O ハンドル (Phase 2 native backend)",  // tp_doc
    nullptr,                                   // tp_traverse
    nullptr,                                   // tp_clear
    nullptr,                                   // tp_richcompare
    0,                                         // tp_weaklistoffset
    nullptr,                                   // tp_iter
    nullptr,                                   // tp_iternext
    PyAsyncPipeHandle_methods,                 // tp_methods
    nullptr,                                   // tp_members
    PyAsyncPipeHandle_getset,                  // tp_getset
    nullptr,                                   // tp_base
    nullptr,                                   // tp_dict
    nullptr,                                   // tp_descr_get
    nullptr,                                   // tp_descr_set
    0,                                         // tp_dictoffset
    reinterpret_cast<initproc>(PyAsyncPipeHandle_init), // tp_init
    nullptr,                                   // tp_alloc
    PyAsyncPipeHandle_new,                     // tp_new
};

// ──────────────────────────────────────────────────────────────────────────────
// モジュールレベル関数
// ──────────────────────────────────────────────────────────────────────────────

// _fire_future: schedule_fire_success / schedule_fire_error から
//              call_soon_threadsafe でスケジュールされる。asyncio スレッド上で実行。
static PyObject*
mod_fire_future(PyObject* module, PyObject* args) {
    return fire_future_impl(module, args);
}

// Linux 用: fd が readable になったときに event loop が呼ぶ
static PyObject*
mod_on_linux_readable(PyObject* /*module*/, PyObject* cap) {
#ifndef _WIN32
    if (!PyCapsule_CheckExact(cap)) {
        PyErr_SetString(PyExc_TypeError, "expected capsule");
        return nullptr;
    }
    auto* ctx = static_cast<pipeutil::detail::LinuxReadCtx_fwd*>(
        PyCapsule_GetPointer(cap, "lrc"));
    // 実装は py_async_pipe.cpp の anonymous namespace 内にあるが
    // extern でなく module 関数経由で呼ぶ構造につき、
    // ここから直接呼び出しは不可。
    // NOTE: Linux 側の readable ハンドラは将来のリリースで完全実装予定。
    // 現在は概念実証として future を cancel 状態のまま通知する。
    PyErr_SetString(PyExc_NotImplementedError,
                    "_on_linux_readable: full implementation pending");
    return nullptr;
#else
    (void)cap;
    Py_RETURN_NONE;
#endif
}

// Linux 用: fd が writable になったときに event loop が呼ぶ
static PyObject*
mod_on_linux_writable(PyObject* /*module*/, PyObject* cap) {
#ifndef _WIN32
    (void)cap;
    PyErr_SetString(PyExc_NotImplementedError,
                    "_on_linux_writable: full implementation pending");
    return nullptr;
#else
    (void)cap;
    Py_RETURN_NONE;
#endif
}

// ─── モジュールメソッドテーブル ──────────────────────────────────────────────

static PyMethodDef module_methods[] = {
    {"_fire_future",       mod_fire_future,        METH_VARARGS, nullptr},
    {"_on_linux_readable", mod_on_linux_readable,  METH_O,       nullptr},
    {"_on_linux_writable", mod_on_linux_writable,  METH_O,       nullptr},
    {nullptr, nullptr, 0, nullptr},
};

// ──────────────────────────────────────────────────────────────────────────────
// PyInit__pipeutil_async
// ──────────────────────────────────────────────────────────────────────────────

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "_pipeutil_async",
    "pipeutil Phase 2 ネイティブ非同期 I/O バックエンド",
    -1,
    module_methods,
};

PyMODINIT_FUNC
PyInit__pipeutil_async(void) {
    // ─── AsyncPipeHandle 型を確定 ─────────────────────────────────────
    if (PyType_Ready(&PyAsyncPipeHandle_Type) < 0) return nullptr;

    // ─── モジュール作成 ───────────────────────────────────────────────
    PyObject* m = PyModule_Create(&module_def);
    if (!m) return nullptr;

    // ─── TooManyConnectionsError 登録 ────────────────────────────────
    // _pipeutil.PipeError の派生として作成
    const char* base_name = nullptr;
    PyObject* base_mod = PyImport_ImportModule("pipeutil._pipeutil");
    PyObject* base_exc = nullptr;
    if (base_mod) {
        base_exc = PyObject_GetAttrString(base_mod, "PipeError");
        Py_DECREF(base_mod);
    }
    if (!base_exc) {
        PyErr_Clear();
        base_exc = PyExc_Exception;
        Py_INCREF(base_exc);
    }
    (void)base_name;

    PyObject* too_many = PyErr_NewException(
        "_pipeutil_async.TooManyConnectionsError", base_exc, nullptr);
    Py_DECREF(base_exc);
    if (!too_many) { Py_DECREF(m); return nullptr; }
    if (PyModule_AddObjectRef(m, "TooManyConnectionsError", too_many) < 0) {
        Py_DECREF(too_many);
        Py_DECREF(m);
        return nullptr;
    }
    Py_DECREF(too_many);

    // ─── AsyncPipeHandle 型をモジュールに追加 ─────────────────────────
    Py_INCREF(&PyAsyncPipeHandle_Type);
    if (PyModule_AddObject(m, "AsyncPipeHandle",
                           reinterpret_cast<PyObject*>(&PyAsyncPipeHandle_Type)) < 0) {
        Py_DECREF(&PyAsyncPipeHandle_Type);
        Py_DECREF(m);
        return nullptr;
    }

    // ─── グローバル変数初期化（interned strings, 例外型参照など） ─────
    if (init_async_globals(m) < 0) {
        Py_DECREF(m);
        return nullptr;
    }

    return m;
}
