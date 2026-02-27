add_subdirectory(core)

if(PIPEUTIL_BUILD_PYTHON)
    find_package(Python3 3.13 REQUIRED COMPONENTS Interpreter Development.Module)
    add_subdirectory(python)
endif()
