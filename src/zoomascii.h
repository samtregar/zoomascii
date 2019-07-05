static PyObject* swapcase(PyObject* self, PyObject* args);
static PyObject* b2a_qp(PyObject* self, PyObject* args, PyObject *kwargs);

static PyMethodDef ZoomMethods[] = {
  {"swapcase", swapcase, METH_VARARGS, "ASCII swap case, fast."},
  {"b2a_qp", (PyCFunction)b2a_qp, METH_VARARGS | METH_KEYWORDS, "Binary to qp, fast."},  
  {NULL, NULL, 0, NULL}
};

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef zoomasciimodule = {
    PyModuleDef_HEAD_INIT,
    "zoomascii",   /* name of module */
    NULL, /* module documentation, may be NULL */
    -1,       /* size of per-interpreter state of the module,
                 or -1 if the module keeps state in global variables. */
    ZoomMethods
};
#endif
