// py_pipe_client.cpp — PyPipeClient 型実装
// 仕様: spec/04_python_wrapper.md §6
// GIL: connect / send / receive はブロッキング I/O のため GIL を解放する。

#include "py_pipe_client.hpp"
#include "py_exceptions.hpp"
#include "py_message.hpp"
#include "py_pipe_stats.hpp"
#include "py_debug_log.hpp"
#include <chrono>

namespace pyutil {

// タイムアウト値はミリ秒単位で受け取る（Python API 規約: timeout_msec 整数 or float）
static std::chrono::milliseconds to_ms_c(double ms_val) noexcept {
    if (ms_val <= 0.0) return std::chrono::milliseconds{0};
    return std::chrono::milliseconds{static_cast<int64_t>(ms_val)};
}

static bool check_client(PyPipeClient* self) noexcept {
    if (!self->client) {
        PyErr_SetString(PyExc_RuntimeError, "PipeClient is closed or not initialized");
        return false;
    }
    return true;
}

// ─── tp_new / tp_init / tp_dealloc ────────────────────────────────────

static PyObject* PyPipeClient_new(PyTypeObject* type,
                                  PyObject* /*args*/,
                                  PyObject* /*kwds*/) {
    PyPipeClient* self = reinterpret_cast<PyPipeClient*>(type->tp_alloc(type, 0));
    if (self) self->client = nullptr;
    return reinterpret_cast<PyObject*>(self);
}

static int PyPipeClient_init(PyPipeClient* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"pipe_name", "buffer_size", nullptr};
    const char* pipe_name = nullptr;
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
        self->client = new pipeutil::PipeClient{
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

static void PyPipeClient_dealloc(PyPipeClient* self) {
    delete self->client;
    self->client = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// ─── メソッド ─────────────────────────────────────────────────────────

static PyObject* PyPipeClient_connect(PyPipeClient* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"timeout", nullptr};
    double timeout_sec = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|d",
                                     const_cast<char**>(kwlist), &timeout_sec)) {
        return nullptr;
    }
    if (!check_client(self)) return nullptr;

    auto timeout_ms = to_ms_c(timeout_sec);
    pipeutil::PipeException* pending = nullptr;

    PIPELOG("connect: BEGIN_ALLOW_THREADS timeout_ms=%lld", (long long)timeout_ms.count());
    Py_BEGIN_ALLOW_THREADS
    try {
        self->client->connect(timeout_ms);
    } catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS
    PIPELOG("connect: END_ALLOW_THREADS pending=%s", pending ? "yes" : "no");

    if (pending) {
        set_python_exception(*pending);
        delete pending;
        return nullptr;
    }
    Py_RETURN_NONE;
}

static PyObject* PyPipeClient_send(PyPipeClient* self, PyObject* args) {
    PyObject* obj = nullptr;
    if (!PyArg_ParseTuple(args, "O", &obj)) return nullptr;
    if (!check_client(self)) return nullptr;

    pipeutil::Message msg{};
    if (!PyMessage_convert(obj, msg)) return nullptr;

    pipeutil::PipeException* pending = nullptr;

    PIPELOG("send: BEGIN_ALLOW_THREADS size=%zu", msg.payload().size());
    Py_BEGIN_ALLOW_THREADS
    try {
        self->client->send(msg);
    } catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS
    PIPELOG("send: END_ALLOW_THREADS pending=%s", pending ? "yes" : "no");

    if (pending) {
        set_python_exception(*pending);
        delete pending;
        return nullptr;
    }
    Py_RETURN_NONE;
}

static PyObject* PyPipeClient_receive(PyPipeClient* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"timeout", nullptr};
    double timeout_sec = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|d",
                                     const_cast<char**>(kwlist), &timeout_sec)) {
        return nullptr;
    }
    if (!check_client(self)) return nullptr;

    auto timeout_ms = to_ms_c(timeout_sec);
    pipeutil::Message result{};
    pipeutil::PipeException* pending = nullptr;

    PIPELOG("receive: BEGIN_ALLOW_THREADS timeout_ms=%lld", (long long)timeout_ms.count());
    Py_BEGIN_ALLOW_THREADS
    try {
        result = self->client->receive(timeout_ms);
    } catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS
    PIPELOG("receive: END_ALLOW_THREADS pending=%s size=%zu",
            pending ? "yes" : "no", pending ? 0 : result.payload().size());

    if (pending) {
        set_python_exception(*pending);
        delete pending;
        return nullptr;
    }
    return PyMessage_from_message(std::move(result));
}

