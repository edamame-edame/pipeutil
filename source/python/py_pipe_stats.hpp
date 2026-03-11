// py_pipe_stats.hpp — PyPipeStats 型宣言
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pipeutil/pipe_stats.hpp"

namespace pyutil {

/// PipeStats 値型を Python オブジェクトとして公開する読み取り専用データクラス。
/// stats() が返すスナップショットを保持する不変オブジェクト。
typedef struct {
    PyObject_HEAD
    pipeutil::PipeStats stats;  // 値コピー（スナップショット）
} PyPipeStats;

extern PyTypeObject PyPipeStats_Type;

/// C++ PipeStats から PyPipeStats を構築して返す（参照カウント +1）。
/// 失敗時は nullptr を返し、Python 例外をセットする。
PyObject* PyPipeStats_from_stats(const pipeutil::PipeStats& stats);

} // namespace pyutil
