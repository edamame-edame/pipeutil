// py_multi_pipe_server.cpp — PyMultiPipeServer 型実装
// 仕様: spec/F001_multi_pipe_server.md §5
//
// GIL ポリシー:
//   serve()  : ブロッキング呼び出しのため GIL を解放する。
//   stop()   : ブロッキング（handler 完了待ち）のため GIL を解放する。
//   handler  : C++ worker スレッドから呼ばれるため PyGILState_Ensure() で GIL を再取得する。

#include "py_multi_pipe_server.hpp"
#include "py_pipe_server.hpp"       // PyPipeServer / PyPipeServer_Type
#include "py_exceptions.hpp"
#include "py_pipe_stats.hpp"
#include <stdexcept>

namespace pyutil {

// ─── helper: PipeServer を Python オブジェクトに包む ──────────────────────
// GIL 保持状態で呼ぶこと。
static PyObject* wrap_pipe_server(pipeutil::PipeServer conn) {
    PyObject* obj = PyPipeServer_Type.tp_alloc(&PyPipeServer_Type, 0);
    if (!obj) return nullptr;
    auto* ps = reinterpret_cast<PyPipeServer*>(obj);
    try {
        ps->server = new pipeutil::PipeServer{std::move(conn)};
    } catch (...) {
        Py_DECREF(obj);
        PyErr_NoMemory();
        return nullptr;
    }
    return obj;
}

// ─── helper: server が有効か確認 ──────────────────────────────────────────
static bool check_server(PyMultiPipeServer* self) noexcept {
    if (!self->server) {
        PyErr_SetString(PyExc_RuntimeError,
                        "MultiPipeServer is not initialized");
        return false;
    }
    return true;
}

// ─── tp_new / tp_init / tp_dealloc ────────────────────────────────────────

static PyObject* PyMultiPipeServer_new(PyTypeObject* type,
                                       PyObject* /*args*/,
                                       PyObject* /*kwds*/) {
    PyMultiPipeServer* self =
        reinterpret_cast<PyMultiPipeServer*>(type->tp_alloc(type, 0));
    if (self) self->server = nullptr;
    return reinterpret_cast<PyObject*>(self);
}

static int PyMultiPipeServer_init(PyMultiPipeServer* self,
                                   PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {
        "pipe_name", "max_connections", "buffer_size", nullptr
    };
    const char* pipe_name     = nullptr;
    Py_ssize_t  max_conn      = 8;
    Py_ssize_t  buffer_size   = 65536;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|nn",
                                     const_cast<char**>(kwlist),
                                     &pipe_name, &max_conn, &buffer_size)) {
        return -1;
    }
    if (max_conn < 1 || max_conn > 64) {
        PyErr_SetString(PyExc_ValueError,
                        "max_connections must be between 1 and 64");
        return -1;
    }
    if (buffer_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "buffer_size must be positive");
        return -1;
    }

    try {
        delete self->server;
        self->server = new pipeutil::MultiPipeServer{
            std::string{pipe_name},
            static_cast<std::size_t>(max_conn),
            static_cast<std::size_t>(buffer_size)
        };
    } catch (const pipeutil::PipeException& e) {
        set_python_exception(e);
        return -1;
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return -1;
    }
    return 0;
}

static void PyMultiPipeServer_dealloc(PyMultiPipeServer* self) {
    delete self->server;
    self->server = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// ─── serve() ──────────────────────────────────────────────────────────────

static PyObject* PyMultiPipeServer_serve(PyMultiPipeServer* self, PyObject* args) {
    PyObject* py_handler = nullptr;
    if (!PyArg_ParseTuple(args, "O", &py_handler)) return nullptr;
    if (!PyCallable_Check(py_handler)) {
        PyErr_SetString(PyExc_TypeError, "handler must be callable");
        return nullptr;
    }
    if (!check_server(self)) return nullptr;

    // ハンドラのライフタイムを serve() の間だけ延長する
    Py_INCREF(py_handler);

    pipeutil::PipeException* pending = nullptr;

    Py_BEGIN_ALLOW_THREADS
    try {
        self->server->serve([py_handler](pipeutil::PipeServer conn) {
            // worker スレッドから呼ばれる → GIL を取得してから Python を触る
            const PyGILState_STATE gstate = PyGILState_Ensure();

            PyObject* py_conn = wrap_pipe_server(std::move(conn));
            if (py_conn) {
                PyObject* result = PyObject_CallOneArg(py_handler, py_conn);
                Py_XDECREF(result);
                Py_DECREF(py_conn);
                // ハンドラ内例外は握り潰す（スレッドをクリーンに終了させる）
                if (!result) PyErr_Clear();
            } else {
                PyErr_Clear();
            }

            PyGILState_Release(gstate);
        });
    } catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    } catch (const std::exception& e) {
        // PipeException 以外の例外を Python RuntimeError に変換
        pending = new pipeutil::PipeException{pipeutil::PipeErrorCode::SystemError, e.what()};
    }
    Py_END_ALLOW_THREADS

    Py_DECREF(py_handler);

    if (pending) {
        set_python_exception(*pending);
        delete pending;
        return nullptr;
    }
    Py_RETURN_NONE;
}

// ─── stop() ───────────────────────────────────────────────────────────────

