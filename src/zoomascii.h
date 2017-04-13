static PyObject* swapcase(PyObject* self, PyObject* args);
static PyObject* b2a_qp(PyObject* self, PyObject* args);

static PyMethodDef ZoomMethods[] = {
  {"swapcase", swapcase, METH_VARARGS, "ASCII swap case, fast."},
  {"b2a_qp", b2a_qp, METH_VARARGS, "Binary to qp, fast."},  
  {NULL, NULL, 0, NULL}
};
