// py_pipe_server.cpp — PyPipeServer 型実装
// 仕様: spec/04_python_wrapper.md §5
// GIL: accept / send / receive はブロッキング I/O のため GIL を解放する。
// Capability Negotiation: spec/A001_capability_negotiation.md

#include "py_pipe_server.hpp"
#include "py_exceptions.hpp"
#include "py_message.hpp"
#include "py_pipe_stats.hpp"
#include "py_capability.hpp"
#include "py_debug_log.hpp"
#include <chrono>

namespace pyutil {

// ─── helper: 秒（double）→ milliseconds 変換 ──────────────────────────
// タイムアウト値はミリ秒単位で受け取る（Python API 規約: timeout_msec 整数 or float）
static std::chrono::milliseconds to_ms(double ms_val) noexcept {
    if (ms_val <= 0.0) return std::chrono::milliseconds{0}; // 0 = 無限
    return std::chrono::milliseconds{static_cast<int64_t>(ms_val)};
}

// ─── helper: server が有効か確認 ──────────────────────────────────────
static bool check_server(PyPipeServer* self) noexcept {
    if (!self->server) {
        PyErr_SetString(PyExc_RuntimeError, "PipeServer is closed or not initialized");
        return false;
    }
    return true;
}

// ─── tp_new / tp_init / tp_dealloc ────────────────────────────────────

static PyObject* PyPipeServer_new(PyTypeObject* type,
                                  PyObject* /*args*/,
                                  PyObject* /*kwds*/) {
    PyPipeServer* self = reinterpret_cast<PyPipeServer*>(type->tp_alloc(type, 0));
    if (self) {
        self->server              = nullptr;
        self->on_hello_complete_cb = nullptr;
    }
    return reinterpret_cast<PyObject*>(self);
}

static int PyPipeServer_init(PyPipeServer* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {
        "pipe_name", "buffer_size", "acl", "custom_sddl", "hello_config", nullptr};
    const char* pipe_name        = nullptr;
    Py_ssize_t  buffer_size      = 65536;
    int         acl_int          = 0;   // PipeAcl::Default
    const char* custom_sddl      = nullptr;
    PyObject*   hello_config_obj = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|nizO",
                                     const_cast<char**>(kwlist),
                                     &pipe_name, &buffer_size,
                                     &acl_int, &custom_sddl,
                                     &hello_config_obj)) {
        return -1;
    }
    if (buffer_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "buffer_size must be positive");
        return -1;
    }
    // PipeAcl の有効範囲は 0(Default)〜3(Custom)。範囲外は ValueError (R-071)
    if (acl_int < 0 || acl_int > 3) {
        PyErr_SetString(PyExc_ValueError,
                        "acl must be a PipeAcl constant: "
                        "0=Default, 1=LocalSystem, 2=Everyone, 3=Custom");
        return -1;
    }

    // HelloConfig の解析 (None / 省略時はデフォルト値) (A-001)
    pipeutil::HelloConfig hello_cfg{};
    if (hello_config_obj && hello_config_obj != Py_None) {
        PyObject* mode_attr = PyObject_GetAttrString(hello_config_obj, "mode");
        if (!mode_attr) return -1;
        long mode_val = PyLong_AsLong(mode_attr);
        Py_DECREF(mode_attr);
        if (mode_val == -1 && PyErr_Occurred()) return -1;
        hello_cfg.mode = static_cast<pipeutil::HelloMode>(mode_val);

        PyObject* timeout_attr = PyObject_GetAttrString(hello_config_obj, "hello_timeout_ms");
        if (!timeout_attr) return -1;
        long timeout_val = PyLong_AsLong(timeout_attr);
        Py_DECREF(timeout_attr);
        if (timeout_val == -1 && PyErr_Occurred()) return -1;
        hello_cfg.hello_timeout = std::chrono::milliseconds{timeout_val};

        PyObject* caps_attr = PyObject_GetAttrString(hello_config_obj, "advertised_capabilities");
        if (!caps_attr) return -1;
        unsigned long caps_val = PyLong_AsUnsignedLong(caps_attr);
        Py_DECREF(caps_attr);
        if (caps_val == static_cast<unsigned long>(-1) && PyErr_Occurred()) return -1;
        hello_cfg.advertised_capabilities = static_cast<uint32_t>(caps_val);
    }

    try {
        delete self->server;
        self->server = new pipeutil::PipeServer{
            std::string{pipe_name},
            static_cast<std::size_t>(buffer_size),
            static_cast<pipeutil::PipeAcl>(acl_int),
            custom_sddl ? std::string{custom_sddl} : std::string{},
            hello_cfg};
    } catch (const pipeutil::PipeException& e) {
        set_python_exception(e);
        return -1;
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return -1;
    }
    return 0;
}

