add_subdirectory(core)

option(PIPEUTIL_WITH_ASYNC
    "Build _pipeutil_async native async I/O extension (requires PIPEUTIL_BUILD_PYTHON=ON)"
    OFF
)

if(PIPEUTIL_BUILD_PYTHON)
    find_package(Python3 3.13 REQUIRED COMPONENTS Interpreter Development.Module)
    add_subdirectory(python)
endif()
