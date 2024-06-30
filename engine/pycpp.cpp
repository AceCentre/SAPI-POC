#include "pycpp.h"
#include "slog.h"

#include <stdexcept>
#include <fmt/printf.h>
#include <span>

using namespace pycpp;

#if 0
PythonVM::PythonVM() {
    PyPreConfig config;
    PyPreConfig_InitIsolatedConfig(&config);
    config.isolated = 1;
    config.utf8_mode = 1;

    // create CPython instance without registering signal handlers
    PyStatus status = Py_PreInitialize(&config);
    if (PyStatus_Exception(status)) {
        throw std::runtime_error("Failed to initialize Python");
    }

    Py_InitializeEx(0);
}
#else
PythonVM::PythonVM() {
    slog("PythonVM::PythonVM");

    // call once idiom
    static auto _ = [this]() {
        // Initialize the pre-configuration structure
        PyPreConfig preconfig;
        PyPreConfig_InitIsolatedConfig(&preconfig);
        preconfig.utf8_mode = 1;

        // Apply the pre-configuration settings
        PyStatus status = Py_PreInitialize(&preconfig);
        if (PyStatus_Exception(status)) {
            throw std::runtime_error("Failed to pre-initialize Python");
        }

        // Initialize the configuration structure
        PyConfig config;
        PyConfig_InitIsolatedConfig(&config);

        // Example configuration adjustments
        config.install_signal_handlers = 0;  // Disable signal handlers
        config.use_environment = 0;  // Ignore environment variables

        // Apply the configuration settings
        status = Py_InitializeFromConfig(&config);
        if (PyStatus_Exception(status)) {
            throw std::runtime_error("Failed to initialize Python");
        }

        // Free the configuration structure (no longer needed)
        PyConfig_Clear(&config);

        thread_state_ = PyEval_SaveThread();

        return 0;
    }();
}
#endif

PythonVM::~PythonVM() {
    slog("PythonVM::~PythonVM");
    //PyEval_RestoreThread(thread_state_);
    //Py_FinalizeEx();
}

#if 0
ExceptionInfo pycpp::get_exception_info() {
    assert(PyErr_Occurred());
    ExceptionInfo info;

    // Declare variables to hold the exception info
    PyObject *ptype, *pvalue, *ptraceback;

    // Fetch the exception info
    PyErr_Fetch(&ptype, &pvalue, &ptraceback);

    // Normalize the exception (optional but recommended)
    PyErr_NormalizeException(&ptype, &pvalue, &ptraceback);

    // Get the string representation of the exception type
    PyObject *ptype_str = PyObject_Str(ptype);
    const char *type_str = PyUnicode_AsUTF8(ptype_str);

    // Get the string representation of the exception value
    PyObject *pvalue_str = PyObject_Str(pvalue);
    const char *value_str = PyUnicode_AsUTF8(pvalue_str);

    info.type = type_str;
    info.value = value_str;

    // Clean up
    Py_XDECREF(ptype);
    Py_XDECREF(pvalue);
    Py_XDECREF(ptraceback);
    Py_XDECREF(ptype_str);
    Py_XDECREF(pvalue_str);

    // Clear the error indicator
    PyErr_Clear();

    return info;
}
#else
ExceptionInfo pycpp::get_exception_info() {
    assert(PyErr_Occurred());
    ExceptionInfo info;

    PyObject *exception = PyErr_GetRaisedException();
    assert(exception);

    PyObject *exception_str = PyObject_Str(exception);
    const char *exception_cstr = PyUnicode_AsUTF8(exception_str);

    info.value = exception_cstr;

    // Clean up
    Py_DECREF(exception);
    Py_XDECREF(exception_str);

    return info;
}
#endif

void pycpp::throw_on_error() {
    if (PyErr_Occurred()) {
        throw PythonException(get_exception_info());
    }
}

bool pycpp::append_to_syspath(std::wstring_view path) {
    Obj sys_module_obj {PyImport_ImportModule("sys")};
    assert(PyModule_Check(sys_module_obj));

    Obj path_list_obj {PyObject_GetAttrString(sys_module_obj, "path")};
    assert(PyList_Check(path_list_obj));

    Obj path_obj {convert(path)};

    Py_ssize_t size = PyList_Size(path_list_obj);
    slog("path_list_obj size: {}", size);

    for (Py_ssize_t i = 0; i < size; i++) {
        Obj path_i {incref(PyList_GetItem(path_list_obj, i))};
        assert(PyUnicode_Check(path_i) == 1);
        if (PyUnicode_Compare(path_obj, path_i) == 0) {
            return false;
        }
    }

    if (PyList_Append(path_list_obj, path_obj) == -1) {
        throw PythonException("PyList_Append failed");
    }

    return true;
}

PyObject* pycpp::convert(std::string_view value) {
    PyObject *o = PyUnicode_FromStringAndSize(value.data(), value.size());
    assert(o);
    return o;
}

PyObject* pycpp::convert(std::wstring_view value) {
    PyObject* o = PyUnicode_FromWideChar(value.data(), value.size());
    assert(o);
    return o;
}

PyObject* pycpp::bytes(std::span<const char> value) {
    PyObject *o = PyBytes_FromStringAndSize(value.data(), value.size());
    assert(o);
    return o;
}

