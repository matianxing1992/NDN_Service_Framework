#ifndef NDNSF_PYTHON_DI_BINDINGS_HPP
#define NDNSF_PYTHON_DI_BINDINGS_HPP

#include <pybind11/pybind11.h>

#include <memory>

using PyFunctionPtr = std::shared_ptr<pybind11::function>;

PyFunctionPtr keepPyFunction(pybind11::function fn);

void bindDistributedInference(pybind11::module_& module);

#endif // NDNSF_PYTHON_DI_BINDINGS_HPP
