// py_message.cpp — PyMessage 型実装
// 仕様: spec/04_python_wrapper.md §4

#include "py_message.hpp"
#include "py_exceptions.hpp"
#include <cstring>

namespace pyutil {

// ─── tp_new ──────────────────────────────────────────────────────────

static PyObject* PyMessage_new(PyTypeObject* type,
                               PyObject* /*args*/,
                               PyObject* /*kwds*/) {
    PyMessage* self = reinterpret_cast<PyMessage*>(type->tp_alloc(type, 0));
    if (self) self->msg = nullptr;
    return reinterpret_cast<PyObject*>(self);
}

// ─── tp_init ─────────────────────────────────────────────────────────

static int PyMessage_init(PyMessage* self, PyObject* args, PyObject* /*kwds*/) {
    PyObject* data = nullptr;
    if (!PyArg_ParseTuple(args, "O", &data)) return -1;

    pipeutil::Message msg_val{};

    if (PyBytes_Check(data)) {
        // bytes → Message
        const char* buf = PyBytes_AS_STRING(data);
        const Py_ssize_t sz = PyBytes_GET_SIZE(data);
        msg_val = pipeutil::Message{
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(buf),
                static_cast<std::size_t>(sz)}};
    } else if (PyByteArray_Check(data)) {
        const char* buf = PyByteArray_AS_STRING(data);
        const Py_ssize_t sz = PyByteArray_GET_SIZE(data);
        msg_val = pipeutil::Message{
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(buf),
                static_cast<std::size_t>(sz)}};
    } else {
        // str を含む bytes 以外の型は拒否する（bytes-only API）
        PyErr_SetString(PyExc_TypeError,
                        "Message() argument must be bytes or bytearray");
        return -1;
    }

    delete self->msg;
    self->msg = new pipeutil::Message{std::move(msg_val)};
    return 0;
}

// ─── tp_dealloc ──────────────────────────────────────────────────────

static void PyMessage_dealloc(PyMessage* self) {
    delete self->msg;
    self->msg = nullptr;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// ─── プロパティ ───────────────────────────────────────────────────────

static PyObject* PyMessage_get_data(PyMessage* self, void* /*closure*/) {
    if (!self->msg) {
        PyErr_SetString(PyExc_RuntimeError, "Message is not initialized");
        return nullptr;
    }
    const auto payload = self->msg->payload();
    return PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(payload.data()),
        static_cast<Py_ssize_t>(payload.size()));
}

static PyObject* PyMessage_get_text(PyMessage* self, void* /*closure*/) {
    if (!self->msg) {
        PyErr_SetString(PyExc_RuntimeError, "Message is not initialized");
        return nullptr;
    }
    const auto sv = self->msg->as_string_view();
    return PyUnicode_DecodeUTF8(sv.data(), static_cast<Py_ssize_t>(sv.size()), "strict");
}

static PyGetSetDef PyMessage_getset[] = {
    {"data", reinterpret_cast<getter>(PyMessage_get_data), nullptr,
     "Payload as bytes", nullptr},
    {"text", reinterpret_cast<getter>(PyMessage_get_text), nullptr,
     "Payload decoded as UTF-8 str", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}  // sentinel
};

// ─── 特殊メソッド ─────────────────────────────────────────────────────

static Py_ssize_t PyMessage_len(PyMessage* self) {
    if (!self->msg) return 0;
    return static_cast<Py_ssize_t>(self->msg->size());
}

static int PyMessage_bool(PyMessage* self) {
    if (!self->msg) return 0;
    return self->msg->empty() ? 0 : 1;
}

static PyObject* PyMessage_repr(PyMessage* self) {
    const std::size_t sz = self->msg ? self->msg->size() : 0;
    return PyUnicode_FromFormat("Message(size=%zu)", sz);
}

// ─── Buffer protocol ────────────────────────────────────────────────────
// bytes(msg) / memoryview(msg) を可能にするため Py_buffer を実装する。
// payload() が返す span は Message 生存中は有効なので read-only バッファとして公開する。

static int PyMessage_getbuffer(PyMessage* self, Py_buffer* view, int flags) {
    if (!self->msg) {
        PyErr_SetString(PyExc_RuntimeError, "Message is not initialized");
        return -1;
    }
    const auto payload = self->msg->payload();
    return PyBuffer_FillInfo(
        view,
        reinterpret_cast<PyObject*>(self),
        const_cast<void*>(static_cast<const void*>(payload.data())),
        static_cast<Py_ssize_t>(payload.size()),
        1,       // read-only
        flags);
}

