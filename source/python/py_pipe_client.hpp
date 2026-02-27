// py_pipe_client.hpp — PyPipeClient 型宣言
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pipeutil/pipe_client.hpp"

namespace pyutil {

struct PyPipeClient {
    PyObject_HEAD
    pipeutil::PipeClient* client;  // nullptr = closed / uninitialized
};

extern PyTypeObject PyPipeClient_Type;

} // namespace pyutil
