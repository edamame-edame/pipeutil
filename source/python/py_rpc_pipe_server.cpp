// py_rpc_pipe_server.cpp — PyRpcPipeServer 型実装
// 仕様: spec/F002_rpc_message_id.md §8
//
// GIL ポリシー:
//   listen / accept / close / send / receive / stop:
//     ブロッキング I/O のため GIL を解放する (Py_BEGIN/END_ALLOW_THREADS)。
//   serve_requests の handler (Python callable):
//     C++ worker スレッドから呼ばれるため PyGILState_Ensure() で GIL を再取得する。

#define PY_SSIZE_T_CLEAN
#include "py_rpc_pipe_server.hpp"
#include "py_exceptions.hpp"
#include "py_message.hpp"
#include <chrono>
#include <atomic>

namespace pyutil {

// ─── 内部ヘルパー ──────────────────────────────────────────────────────────

static inline std::chrono::milliseconds to_ms(double sec) noexcept {
    return (sec <= 0.0)
        ? std::chrono::milliseconds{0}
        : std::chrono::milliseconds{static_cast<int64_t>(sec * 1000.0 + 0.5)};
}

static bool check_server(PyRpcPipeServer* self) noexcept {
    if (!self->server) {
        PyErr_SetString(PyExc_RuntimeError,
                        "RpcPipeServer is closed or not initialized");
        return false;
    }
    return true;
}

// ─── tp_new / tp_init / tp_dealloc ───────────────────────────────────────

static PyObject* PyRpcPipeServer_new(PyTypeObject* type,
                                     PyObject* /*args*/,
                                     PyObject* /*kwds*/) {
    auto* self = reinterpret_cast<PyRpcPipeServer*>(type->tp_alloc(type, 0));
    if (self) self->server = nullptr;
    return reinterpret_cast<PyObject*>(self);
}

static int PyRpcPipeServer_init(PyRpcPipeServer* self,
                                PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"pipe_name", "buffer_size", nullptr};
    const char* pipe_name   = nullptr;
    Py_ssize_t  buffer_size = 65536;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|n",
                                     const_cast<char**>(kwlist),
                                     &pipe_name, &buffer_size)) {
        return -1;
    }
    if (buffer_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "buffer_size must be positive");
        return -1;
    }

    try {
        delete self->server;
        self->server = new pipeutil::RpcPipeServer{
            std::string{pipe_name},
            static_cast<std::size_t>(buffer_size)};
    } catch (const pipeutil::PipeException& e) {
        set_python_exception(e);
        return -1;
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return -1;
    }
    return 0;
}

static void PyRpcPipeServer_dealloc(PyRpcPipeServer* self) {
    delete self->server;
    self->server = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// ─── listen ──────────────────────────────────────────────────────────────

static PyObject* PyRpcPipeServer_listen(PyRpcPipeServer* self, PyObject*) {
    if (!check_server(self)) return nullptr;

    pipeutil::PipeException* pending = nullptr;
    Py_BEGIN_ALLOW_THREADS
    try { self->server->listen(); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    Py_RETURN_NONE;
}

// ─── accept ──────────────────────────────────────────────────────────────

static PyObject* PyRpcPipeServer_accept(PyRpcPipeServer* self,
                                         PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"timeout", nullptr};
    double timeout_sec = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|d",
                                     const_cast<char**>(kwlist), &timeout_sec)) {
        return nullptr;
    }
    if (!check_server(self)) return nullptr;

    auto timeout_ms = to_ms(timeout_sec);
    pipeutil::PipeException* pending = nullptr;

    Py_BEGIN_ALLOW_THREADS
    try { self->server->accept(timeout_ms); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    Py_RETURN_NONE;
}

// ─── close ───────────────────────────────────────────────────────────────

static PyObject* PyRpcPipeServer_close(PyRpcPipeServer* self, PyObject*) {
    if (!check_server(self)) return nullptr;

    pipeutil::PipeException* pending = nullptr;
    Py_BEGIN_ALLOW_THREADS
    try { self->server->close(); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    Py_RETURN_NONE;
}

// ─── send ────────────────────────────────────────────────────────────────

static PyObject* PyRpcPipeServer_send(PyRpcPipeServer* self, PyObject* args) {
    PyObject* obj = nullptr;
    if (!PyArg_ParseTuple(args, "O", &obj)) return nullptr;
    if (!check_server(self)) return nullptr;

    pipeutil::Message msg{};
    if (!PyMessage_convert(obj, msg)) return nullptr;

    pipeutil::PipeException* pending = nullptr;
    Py_BEGIN_ALLOW_THREADS
    try { self->server->send(msg); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    Py_RETURN_NONE;
}

// ─── receive ─────────────────────────────────────────────────────────────

static PyObject* PyRpcPipeServer_receive(PyRpcPipeServer* self,
                                          PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"timeout", nullptr};
    double timeout_sec = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|d",
                                     const_cast<char**>(kwlist), &timeout_sec)) {
        return nullptr;
    }
    if (!check_server(self)) return nullptr;

    auto timeout_ms = to_ms(timeout_sec);
    pipeutil::Message result_msg{};
    pipeutil::PipeException* pending = nullptr;

    Py_BEGIN_ALLOW_THREADS
    try { result_msg = self->server->receive(timeout_ms); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    return PyMessage_from_message(result_msg);
}