static PyObject* PyPipeClient_close(PyPipeClient* self, PyObject* /*args*/) {
    if (self->client) {
        // CloseHandle はわずかにブロックする可能性があるため GIL を解放する (R-GIL-001)。
        PIPELOG("close: BEGIN_ALLOW_THREADS");
        Py_BEGIN_ALLOW_THREADS
        self->client->close();
        Py_END_ALLOW_THREADS
        PIPELOG("close: END_ALLOW_THREADS");
    }
    Py_RETURN_NONE;
}

static PyObject* PyPipeClient_enter(PyPipeClient* self, PyObject* /*args*/) {
    Py_INCREF(self);
    return reinterpret_cast<PyObject*>(self);
}

static PyObject* PyPipeClient_exit(PyPipeClient* self, PyObject* /*args*/) {
    PyPipeClient_close(self, nullptr);
    Py_RETURN_NONE;
}

// ─── 診断・メトリクス (F-006) ─────────────────────────────────────────

static PyObject* PyPipeClient_stats(PyPipeClient* self, PyObject* /*args*/) {
    if (!check_client(self)) return nullptr;
    return PyPipeStats_from_stats(self->client->stats());
}

static PyObject* PyPipeClient_reset_stats(PyPipeClient* self, PyObject* /*args*/) {
    if (!check_client(self)) return nullptr;
    self->client->reset_stats();
    Py_RETURN_NONE;
}

// ─── プロパティ ───────────────────────────────────────────────────────

static PyObject* PyPipeClient_get_is_connected(PyPipeClient* self, void*) {
    if (!self->client) Py_RETURN_FALSE;
    return PyBool_FromLong(self->client->is_connected() ? 1 : 0);
}

static PyObject* PyPipeClient_get_pipe_name(PyPipeClient* self, void*) {
    if (!self->client) {
        PyErr_SetString(PyExc_RuntimeError, "PipeClient is closed");
        return nullptr;
    }
    return PyUnicode_FromString(self->client->pipe_name().c_str());
}

static PyGetSetDef PyPipeClient_getset[] = {
    {"is_connected", reinterpret_cast<getter>(PyPipeClient_get_is_connected),
     nullptr, "True when connected", nullptr},
    {"pipe_name",    reinterpret_cast<getter>(PyPipeClient_get_pipe_name),
     nullptr, "Pipe identifier name", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}
};

// ─── メソッドテーブル ─────────────────────────────────────────────────

static PyMethodDef PyPipeClient_methods[] = {
    {"connect",   reinterpret_cast<PyCFunction>(PyPipeClient_connect),
     METH_VARARGS | METH_KEYWORDS,
     "connect(timeout: float = 0.0) -> None\nConnect to a PipeServer."},
    {"send",      reinterpret_cast<PyCFunction>(PyPipeClient_send),
     METH_VARARGS, "send(msg: Message | bytes | str) -> None\nSend a framed message."},
    {"receive",   reinterpret_cast<PyCFunction>(PyPipeClient_receive),
     METH_VARARGS | METH_KEYWORDS,
     "receive(timeout: float = 0.0) -> Message\nReceive a framed message."},
    {"close",     reinterpret_cast<PyCFunction>(PyPipeClient_close),
     METH_NOARGS, "Close the connection."},
    {"__enter__", reinterpret_cast<PyCFunction>(PyPipeClient_enter),
     METH_NOARGS, "Context manager entry."},
    {"__exit__",  reinterpret_cast<PyCFunction>(PyPipeClient_exit),
     METH_VARARGS, "Context manager exit (calls close)."},
    {"stats",       reinterpret_cast<PyCFunction>(PyPipeClient_stats),
     METH_NOARGS, "stats() -> PipeStats\nReturn a diagnostics snapshot."},
    {"reset_stats", reinterpret_cast<PyCFunction>(PyPipeClient_reset_stats),
     METH_NOARGS, "reset_stats() -> None\nReset all counters to 0."},
    {nullptr, nullptr, 0, nullptr}
};

// ─── PyTypeObject ─────────────────────────────────────────────────────

PyTypeObject PyPipeClient_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "_pipeutil.PipeClient",
    sizeof(PyPipeClient),
    0,
    reinterpret_cast<destructor>(PyPipeClient_dealloc),
    0, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    "PipeClient(pipe_name: str, buffer_size: int = 65536)\n\n"
    "Named pipe client. Use: connect() -> send()/receive() -> close()",
    nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    PyPipeClient_methods,
    nullptr,
    PyPipeClient_getset,
    nullptr, nullptr, nullptr, nullptr, 0,
    reinterpret_cast<initproc>(PyPipeClient_init),
    nullptr,
    PyPipeClient_new,
};

} // namespace pyutil