static PyBufferProcs PyMessage_as_buffer = {
    reinterpret_cast<getbufferproc>(PyMessage_getbuffer),
    nullptr  // bf_releasebuffer: PyBuffer_FillInfo は解放不要
};

// ─── スロット定義 ─────────────────────────────────────────────────────

static PySequenceMethods PyMessage_as_sequence = {
    reinterpret_cast<lenfunc>(PyMessage_len),  // sq_length
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr
};

static PyNumberMethods PyMessage_as_number = {
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
    reinterpret_cast<inquiry>(PyMessage_bool),  // nb_bool
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr
};

// ─── PyTypeObject ─────────────────────────────────────────────────────

PyTypeObject PyMessage_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "_pipeutil.Message",                       // tp_name
    sizeof(PyMessage),                         // tp_basicsize
    0,                                         // tp_itemsize
    reinterpret_cast<destructor>(PyMessage_dealloc), // tp_dealloc
    0,                                         // tp_vectorcall_offset
    nullptr,                                   // tp_getattr
    nullptr,                                   // tp_setattr
    nullptr,                                   // tp_as_async
    reinterpret_cast<reprfunc>(PyMessage_repr),// tp_repr
    &PyMessage_as_number,                      // tp_as_number
    &PyMessage_as_sequence,                    // tp_as_sequence
    nullptr,                                   // tp_as_mapping
    nullptr,                                   // tp_hash
    nullptr,                                   // tp_call
    nullptr,                                   // tp_str
    nullptr,                                   // tp_getattro
    nullptr,                                   // tp_setattro
    &PyMessage_as_buffer,                       // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  // tp_flags
    "pipeutil Message object.\n\nMessage(data: bytes | str)\n",  // tp_doc
    nullptr,                                   // tp_traverse
    nullptr,                                   // tp_clear
    nullptr,                                   // tp_richcompare
    0,                                         // tp_weaklistoffset
    nullptr,                                   // tp_iter
    nullptr,                                   // tp_iternext
    nullptr,                                   // tp_methods
    nullptr,                                   // tp_members
    PyMessage_getset,                          // tp_getset
    nullptr,                                   // tp_base
    nullptr,                                   // tp_dict
    nullptr,                                   // tp_descr_get
    nullptr,                                   // tp_descr_set
    0,                                         // tp_dictoffset
    reinterpret_cast<initproc>(PyMessage_init),// tp_init
    nullptr,                                   // tp_alloc
    PyMessage_new,                             // tp_new
};

// ─── ファクトリ ───────────────────────────────────────────────────────

PyObject* PyMessage_from_message(pipeutil::Message msg) noexcept {
    PyMessage* obj = reinterpret_cast<PyMessage*>(
        PyMessage_Type.tp_alloc(&PyMessage_Type, 0));
    if (!obj) return nullptr;
    obj->msg = new pipeutil::Message{std::move(msg)};
    return reinterpret_cast<PyObject*>(obj);
}

// ─── 型変換ユーティリティ ─────────────────────────────────────────────

bool PyMessage_convert(PyObject* obj, pipeutil::Message& out_msg) noexcept {
    if (PyObject_TypeCheck(obj, &PyMessage_Type)) {
        // PyMessage → Message コピー
        PyMessage* pm = reinterpret_cast<PyMessage*>(obj);
        if (!pm->msg) {
            PyErr_SetString(PyExc_RuntimeError, "Message is not initialized");
            return false;
        }
        out_msg = pipeutil::Message{pm->msg->payload()};
        return true;
    }
    if (PyBytes_Check(obj)) {
        out_msg = pipeutil::Message{
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(PyBytes_AS_STRING(obj)),
                static_cast<std::size_t>(PyBytes_GET_SIZE(obj))}};
        return true;
    }
    if (PyByteArray_Check(obj)) {
        out_msg = pipeutil::Message{
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(PyByteArray_AS_STRING(obj)),
                static_cast<std::size_t>(PyByteArray_GET_SIZE(obj))}};
        return true;
    }
    if (PyUnicode_Check(obj)) {
        Py_ssize_t sz = 0;
        const char* utf8 = PyUnicode_AsUTF8AndSize(obj, &sz);
        if (!utf8) return false;
        out_msg = pipeutil::Message{
            std::string_view{utf8, static_cast<std::size_t>(sz)}};
        return true;
    }
    PyErr_SetString(PyExc_TypeError,
                    "send() argument must be Message, bytes, bytearray, or str");
    return false;
}

} // namespace pyutil
