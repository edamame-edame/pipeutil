// py_rpc_pipe_client.hpp — PyRpcPipeClient 型宣言
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pipeutil/rpc_pipe_client.hpp"

namespace pyutil {

struct PyRpcPipeClient {
    PyObject_HEAD
    pipeutil::RpcPipeClient* client;  // nullptr = closed / uninitialized
};

extern PyTypeObject PyRpcPipeClient_Type;

} // namespace pyutil
