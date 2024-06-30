#pragma once

#include <Python.h>
#include <cassert>
#include <string>
#include <string_view>
#include <span>

namespace pycpp {

class Obj;

void throw_on_error();

PyObject* convert(std::string_view value);
PyObject* convert(std::wstring_view value);
PyObject* bytes(std::span<const char> value);

inline PyObject* incref(PyObject* o) {
    throw_on_error();
    assert(o);
    Py_INCREF(o);
    return o;
}

bool append_to_syspath(std::wstring_view path);

class PythonVM {
public:
    PythonVM();
    ~PythonVM();
private:
    PyThreadState* thread_state_;
};

class Obj {
public:
    Obj() : o_(nullptr) {}

    ~Obj() {
        if (o_ != nullptr) {
            decref();
        }
    }

    explicit Obj(PyObject* o) {
        throw_on_error();
        assert(o);
        o_ = o;
    }

    Obj(const Obj& o) {
        if (o != o_) {
            decref();
            o_ = o;
        }
    }

    const Obj& operator=(PyObject* o) {
        throw_on_error();
        assert(o);
        if (o != o_) {
            decref();
            o_ = o;
        }

        return *this;
    }

    void reset(PyObject* o) {
        throw_on_error();
        assert(o);
        if (o != o_) {
            decref();
            o_ = o;
            Py_XINCREF(o_);
        }
    }

    PyObject* reset() {
        throw_on_error();
        decref();
        PyObject* o = o_;
        o_ = nullptr;
        return o;
    }

    void decref() {
        assert(!o_ || (o_ && (Py_REFCNT(o_) > 0)));
        Py_XDECREF(o_);
    }

    const char* type_name() const {
      assert(o_);
      return o_->ob_type->tp_name;
    }

    PyObject* ptr() const {
        return o_;
    }

    operator PyObject*() const {
        return o_;
    }

    operator bool() {
        return o_ != nullptr;
    }

private:
    PyObject* o_;
};

struct ExceptionInfo {
    // std::string type;
    std::string value;

    operator bool() const {
        return !value.empty();
    }
};

ExceptionInfo get_exception_info();

class PythonException : public std::exception
{
public:
    PythonException(const ExceptionInfo &info)
        : info_(info) {}

    PythonException(std::string&& info)
        : info_({std::move(info)}) {
    }

    ~PythonException() throw() {}

    const char *what() const throw()
    {
        return info_.value.c_str();
    }

private:
    const ExceptionInfo info_;
};

class ScopedGIL
{
public:
    ScopedGIL()
    {
        state_ = PyGILState_Ensure();
    }

    ~ScopedGIL()
    {
        PyGILState_Release(state_);
    }

private:
    PyGILState_STATE state_;
};

} // namespace pycpp
