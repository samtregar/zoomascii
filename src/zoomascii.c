#include <Python.h>

/* could just inline this table, but I'm lazy, maybe when it's release-ready */
unsigned char swapcase_table[256];
unsigned int swapcase_init_done = 0;

void _do_swapcase_init(void) {
  unsigned int c = 0;
  do {
    if (c>='A' && c<='Z') {
      swapcase_table[c] = c + 32;
    } else if (c>='a' && c<='z') {
      swapcase_table[c] = c - 32;
    } else {
      swapcase_table[c] = c;
    }
  } while (c++ != 255);

  swapcase_init_done = 1;
}

static PyObject*
swapcase(PyObject* self, PyObject* args) {
    PyObject *input_obj;
    const unsigned char* input;
    char *output;
    int len, i, front_len;
    PyObject *ret;

    // build the lookup table first time through
    if (!swapcase_init_done)
      _do_swapcase_init();

    // get the input string without copying it
    if (!PyArg_ParseTuple(args, "S", &input_obj))
        return NULL;
    input = (unsigned char *) PyString_AS_STRING(input_obj);
    len = PyString_Size(input_obj);
    
    // get a string to work on, in a format we can return directly
    // without more copying
    ret = PyString_FromStringAndSize(NULL, len);
    output = PyString_AS_STRING(ret);

    // process 64bit chunks, worth around 20% speedup in testing
    front_len = len - (len % 8);
    for (i = 0; i < front_len; i+=8) {
      output[i]   = swapcase_table[input[i]];
      output[i+1] = swapcase_table[input[i+1]];
      output[i+2] = swapcase_table[input[i+2]];
      output[i+3] = swapcase_table[input[i+3]];
      output[i+4] = swapcase_table[input[i+4]];
      output[i+5] = swapcase_table[input[i+5]];
      output[i+6] = swapcase_table[input[i+6]];
      output[i+7] = swapcase_table[input[i+7]];
    }

    // cleanup the rest
    for (i = front_len; i < len; i++)
      output[i] = swapcase_table[input[i]];

    return ret;
}

static PyMethodDef ZoomMethods[] =
{
     {"swapcase", swapcase, METH_VARARGS, "ASCII swap case, fast."},
     {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
initzoomascii(void)
{
     (void) Py_InitModule("zoomascii", ZoomMethods);
}