static void PyPipeServer_dealloc(PyPipeServer* self) {
    delete self->server;
    self->server = nullptr;
    Py_XDECREF(self->on_hello_complete_cb);
    self->on_hello_complete_cb = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// ─── メソッド ─────────────────────────────────────────────────────────

static PyObject* PyPipeServer_listen(PyPipeServer* self, PyObject* /*args*/) {
    if (!check_server(self)) return nullptr;
    try {
        self->server->listen();
    } catch (const pipeutil::PipeException& e) {
        set_python_exception(e);
        return nullptr;
    }
    Py_RETURN_NONE;
}

static PyObject* PyPipeServer_accept(PyPipeServer* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"timeout", nullptr};
    double timeout_sec = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|d",
                                     const_cast<char**>(kwlist), &timeout_sec)) {
        return nullptr;
    }
    if (!check_server(self)) return nullptr;

    auto timeout_ms = to_ms(timeout_sec);
    pipeutil::PipeException* pending = nullptr;

    PIPELOG("accept: BEGIN_ALLOW_THREADS timeout_ms=%lld", (long long)timeout_ms.count());
    Py_BEGIN_ALLOW_THREADS
    try {
        self->server->accept(timeout_ms);
    } catch (const pipeutil::PipeException& e) {
        pending = new pipeutil::PipeException{e};
    }
    Py_END_ALLOW_THREADS
    PIPELOG("accept: END_ALLOW_THREADS pending=%s", pending ? "yes" : "no");

    if (pending) {
        set_python_exception(*pending);
        delete pending;
        return nullptr;
    }

    // on_hello_complete コールバックを GIL 保持下で呼び出す (A-001)
    if (self->on_hello_complete_cb) {
        auto caps = self->server->negotiated_capabilities();
        PyObject* py_caps = PyNegotiatedCapabilities_from_caps(caps);
        if (!py_caps) return nullptr;
        PyObject* result = PyObject_CallOneArg(self->on_hello_complete_cb, py_caps);
        Py_DECREF(py_caps);
        if (!result) return nullptr;  // Python 例外を伝播
        Py_DECREF(result);
    }

    Py_RETURN_NONE;
}

static PyObject* PyPipeServer_send(PyPipeServer* self, PyObject* args) {
    PyObject* obj = nullptr;
    if (!PyArg_ParseTuple(args, "O", &obj)) return nullptr;
    if (!check_server(self)) return nullptr;

    pipeutil::Message msg{};
    if (!PyMessage_convert(obj, msg)) return nullptr;

    pipeutil::PipeException* pending = nullptr;

    PIPELOG("send: BEGIN_ALLOW_THREADS size=%zu", msg.payload().size());
    Py_BEGIN_ALLOW_THREADS
    try {
        self->server->send(msg);
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

static PyObject* PyPipeServer_receive(PyPipeServer* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"timeout", nullptr};
    double timeout_sec = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|d",
                                     const_cast<char**>(kwlist), &timeout_sec)) {
        return nullptr;
    }
    if (!check_server(self)) return nullptr;

    auto timeout_ms = to_ms(timeout_sec);
    pipeutil::Message result{};
    pipeutil::PipeException* pending = nullptr;

    PIPELOG("receive: BEGIN_ALLOW_THREADS timeout_ms=%lld", (long long)timeout_ms.count());
    Py_BEGIN_ALLOW_THREADS
    try {
        result = self->server->receive(timeout_ms);
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

static PyObject* PyPipeServer_close(PyPipeServer* self, PyObject* /*args*/) {
    if (self->server) {
        // close() は内部で FlushFileBuffers (ブロッキング I/O) を呼ぶ可能性があるため
        // GIL を解放してデッドロックを回避する (R-GIL-001)。
        PIPELOG("close: BEGIN_ALLOW_THREADS");
        Py_BEGIN_ALLOW_THREADS
        self->server->close();
        Py_END_ALLOW_THREADS
        PIPELOG("close: END_ALLOW_THREADS");
    }
    Py_RETURN_NONE;
}

// ─── コンテキストマネージャ ────────────────────────────────────────────

static PyObject* PyPipeServer_enter(PyPipeServer* self, PyObject* /*args*/) {
    Py_INCREF(self);
    return reinterpret_cast<PyObject*>(self);
}

static PyObject* PyPipeServer_exit(PyPipeServer* self, PyObject* /*args*/) {
    PyPipeServer_close(self, nullptr);
    Py_RETURN_NONE;
}

// ─── 診断・メトリクス (F-006) ─────────────────────────────────

static PyObject* PyPipeServer_stats(PyPipeServer* self, PyObject* /*args*/) {
    if (!check_server(self)) return nullptr;
    return PyPipeStats_from_stats(self->server->stats());
}

static PyObject* PyPipeServer_reset_stats(PyPipeServer* self, PyObject* /*args*/) {
    if (!check_server(self)) return nullptr;
    self->server->reset_stats();
    Py_RETURN_NONE;
}

// ─── プロパティ ───────────────────────────────────────────────────────

static PyObject* PyPipeServer_get_is_listening(PyPipeServer* self, void*) {
    if (!self->server) Py_RETURN_FALSE;
    return PyBool_FromLong(self->server->is_listening() ? 1 : 0);
}

static PyObject* PyPipeServer_get_is_connected(PyPipeServer* self, void*) {
    if (!self->server) Py_RETURN_FALSE;
    return PyBool_FromLong(self->server->is_connected() ? 1 : 0);
}

static PyObject* PyPipeServer_get_pipe_name(PyPipeServer* self, void*) {
    if (!self->server) {
        PyErr_SetString(PyExc_RuntimeError, "PipeServer is closed");
        return nullptr;
    }
    return PyUnicode_FromString(self->server->pipe_name().c_str());
}

// ネゴシエーション結果プロパティ (A-001)
static PyObject* PyPipeServer_get_negotiated_capabilities(PyPipeServer* self, void*) {
    if (!self->server) {
        PyErr_SetString(PyExc_RuntimeError, "PipeServer is closed");
        return nullptr;
    }
    return PyNegotiatedCapabilities_from_caps(self->server->negotiated_capabilities());
}

// on_hello_complete コールバックゲッター
static PyObject* PyPipeServer_get_on_hello_complete(PyPipeServer* self, void*) {
    PyObject* cb = self->on_hello_complete_cb ? self->on_hello_complete_cb : Py_None;
    Py_INCREF(cb);
    return cb;
}

// on_hello_complete コールバックセッター
static int PyPipeServer_set_on_hello_complete(PyPipeServer* self, PyObject* value, void*) {
    if (value && value != Py_None && !PyCallable_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "on_hello_complete must be callable or None");
        return -1;
    }
    Py_XDECREF(self->on_hello_complete_cb);
    self->on_hello_complete_cb = (value == Py_None || value == nullptr) ? nullptr : value;
    Py_XINCREF(self->on_hello_complete_cb);
    return 0;
}

