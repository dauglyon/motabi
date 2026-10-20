#define PY_SSIZE_T_CLEAN
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <Python.h>
#include <numpy/arrayobject.h>
/* NumPy array buffers go through this handler, which forwards to Python's RAW
   domain, where foldalloc's wrapper sits: at home that is plain malloc, in a
   branch it is the branch's range. */
static void *np_malloc(void *c, size_t n){ return PyMem_RawMalloc(n); }
static void *np_calloc(void *c, size_t a, size_t b){ return PyMem_RawCalloc(a, b); }
static void *np_realloc(void *c, void *p, size_t n){ return PyMem_RawRealloc(p, n); }
static void np_free(void *c, void *p, size_t n){ PyMem_RawFree(p); }
static PyDataMem_Handler H = { "foldalloc", 1, { NULL, np_malloc, np_calloc, np_realloc, np_free } };
static PyObject *py_divert(PyObject *s, PyObject *a){
  PyObject *cap = PyCapsule_New(&H, "mem_handler", NULL); if (!cap) return NULL;
  PyObject *old = PyDataMem_SetHandler(cap); Py_DECREF(cap); if (!old) return NULL; Py_DECREF(old); Py_RETURN_NONE;
}
static PyMethodDef M[] = { {"divert", py_divert, METH_NOARGS, ""}, {NULL, NULL, 0, NULL} };
static struct PyModuleDef mod = { PyModuleDef_HEAD_INIT, "npdivert", NULL, -1, M };
PyMODINIT_FUNC PyInit_npdivert(void){ import_array(); return PyModule_Create(&mod); }
