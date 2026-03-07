// py_multi_pipe_server.hpp — PyMultiPipeServer 型宣言
// 仕様: spec/F001_multi_pipe_server.md §5
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "pipeutil/multi_pipe_server.hpp"

namespace pyutil {

// ─── PyMultiPipeServer 構造体 ─────────────────────────────────────────────
// Python オブジェクトレイアウト: PipeServer と同じパターンに倣う。
// server は nullptr = 未初期化 or 解放済みを表す。

struct PyMultiPipeServer {
    PyObject_HEAD
    pipeutil::MultiPipeServer* server;  // nullptr = closed / not initialized
};

extern PyTypeObject PyMultiPipeServer_Type;

} // namespace pyutil
