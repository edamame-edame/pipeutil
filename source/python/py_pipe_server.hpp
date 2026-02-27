// py_pipe_server.hpp — PyPipeServer 型宣言
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pipeutil/pipe_server.hpp"

namespace pyutil {

struct PyPipeServer {
    PyObject_HEAD
    pipeutil::PipeServer* server;  // nullptr = closed / uninitialized
};

extern PyTypeObject PyPipeServer_Type;

} // namespace pyutil
