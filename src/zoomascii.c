#include <Python.h>
#include "zoomascii.h"

#ifndef INLINE
# if __GNUC__ && !__GNUC_STDC_INLINE__
#  define INLINE extern inline
# else
#  define INLINE inline
# endif
#endif

/* could just inline this table, but I'm lazy, maybe when it's release-ready */
unsigned char swapcase_table[256];
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
}

static PyObject*
swapcase(PyObject* self, PyObject* args) {
    PyObject *input_obj;
    const unsigned char* input;
    char *output;
    Py_ssize_t len, i, front_len;
    PyObject *ret;

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


INLINE void encode_qp(char c, char *output, int *j_in) {
  char c2;
  int j = *j_in;
  output[j] = '=';
  c2 = (c >> 4) & 0xf;
  output[j+1] = (c2 > 9) ? c2 + 'A' - 10 : c2 + '0';
  c2 = c & 0xf;
  output[j+2] = (c2 > 9) ? c2 + 'A' - 10 : c2 + '0';
  *j_in = j + 3;
}

// used to round up the output buffer sizes to 4k + 1 so we always
// have room to work without frequent reallocs and checks
int roundUp4k(int numToRound) {
  int multiple = 4 * 1024;
  
  int remainder = numToRound % multiple;
  if (remainder == 0)
    return numToRound;
  
  return numToRound + multiple - remainder;
}

INLINE void check_and_resize_output(PyObject *ret, char **output, int *output_len, int *j) {
    // check that output doesn't need to be resized
    if (*j >= *output_len) {
      // get another 4k and realloc the string
      *output_len = roundUp4k(*output_len+1);
      _PyString_Resize(&ret, *output_len);
      *output = PyString_AS_STRING(ret);
    }
}

#define CR 13
#define LF 10
#define MAX_LINE_LENGTH 72

static PyObject*
b2a_qp(PyObject* self, PyObject* args) {
  Py_buffer input_buf;
  PyObject *ret;
  char *input, *output, c, cx;
  int input_len, i, j, x, output_len, line_len;

  // get the input string without copying it
  if (!PyArg_ParseTuple(args, "s*", &input_buf))
    return NULL;
  input = input_buf.buf;
  input_len = input_buf.len;
  
  assert(input_len >= 0);
  if (input_len > PY_SSIZE_T_MAX / 2) {
    PyBuffer_Release(&input_buf);
    return PyErr_NoMemory();
  }
  
  // get a string to work on, in a format we can return directly
  // without more copying - start with a string twice as large, could
  // be more careful here and use less memory
  output_len = input_len*2;
  output_len = roundUp4k(output_len);
  ret = PyString_FromStringAndSize(NULL, output_len);
  if (!ret) {
    PyBuffer_Release(&input_buf);
    return NULL;
  }

  output = PyString_AS_STRING(ret);
  if (!output) {
    PyBuffer_Release(&input_buf);
    Py_DECREF(ret);
    return NULL;
  }

  j = 0;
  line_len = 0;
  for (i = 0; i < input_len; i++) {
    check_and_resize_output(ret, &output, &output_len, &j);
    
    c = input[i];
    if (c == '.' && line_len == 0) {
      // not actually part of QP encoding but SMTP needs this - encode
      // leading . on line
      encode_qp(c, output, &j);
      line_len+=3;
    } else if ((c >= 33 && c <= 60) || (c >= 62 && c <= 126)) {
      // see if we can memcpy a bunch of the string all at once -
      // faster than doing it char by char
      for(x = 1; x < MAX_LINE_LENGTH-line_len; x++) {
        cx = input[i+x];
        if (cx < 33 || cx > 126 || cx == 61)
          break;
      }
      j += x;
      memcpy(output+j-x, input+i, x);
      line_len += x;
      i += x-1;
      
    } else if (c == ' ' || c == '\t') {
      // space or tab is ok unless the next sequence is a CRLF
      if (i < input_len+2 && input[i+1] == CR && input[i+2] == LF) {
        encode_qp(c, output, &j);
        line_len+=3;
      } else {
        output[j++] = c;
        line_len++;
      }
    } else if (c == CR && i < input_len+1 && input[i+1] == LF) {
      // CRLF can go as-is
      output[j] = CR;
      output[j+1] = LF;
      j += 2;
      i++;
      line_len = 0;
    } else {
      encode_qp(c, output, &j);
      line_len+=3;      
    }

    if (line_len >= MAX_LINE_LENGTH) {
      output[j] = '=';
      output[j+1] = CR;
      output[j+2] = LF;
      j += 3;
      line_len = 0;
    }
     
  }

  // check that output doesn't need to be resized before NULL
  check_and_resize_output(ret, &output, &output_len, &j);
  
  // NULL terminate
  output[j] = 0;
    
  // shorten the string by assigning to size directly - seems to work
  // fine, may not be 100% legal, need to check for memory leaks with
  // this method.
  Py_SIZE(ret) = j;
                  
  PyBuffer_Release(&input_buf);
  return ret;
}

PyMODINIT_FUNC initzoomascii(void) {
  (void) Py_InitModule("zoomascii", ZoomMethods);
  _do_swapcase_init();
}
