// py_pipe_stats.cpp — PyPipeStats 型実装
// 仕様: spec/F006_diagnostics_metrics.md §5.1

#include "py_compat.hpp"  // must precede all Python includes; provides 3.8+ shims
#include "py_pipe_stats.hpp"
#include <new>             // std::operator new (placement new)

namespace pyutil {

// ─── tp_new / tp_dealloc ──────────────────────────────────────────────

static PyObject* PyPipeStats_new(PyTypeObject* type, PyObject* /*args*/, PyObject* /*kwds*/) {
    PyPipeStats* self = reinterpret_cast<PyPipeStats*>(type->tp_alloc(type, 0));
    if (self) {
        // placement new で PipeStats を値初期化（フィールドを 0 に）
        new (&self->stats) pipeutil::PipeStats();
    }
    return reinterpret_cast<PyObject*>(self);
}

static void PyPipeStats_dealloc(PyPipeStats* self) {
    self->stats.~PipeStats();
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

// ─── __repr__ ─────────────────────────────────────────────────────────

static PyObject* PyPipeStats_repr(PyPipeStats* self) {
    const auto& s = self->stats;
    return PyUnicode_FromFormat(
        "PipeStats(sent=%llu, recv=%llu, bytes_sent=%llu, bytes_received=%llu, errors=%llu)",
        (unsigned long long)s.messages_sent,
        (unsigned long long)s.messages_received,
        (unsigned long long)s.bytes_sent,
        (unsigned long long)s.bytes_received,
        (unsigned long long)s.errors);
}

// ─── 読み取り専用プロパティ ───────────────────────────────────────────

#define PROP_UINT64(field_name, cpp_field, doc_str)                             \
    static PyObject* PyPipeStats_get_##field_name(PyPipeStats* self, void*) {  \
        return PyLong_FromUnsignedLongLong(                                     \
            static_cast<unsigned long long>(self->stats.cpp_field));            \
    }

PROP_UINT64(messages_sent,     messages_sent,     "送信に成功したメッセージ数")
PROP_UINT64(messages_received, messages_received, "受信に成功したメッセージ数")
PROP_UINT64(bytes_sent,        bytes_sent,        "送信ペイロード総バイト数")
PROP_UINT64(bytes_received,    bytes_received,    "受信ペイロード総バイト数")
PROP_UINT64(errors,            errors,            "PipeError 総数")
PROP_UINT64(rpc_calls,         rpc_calls,         "send_request() 正常完了回数")
PROP_UINT64(rtt_total_ns,      rtt_total_ns,      "RTT 合計ナノ秒")
PROP_UINT64(rtt_last_ns,       rtt_last_ns,       "最後の RTT ナノ秒")

#undef PROP_UINT64

static PyObject* PyPipeStats_get_avg_round_trip_ns(PyPipeStats* self, void*) {
    return PyLong_FromUnsignedLongLong(
        static_cast<unsigned long long>(self->stats.avg_round_trip_ns()));
}

static PyGetSetDef PyPipeStats_getset[] = {
    {"messages_sent",     reinterpret_cast<getter>(PyPipeStats_get_messages_sent),
     nullptr, "Sent message count", nullptr},
    {"messages_received", reinterpret_cast<getter>(PyPipeStats_get_messages_received),
     nullptr, "Received message count", nullptr},
    {"bytes_sent",        reinterpret_cast<getter>(PyPipeStats_get_bytes_sent),
     nullptr, "Sent payload bytes", nullptr},
    {"bytes_received",    reinterpret_cast<getter>(PyPipeStats_get_bytes_received),
     nullptr, "Received payload bytes", nullptr},
    {"errors",            reinterpret_cast<getter>(PyPipeStats_get_errors),
     nullptr, "PipeException count", nullptr},
    {"rpc_calls",         reinterpret_cast<getter>(PyPipeStats_get_rpc_calls),
     nullptr, "Successful send_request() count (RpcPipeClient only)", nullptr},
    {"rtt_total_ns",      reinterpret_cast<getter>(PyPipeStats_get_rtt_total_ns),
     nullptr, "Total RTT nanoseconds (RpcPipeClient only)", nullptr},
    {"rtt_last_ns",       reinterpret_cast<getter>(PyPipeStats_get_rtt_last_ns),
     nullptr, "Last RTT nanoseconds (RpcPipeClient only)", nullptr},
    {"avg_round_trip_ns", reinterpret_cast<getter>(PyPipeStats_get_avg_round_trip_ns),
     nullptr, "Average RTT nanoseconds; 0 if rpc_calls == 0", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}
};

// ─── PyTypeObject ─────────────────────────────────────────────────────

PyTypeObject PyPipeStats_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "_pipeutil.PipeStats",
    sizeof(PyPipeStats),
    0,
    reinterpret_cast<destructor>(PyPipeStats_dealloc),
    0, nullptr, nullptr, nullptr,
    reinterpret_cast<reprfunc>(PyPipeStats_repr),
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT,
    "PipeStats — diagnostics/metrics snapshot returned by stats().\n"
    "All properties are read-only integers.",
    nullptr, nullptr, nullptr, 0, nullptr, nullptr,
    nullptr,     // tp_methods
    nullptr,     // tp_members
    PyPipeStats_getset,
    nullptr, nullptr, nullptr, nullptr, 0,
    nullptr,     // tp_init (不変: __init__ 不要)
    nullptr,
    PyPipeStats_new,
};

// ─── ヘルパー関数 ─────────────────────────────────────────────────────

PyObject* PyPipeStats_from_stats(const pipeutil::PipeStats& stats) {
    PyPipeStats* obj = reinterpret_cast<PyPipeStats*>(
        PyPipeStats_Type.tp_alloc(&PyPipeStats_Type, 0));
    if (!obj) return nullptr;
    new (&obj->stats) pipeutil::PipeStats(stats);
    return reinterpret_cast<PyObject*>(obj);
}

} // namespace pyutil
