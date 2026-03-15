// py_capability.hpp — PyNegotiatedCapabilities 型宣言 (A-001)
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pipeutil/capability.hpp"

namespace pyutil {

// ─── PyNegotiatedCapabilities ─────────────────────────────────────────────
// C 拡張型。HELLO 交換後の合意済み機能セットを Python へ公開する。
// Python 側からは _pipeutil.NegotiatedCapabilities として参照される。

struct PyNegotiatedCapabilities {
    PyObject_HEAD
    uint32_t bitmap;      // 合意済み capability ビット
    int      v1_compat;   // 1 = v1.0.0 クライアント（version=0x01 で接続）
};

extern PyTypeObject PyNegotiatedCapabilities_Type;

/// pipeutil::NegotiatedCapabilities から PyNegotiatedCapabilities を生成する。
/// 戻り値: 新規参照 (nullptr = メモリ不足)
PyObject* PyNegotiatedCapabilities_from_caps(
    const pipeutil::NegotiatedCapabilities& caps);

} // namespace pyutil
