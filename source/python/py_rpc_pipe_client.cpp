// py_rpc_pipe_client.cpp — PyRpcPipeClient 型実装
// 仕様: spec/F002_rpc_message_id.md §8
//
// GIL ポリシー:
//   connect / close / send / receive / send_request:
//     ブロッキング I/O のため GIL を解放する (Py_BEGIN/END_ALLOW_THREADS)。

#define PY_SSIZE_T_CLEAN
#include "py_rpc_pipe_client.hpp"
#include "py_exceptions.hpp"
#include "py_message.hpp"
#include <chrono>

namespace pyutil {

// ─── 内部ヘルパー ──────────────────────────────────────────────────────────

static inline std::chrono::milliseconds to_ms(double sec) noexcept {
    return (sec <= 0.0)
        ? std::chrono::milliseconds{0}
        : std::chrono::milliseconds{static_cast<int64_t>(sec * 1000.0 + 0.5)};
}

static bool check_client(PyRpcPipeClient* self) noexcept {
    if (!self->client) {
        PyErr_SetString(PyExc_RuntimeError,
                        "RpcPipeClient is closed or not initialized");
        return false;
    }
    return true;
}

// ─── tp_new / tp_init / tp_dealloc ───────────────────────────────────────

static PyObject* PyRpcPipeClient_new(PyTypeObject* type,
                                     PyObject* /*args*/,
                                     PyObject* /*kwds*/) {
    auto* self = reinterpret_cast<PyRpcPipeClient*>(type->tp_alloc(type, 0));
    if (self) self->client = nullptr;
    return reinterpret_cast<PyObject*>(self);
}

static int PyRpcPipeClient_init(PyRpcPipeClient* self,
                                PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"pipe_name", "buffer_size", nullptr};
    const char* pipe_name  = nullptr;
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
        delete self->client;
        self->client = new pipeutil::RpcPipeClient{
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

static void PyRpcPipeClient_dealloc(PyRpcPipeClient* self) {
    delete self->client;
    self->client = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// ─── connect ─────────────────────────────────────────────────────────────

static PyObject* PyRpcPipeClient_connect(PyRpcPipeClient* self,
                                          PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"timeout", nullptr};
    double timeout_sec = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|d",
                                     const_cast<char**>(kwlist), &timeout_sec)) {
        return nullptr;
    }
    if (!check_client(self)) return nullptr;

    auto timeout_ms = to_ms(timeout_sec);
    pipeutil::PipeException* pending = nullptr;

    Py_BEGIN_ALLOW_THREADS
    try { self->client->connect(timeout_ms); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    Py_RETURN_NONE;
}

// ─── close ───────────────────────────────────────────────────────────────

static PyObject* PyRpcPipeClient_close(PyRpcPipeClient* self, PyObject*) {
    if (!check_client(self)) return nullptr;

    pipeutil::PipeException* pending = nullptr;
    Py_BEGIN_ALLOW_THREADS
    try { self->client->close(); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    Py_RETURN_NONE;
}

// ─── send ────────────────────────────────────────────────────────────────

static PyObject* PyRpcPipeClient_send(PyRpcPipeClient* self, PyObject* args) {
    PyObject* obj = nullptr;
    if (!PyArg_ParseTuple(args, "O", &obj)) return nullptr;
    if (!check_client(self)) return nullptr;

    pipeutil::Message msg{};
    if (!PyMessage_convert(obj, msg)) return nullptr;

    pipeutil::PipeException* pending = nullptr;
    Py_BEGIN_ALLOW_THREADS
    try { self->client->send(msg); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    Py_RETURN_NONE;
}

// ─── receive ─────────────────────────────────────────────────────────────

static PyObject* PyRpcPipeClient_receive(PyRpcPipeClient* self,
                                          PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"timeout", nullptr};
    double timeout_sec = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|d",
                                     const_cast<char**>(kwlist), &timeout_sec)) {
        return nullptr;
    }
    if (!check_client(self)) return nullptr;

    auto timeout_ms = to_ms(timeout_sec);
    pipeutil::Message result_msg{};
    pipeutil::PipeException* pending = nullptr;