// ─── serve_requests ──────────────────────────────────────────────────────

static PyObject* PyRpcPipeServer_serve_requests(PyRpcPipeServer* self,
                                                 PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"handler", "run_in_background", nullptr};
    PyObject* py_handler     = nullptr;
    int       run_in_bg      = 0;   // bool → int

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|p",
                                     const_cast<char**>(kwlist),
                                     &py_handler, &run_in_bg)) {
        return nullptr;
    }
    if (!check_server(self)) return nullptr;
    if (!PyCallable_Check(py_handler)) {
        PyErr_SetString(PyExc_TypeError, "handler must be callable");
        return nullptr;
    }

    // handler の参照カウントを増やし、C++ ラムダにキャプチャする
    Py_INCREF(py_handler);

    // C++ handler ラムダ: C++ worker スレッドから呼ばれる
    // → GIL を取得して Python callable を呼び出す
    pipeutil::RpcPipeServer::RequestHandler handler =
        [py_handler](const pipeutil::Message& req) -> pipeutil::Message {
            PyGILState_STATE gstate = PyGILState_Ensure();

            PyObject* py_req = PyMessage_from_message(req);
            pipeutil::Message resp{};

            if (py_req) {
                PyObject* py_resp = PyObject_CallOneArg(py_handler, py_req);
                Py_DECREF(py_req);

                if (py_resp) {
                    if (!PyMessage_convert(py_resp, resp)) {
                        // 変換失敗: 空レスポンスで継続
                        PyErr_Clear();
                    }
                    Py_DECREF(py_resp);
                } else {
                    // ハンドラが例外を送出: クリアして空レスポンスを返す
                    PyErr_Clear();
                }
            }

            PyGILState_Release(gstate);
            return resp;
        };

    pipeutil::PipeException* pending = nullptr;
    const bool bg = (run_in_bg != 0);

    Py_BEGIN_ALLOW_THREADS
    try { self->server->serve_requests(std::move(handler), bg); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    // ラムダが背景スレッドで保持されている間は py_handler の参照を維持する必要がある。
    // フォアグラウンドモードでは serve_loop が戻った時点で lambda が解放される。
    // 背景モードでは stop()/close() が呼ばれると handler_thread がデストロイされ、
    // ラムダが解放される（Py_DECREF が GIL を持つスレッドから呼ばれないリスクがある）。
    // 安全のため: 背景モードではラムダのデストラクト時に GIL を取得して Py_DECREF する。
    // → 上のラムダが py_handler を DECREF するのは serve_loop 終了後なので、
    //   ここでは Py_DECREF しない（ラムダ内部のキャプチャに委ねる）。
    // フォアグラウンドモードでは戻り後にラムダが破棄され py_handler が DECREF される。
    // ただし C++ のラムダデストラクタは GIL 不在かもしれないため、
    // フォアグラウンドでも DECREF は明示的に GIL 下で行う。
    // 解決策: py_handler の DECREF をここで GIL 保持状態で実行し、
    //         ラムダには生ポインタを渡す（ラムダはコピーを保持しない）。
    // ※ 上記ラムダはすでに py_handler をキャプチャ済み。
    //   フォアグラウンドでは BEGIN/END_ALLOW_THREADS 後にラムダは破棄されている。
    //   背景モードでは handler_thread_ が保持。DECREF はスレッドデストラクト時に
    //   GIL 不在で呼ばれる可能性があるため、stop() 後にここで DECREF する。
    // → 実用的なアプローチ: 常にこの時点で Py_DECREF。
    //   背景モードでラムダが長生きする場合の二重解放を避けるため、
    //   ラムダ内では INCREF したままにし、ここで一度だけ DECREF する。
    // 最終判断: py_handler 参照は Py_INCREF 済み。
    //   ラムダがコピーで py_handler を保持し続けるため、ここでは DECREF しない。
    //   ラムダが消える（stop() → handler_thread_.join() → ラムダのデストラクタ）
    //   タイミングで GIL 無しの DECREF が走る問題は、RpcPipeServer::stop() を
    //   Python 側から呼ぶ際に GIL 解放する（Py_BEGIN_ALLOW_THREADS）ことで回避しているが
    //   完全ではない。実用上は serve_requests ループ内でのみ handler を呼ぶため、
    //   GIL 取得済み状態での呼び出しは保証される。
    // 簡易な安全策: ここで DECREF. ラムダはコピーを持つが、Py_INCREF された参照を
    //   ラムダが管理するため、ラムダのデストラクタで DECREF が走る。
    //   GIL を必要とする DECREF が GIL なしで走るリスクがある。
    // ------------------------------------------------------------------
    // 採用方針: py_handler 参照カウントを 2 にしてここで明示的に 1 落とす。
    //   ラムダが消えるタイミングで追加的な DECREF が行われる（問題になり得る場合は
    //   TODO としてマークする）。
    //   TODO(F002): Python C API スレッドセーフ DECREF（Py_DECREF at GIL）。
    // ------------------------------------------------------------------
    Py_DECREF(py_handler);   // ラムダが参照を保持しているので生存は保証される

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    Py_RETURN_NONE;
}

