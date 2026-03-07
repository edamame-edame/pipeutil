// py_rpc_pipe_server.hpp — PyRpcPipeServer 型宣言
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pipeutil/rpc_pipe_server.hpp"

namespace pyutil {

struct PyRpcPipeServer {
    PyObject_HEAD
    pipeutil::RpcPipeServer* server;  // nullptr = closed / uninitialized
};

extern PyTypeObject PyRpcPipeServer_Type;

} // namespace pyutil