static PyGetSetDef PyPipeServer_getset[] = {
    {"is_listening", reinterpret_cast<getter>(PyPipeServer_get_is_listening),
     nullptr, "True after listen()", nullptr},
    {"is_connected", reinterpret_cast<getter>(PyPipeServer_get_is_connected),
     nullptr, "True after accept()", nullptr},
    {"pipe_name",    reinterpret_cast<getter>(PyPipeServer_get_pipe_name),
     nullptr, "Pipe identifier name", nullptr},
    {"negotiated_capabilities",
     reinterpret_cast<getter>(PyPipeServer_get_negotiated_capabilities),
     nullptr, "NegotiatedCapabilities after accept()", nullptr},
    {"on_hello_complete",
     reinterpret_cast<getter>(PyPipeServer_get_on_hello_complete),
     reinterpret_cast<setter>(PyPipeServer_set_on_hello_complete),
     "Callable invoked with NegotiatedCapabilities after accept()", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}
};

// ─── メソッドテーブル ─────────────────────────────────────────────────

static PyMethodDef PyPipeServer_methods[] = {
    {"listen",    reinterpret_cast<PyCFunction>(PyPipeServer_listen),
     METH_NOARGS, "Create pipe and start listening."},
    {"accept",    reinterpret_cast<PyCFunction>(PyPipeServer_accept),
     METH_VARARGS | METH_KEYWORDS,
     "accept(timeout: float = 0.0) -> None\nWait for a client connection."},
    {"send",      reinterpret_cast<PyCFunction>(PyPipeServer_send),
     METH_VARARGS, "send(msg: Message | bytes | str) -> None\nSend a framed message."},
    {"receive",   reinterpret_cast<PyCFunction>(PyPipeServer_receive),
     METH_VARARGS | METH_KEYWORDS,
     "receive(timeout: float = 0.0) -> Message\nReceive a framed message."},
    {"close",     reinterpret_cast<PyCFunction>(PyPipeServer_close),
     METH_NOARGS, "Close the pipe connection."},
    {"__enter__", reinterpret_cast<PyCFunction>(PyPipeServer_enter),
     METH_NOARGS, "Context manager entry."},
    {"__exit__",  reinterpret_cast<PyCFunction>(PyPipeServer_exit),
     METH_VARARGS, "Context manager exit (calls close)."},
    {"stats",       reinterpret_cast<PyCFunction>(PyPipeServer_stats),
     METH_NOARGS, "stats() -> PipeStats\nReturn a diagnostics snapshot."},
    {"reset_stats", reinterpret_cast<PyCFunction>(PyPipeServer_reset_stats),
     METH_NOARGS, "reset_stats() -> None\nReset all counters to 0."},
    {nullptr, nullptr, 0, nullptr}
};

// ─── PyTypeObject ─────────────────────────────────────────────────────

PyTypeObject PyPipeServer_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "_pipeutil.PipeServer",
    sizeof(PyPipeServer),
    0,
    reinterpret_cast<destructor>(PyPipeServer_dealloc),
    0, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    "PipeServer(pipe_name: str, buffer_size: int = 65536)\n\n"
    "Named pipe server. Use: listen() -> accept() -> send()/receive() -> close()",
    nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    PyPipeServer_methods,
    nullptr,
    PyPipeServer_getset,
    nullptr, nullptr, nullptr, nullptr, 0,
    reinterpret_cast<initproc>(PyPipeServer_init),
    nullptr,
    PyPipeServer_new,
};

} // namespace pyutil