    Py_BEGIN_ALLOW_THREADS
    try { result_msg = self->client->receive(timeout_ms); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    return PyMessage_from_message(result_msg);
}

// ─── send_request ────────────────────────────────────────────────────────

static PyObject* PyRpcPipeClient_send_request(PyRpcPipeClient* self,
                                               PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"message", "timeout", nullptr};
    PyObject* obj          = nullptr;
    double    timeout_sec  = 5.0;   // デフォルト 5 秒

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|d",
                                     const_cast<char**>(kwlist),
                                     &obj, &timeout_sec)) {
        return nullptr;
    }
    if (!check_client(self)) return nullptr;

    pipeutil::Message req{};
    if (!PyMessage_convert(obj, req)) return nullptr;

    auto timeout_ms = to_ms(timeout_sec);
    pipeutil::Message resp{};
    pipeutil::PipeException* pending = nullptr;

    Py_BEGIN_ALLOW_THREADS
    try { resp = self->client->send_request(req, timeout_ms); }
    catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS

    if (pending) { set_python_exception(*pending); delete pending; return nullptr; }
    return PyMessage_from_message(resp);
}

// ─── is_connected ────────────────────────────────────────────────────────

static PyObject* PyRpcPipeClient_is_connected(PyRpcPipeClient* self, PyObject*) {
    if (!check_client(self)) return nullptr;
    return PyBool_FromLong(static_cast<long>(self->client->is_connected()));
}

// ─── pipe_name ───────────────────────────────────────────────────────────

static PyObject* PyRpcPipeClient_pipe_name(PyRpcPipeClient* self, PyObject*) {
    if (!check_client(self)) return nullptr;
    return PyUnicode_FromString(self->client->pipe_name().c_str());
}

// ─── メソッドテーブル ─────────────────────────────────────────────────────

static PyMethodDef PyRpcPipeClient_methods[] = {
    {"connect",      reinterpret_cast<PyCFunction>(PyRpcPipeClient_connect),
     METH_VARARGS | METH_KEYWORDS, "connect(timeout=0.0)"},
    {"close",        reinterpret_cast<PyCFunction>(PyRpcPipeClient_close),
     METH_NOARGS, "close()"},
    {"send",         reinterpret_cast<PyCFunction>(PyRpcPipeClient_send),
     METH_VARARGS, "send(message)"},
    {"receive",      reinterpret_cast<PyCFunction>(PyRpcPipeClient_receive),
     METH_VARARGS | METH_KEYWORDS, "receive(timeout=0.0)"},
    {"send_request", reinterpret_cast<PyCFunction>(PyRpcPipeClient_send_request),
     METH_VARARGS | METH_KEYWORDS, "send_request(message, timeout=5.0)"},
    {"is_connected", reinterpret_cast<PyCFunction>(PyRpcPipeClient_is_connected),
     METH_NOARGS, "is_connected() -> bool"},
    {"pipe_name",    reinterpret_cast<PyCFunction>(PyRpcPipeClient_pipe_name),
     METH_NOARGS, "pipe_name() -> str"},
    {nullptr}
};

// ─── PyTypeObject ────────────────────────────────────────────────────────

PyTypeObject PyRpcPipeClient_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "pipeutil.RpcPipeClient",           // tp_name
    sizeof(PyRpcPipeClient),            // tp_basicsize
    0,                                  // tp_itemsize
    reinterpret_cast<destructor>(PyRpcPipeClient_dealloc), // tp_dealloc
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // skipped
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   // tp_flags
    "RpcPipeClient(pipe_name, buffer_size=65536)\n\n"
    "RPC 対応パイプクライアント。send_request() でリクエスト/レスポンス型通信を行う。", // tp_doc
    0, 0, 0, 0, 0, 0,
    PyRpcPipeClient_methods,            // tp_methods
    0, 0, 0, 0, 0, 0, 0,
    reinterpret_cast<initproc>(PyRpcPipeClient_init),   // tp_init
    0,
    PyRpcPipeClient_new,                // tp_new
};

} // namespace pyutil
