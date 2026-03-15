// py_capability.cpp — PyNegotiatedCapabilities 型実装 (A-001)
// 仕様: spec/A001_capability_negotiation.md §9

#include "py_compat.hpp"  // must precede all Python includes; provides 3.8+ shims
#include "py_capability.hpp"

namespace pyutil {

// ─── tp_new / tp_dealloc ──────────────────────────────────────────────────

static PyObject* PyNegotiatedCapabilities_new(PyTypeObject* type,
                                               PyObject* /*args*/,
                                               PyObject* /*kwds*/)
{
    auto* self = reinterpret_cast<PyNegotiatedCapabilities*>(type->tp_alloc(type, 0));
    if (self) {
        self->bitmap    = 0u;
        self->v1_compat = 0;
    }
    return reinterpret_cast<PyObject*>(self);
}

static void PyNegotiatedCapabilities_dealloc(PyNegotiatedCapabilities* self)
{
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// ─── repr ─────────────────────────────────────────────────────────────────

static PyObject* PyNegotiatedCapabilities_repr(PyNegotiatedCapabilities* self)
{
    return PyUnicode_FromFormat(
        "NegotiatedCapabilities(bitmap=%#010x, v1_compat=%s)",
        (unsigned int)self->bitmap,
        self->v1_compat ? "True" : "False");
}

// ─── メソッド ─────────────────────────────────────────────────────────────

static PyObject* PyNegotiatedCapabilities_has(PyNegotiatedCapabilities* self, PyObject* args)
{
    long cap_val = 0;
    if (!PyArg_ParseTuple(args, "l", &cap_val)) return nullptr;
    const bool result = (self->bitmap & static_cast<uint32_t>(cap_val)) != 0u;
    return PyBool_FromLong(result ? 1 : 0);
}

static PyMethodDef PyNegotiatedCapabilities_methods[] = {
    {"has", reinterpret_cast<PyCFunction>(PyNegotiatedCapabilities_has),
     METH_VARARGS,
     "has(cap: int) -> bool\n"
     "Return True if the capability bit is set in the negotiated bitmap."},
    {nullptr, nullptr, 0, nullptr}
};

// ─── プロパティ ───────────────────────────────────────────────────────────

static PyObject* PyNegotiatedCapabilities_get_bitmap(PyNegotiatedCapabilities* self, void*)
{
    return PyLong_FromUnsignedLong(static_cast<unsigned long>(self->bitmap));
}

static PyObject* PyNegotiatedCapabilities_get_v1_compat(PyNegotiatedCapabilities* self, void*)
{
    return PyBool_FromLong(self->v1_compat ? 1 : 0);
}

static PyObject* PyNegotiatedCapabilities_get_is_legacy_v1(PyNegotiatedCapabilities* self, void*)
{
    // bitmap == 0 かつ v1_compat でない場合 True (C++ NegotiatedCapabilities::is_legacy_v1 と一致)
    return PyBool_FromLong((self->bitmap == 0u && !self->v1_compat) ? 1 : 0);
}

static PyObject* PyNegotiatedCapabilities_get_is_v1_compat(PyNegotiatedCapabilities* self, void*)
{
    return PyBool_FromLong(self->v1_compat ? 1 : 0);
}

static PyGetSetDef PyNegotiatedCapabilities_getset[] = {
    {"bitmap",
     reinterpret_cast<getter>(PyNegotiatedCapabilities_get_bitmap),
     nullptr, "Negotiated capability bitmap (int)", nullptr},
    {"v1_compat",
     reinterpret_cast<getter>(PyNegotiatedCapabilities_get_v1_compat),
     nullptr, "True if peer is a v1.0.0 client", nullptr},
    {"is_legacy_v1",
     reinterpret_cast<getter>(PyNegotiatedCapabilities_get_is_legacy_v1),
     nullptr, "True when bitmap == 0 (no capabilities negotiated)", nullptr},
    {"is_v1_compat",
     reinterpret_cast<getter>(PyNegotiatedCapabilities_get_is_v1_compat),
     nullptr, "Alias for v1_compat", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}
};

// ─── tp_init ─────────────────────────────────────────────────────────────
// NegotiatedCapabilities(bitmap=0, v1_compat=False)
// テストや手動構築用に引数を受け付ける
static int PyNegotiatedCapabilities_init(PyNegotiatedCapabilities* self,
                                          PyObject* args, PyObject* kwds)
{
    static const char* kwlist[] = {"bitmap", "v1_compat", nullptr};
    unsigned long bitmap    = 0u;
    int           v1_compat = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|kp",
                                     const_cast<char**>(kwlist),
                                     &bitmap, &v1_compat)) {
        return -1;
    }
    self->bitmap    = static_cast<uint32_t>(bitmap);
    self->v1_compat = v1_compat;
    return 0;
}

// ─── PyTypeObject ─────────────────────────────────────────────────────────

PyTypeObject PyNegotiatedCapabilities_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "_pipeutil.NegotiatedCapabilities",
    sizeof(PyNegotiatedCapabilities),
    0,
    reinterpret_cast<destructor>(PyNegotiatedCapabilities_dealloc),
    0, nullptr, nullptr, nullptr,
    reinterpret_cast<reprfunc>(PyNegotiatedCapabilities_repr),
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT,
    "NegotiatedCapabilities(bitmap: int = 0, v1_compat: bool = False)\n\n"
    "Result of HELLO capability negotiation (A-001).\n"
    "bitmap     : client_bitmap & server_bitmap\n"
    "v1_compat  : True when peer connected with version=0x01 (v1.0.0 client)",
    nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    PyNegotiatedCapabilities_methods,
    nullptr,
    PyNegotiatedCapabilities_getset,
    nullptr, nullptr, nullptr, nullptr, 0,
    reinterpret_cast<initproc>(PyNegotiatedCapabilities_init),
    nullptr,
    PyNegotiatedCapabilities_new,
};

// ─── 生成ヘルパー ─────────────────────────────────────────────────────────

PyObject* PyNegotiatedCapabilities_from_caps(const pipeutil::NegotiatedCapabilities& caps)
{
    auto* obj = reinterpret_cast<PyNegotiatedCapabilities*>(
        PyNegotiatedCapabilities_Type.tp_alloc(&PyNegotiatedCapabilities_Type, 0));
    if (!obj) return nullptr;
    obj->bitmap    = caps.bitmap;
    obj->v1_compat = caps.v1_compat ? 1 : 0;
    return reinterpret_cast<PyObject*>(obj);
}

} // namespace pyutil