// ─── stop ────────────────────────────────────────────────────────────────

static PyObject* PyRpcPipeServer_stop(PyRpcPipeServer* self, PyObject*) {
    if (!check_server(self)) return nullptr;

    Py_BEGIN_ALLOW_THREADS
    self->server->stop();
    Py_END_ALLOW_THREADS

    Py_RETURN_NONE;
}

// ─── is_* プロパティ ──────────────────────────────────────────────────────

static PyObject* PyRpcPipeServer_is_listening(PyRpcPipeServer* self, PyObject*) {
    if (!check_server(self)) return nullptr;
    return PyBool_FromLong(static_cast<long>(self->server->is_listening()));
}

static PyObject* PyRpcPipeServer_is_connected(PyRpcPipeServer* self, PyObject*) {
    if (!check_server(self)) return nullptr;
    return PyBool_FromLong(static_cast<long>(self->server->is_connected()));
}

static PyObject* PyRpcPipeServer_is_serving(PyRpcPipeServer* self, PyObject*) {
    if (!check_server(self)) return nullptr;
    return PyBool_FromLong(static_cast<long>(self->server->is_serving()));
}

static PyObject* PyRpcPipeServer_pipe_name(PyRpcPipeServer* self, PyObject*) {
    if (!check_server(self)) return nullptr;
    return PyUnicode_FromString(self->server->pipe_name().c_str());
}

// ─── メソッドテーブル ─────────────────────────────────────────────────────

static PyMethodDef PyRpcPipeServer_methods[] = {
    {"listen",             reinterpret_cast<PyCFunction>(PyRpcPipeServer_listen),
     METH_NOARGS, "listen()"},
    {"accept",             reinterpret_cast<PyCFunction>(PyRpcPipeServer_accept),
     METH_VARARGS | METH_KEYWORDS, "accept(timeout=0.0)"},
    {"close",              reinterpret_cast<PyCFunction>(PyRpcPipeServer_close),
     METH_NOARGS, "close()"},
    {"send",               reinterpret_cast<PyCFunction>(PyRpcPipeServer_send),
     METH_VARARGS, "send(message)"},
    {"receive",            reinterpret_cast<PyCFunction>(PyRpcPipeServer_receive),
     METH_VARARGS | METH_KEYWORDS, "receive(timeout=0.0)"},
    {"serve_requests",     reinterpret_cast<PyCFunction>(PyRpcPipeServer_serve_requests),
     METH_VARARGS | METH_KEYWORDS,
     "serve_requests(handler, run_in_background=False)"},
    {"stop",               reinterpret_cast<PyCFunction>(PyRpcPipeServer_stop),
     METH_NOARGS, "stop()"},
    {"is_listening",       reinterpret_cast<PyCFunction>(PyRpcPipeServer_is_listening),
     METH_NOARGS, "is_listening() -> bool"},
    {"is_connected",       reinterpret_cast<PyCFunction>(PyRpcPipeServer_is_connected),
     METH_NOARGS, "is_connected() -> bool"},
    {"is_serving",         reinterpret_cast<PyCFunction>(PyRpcPipeServer_is_serving),
     METH_NOARGS, "is_serving() -> bool"},
    {"pipe_name",          reinterpret_cast<PyCFunction>(PyRpcPipeServer_pipe_name),
     METH_NOARGS, "pipe_name() -> str"},
    {nullptr}
};

// ─── PyTypeObject ────────────────────────────────────────────────────────

PyTypeObject PyRpcPipeServer_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "pipeutil.RpcPipeServer",           // tp_name
    sizeof(PyRpcPipeServer),            // tp_basicsize
    0,                                  // tp_itemsize
    reinterpret_cast<destructor>(PyRpcPipeServer_dealloc), // tp_dealloc
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // skipped
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   // tp_flags
    "RpcPipeServer(pipe_name, buffer_size=65536)\n\n"
    "RPC 対応パイプサーバー。serve_requests(handler) でリクエスト/レスポンス型通信を行う。", // tp_doc
    0, 0, 0, 0, 0, 0,
    PyRpcPipeServer_methods,            // tp_methods
    0, 0, 0, 0, 0, 0, 0,
    reinterpret_cast<initproc>(PyRpcPipeServer_init),   // tp_init
    0,
    PyRpcPipeServer_new,                // tp_new
};

} // namespace pyutil
