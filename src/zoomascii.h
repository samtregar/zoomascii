static PyObject* swapcase(PyObject* self, PyObject* args);
static PyObject* b2a_qp(PyObject* self, PyObject* args, PyObject *kwargs);

static PyMethodDef ZoomMethods[] = {
  {"swapcase", swapcase, METH_VARARGS, "ASCII swap case, fast."},
  {"b2a_qp", (PyCFunction)b2a_qp, METH_VARARGS | METH_KEYWORDS, "Binary to qp, fast."},  
  {NULL, NULL, 0, NULL}
};