static PyObject* PyMultiPipeServer_stop(PyMultiPipeServer* self, PyObject* /*args*/) {
    if (!self->server) Py_RETURN_NONE;

    Py_BEGIN_ALLOW_THREADS
    self->server->stop();
    Py_END_ALLOW_THREADS

    Py_RETURN_NONE;
}

// ─── コンテキストマネージャ ────────────────────────────────────────────────

static PyObject* PyMultiPipeServer_enter(PyMultiPipeServer* self, PyObject* /*args*/) {
    Py_INCREF(self);
    return reinterpret_cast<PyObject*>(self);
}

static PyObject* PyMultiPipeServer_exit(PyMultiPipeServer* self, PyObject* /*args*/) {
    return PyMultiPipeServer_stop(self, nullptr);
}

// ─── 診断・メトリクス (F-006) ─────────────────────────────────

static PyObject* PyMultiPipeServer_stats(PyMultiPipeServer* self, PyObject* /*args*/) {
    if (!self->server) return PyPipeStats_from_stats(pipeutil::PipeStats{});
    return PyPipeStats_from_stats(self->server->stats());
}

static PyObject* PyMultiPipeServer_reset_stats(PyMultiPipeServer* self, PyObject* /*args*/) {
    if (self->server) self->server->reset_stats();
    Py_RETURN_NONE;
}

// ─── プロパティ ───────────────────────────────────────────────────────────

static PyObject* PyMultiPipeServer_get_is_serving(PyMultiPipeServer* self, void*) {
    if (!self->server) Py_RETURN_FALSE;
    return PyBool_FromLong(self->server->is_serving() ? 1 : 0);
}

static PyObject* PyMultiPipeServer_get_active_connections(PyMultiPipeServer* self, void*) {
    if (!self->server) return PyLong_FromLong(0);
    return PyLong_FromSize_t(self->server->active_connections());
}

static PyObject* PyMultiPipeServer_get_pipe_name(PyMultiPipeServer* self, void*) {
    if (!self->server) {
        PyErr_SetString(PyExc_RuntimeError, "MultiPipeServer is not initialized");
        return nullptr;
    }
    return PyUnicode_FromString(self->server->pipe_name().c_str());
}

static PyGetSetDef PyMultiPipeServer_getset[] = {
    {"is_serving",
     reinterpret_cast<getter>(PyMultiPipeServer_get_is_serving),
     nullptr, "True after serve() has started and before stop()", nullptr},
    {"active_connections",
     reinterpret_cast<getter>(PyMultiPipeServer_get_active_connections),
     nullptr, "Number of currently active handler threads", nullptr},
    {"pipe_name",
     reinterpret_cast<getter>(PyMultiPipeServer_get_pipe_name),
     nullptr, "Pipe identifier name", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}
};

// ─── メソッドテーブル ─────────────────────────────────────────────────────

static PyMethodDef PyMultiPipeServer_methods[] = {
    {"serve",     reinterpret_cast<PyCFunction>(PyMultiPipeServer_serve),
     METH_VARARGS,
     "serve(handler: Callable[[PipeServer], None]) -> None\n\n"
     "Start accepting connections and dispatch each to handler in a new thread.\n"
     "Blocks until stop() is called from another thread."},
    {"stop",      reinterpret_cast<PyCFunction>(PyMultiPipeServer_stop),
     METH_NOARGS,
     "stop() -> None\n\nStop the server and wait for all handlers to finish."},
    {"__enter__", reinterpret_cast<PyCFunction>(PyMultiPipeServer_enter),
     METH_NOARGS, "Context manager entry."},
    {"__exit__",  reinterpret_cast<PyCFunction>(PyMultiPipeServer_exit),
     METH_VARARGS, "Context manager exit (calls stop)."},
    {"stats",       reinterpret_cast<PyCFunction>(PyMultiPipeServer_stats),
     METH_NOARGS, "stats() -> PipeStats\nReturn aggregated diagnostics snapshot."},
    {"reset_stats", reinterpret_cast<PyCFunction>(PyMultiPipeServer_reset_stats),
     METH_NOARGS, "reset_stats() -> None\nReset all session counters to 0."},
    {nullptr, nullptr, 0, nullptr}
};

// ─── PyTypeObject ─────────────────────────────────────────────────────────

PyTypeObject PyMultiPipeServer_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "_pipeutil.MultiPipeServer",
    sizeof(PyMultiPipeServer),
    0,
    reinterpret_cast<destructor>(PyMultiPipeServer_dealloc),
    0, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    "MultiPipeServer(pipe_name: str, max_connections: int = 8, buffer_size: int = 65536)\n\n"
    "Multi-client pipe server. Dispatches each connection to handler in a thread.\n\n"
    "Usage::\n\n"
    "    srv = MultiPipeServer('my_pipe', max_connections=4)\n"
    "    threading.Thread(target=srv.serve, args=[handler]).start()\n"
    "    # ... later ...\n"
    "    srv.stop()\n",
    nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    PyMultiPipeServer_methods,
    nullptr,
    PyMultiPipeServer_getset,
    nullptr, nullptr, nullptr, nullptr, 0,
    reinterpret_cast<initproc>(PyMultiPipeServer_init),
    nullptr,
    PyMultiPipeServer_new,
};

} // namespace pyutil
