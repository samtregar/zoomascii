#include <Python.h>
#include "zoomascii.h"

#ifndef INLINE
# if __GNUC__ && !__GNUC_STDC_INLINE__
#  define INLINE extern inline
# else
#  define INLINE inline
# endif
#endif

#if __GNUC__ >= 3
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

#if PY_MAJOR_VERSION < 3
#define PyBytes_FromStringAndSize PyString_FromStringAndSize
#define PyBytes_AS_STRING PyString_AS_STRING
#define _PyBytes_Resize _PyString_Resize
#endif

// support Python <3.9
#if PY_VERSION_HEX < 0x030900A4 && !defined(Py_SET_SIZE)
static inline void _Py_SET_SIZE(PyVarObject *ob, Py_ssize_t size) { ob->ob_size = size; }
#define Py_SET_SIZE(ob, size) _Py_SET_SIZE((PyVarObject*)(ob), size)
#endif


static PyObject*
swapcase(PyObject* self, PyObject* args) {
    Py_buffer input_buf;
    const unsigned char* input;
    char *output;
    Py_ssize_t len, i, front_len;
    PyObject *ret;

    // get the input string without copying it
    if (!PyArg_ParseTuple(args, "s*", &input_buf))
        return NULL;
    input = input_buf.buf;
    len = input_buf.len;

    // get a string to work on, in a format we can return directly
    // without more copying
    ret = PyBytes_FromStringAndSize(NULL, len);
    output = PyBytes_AS_STRING(ret);

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


INLINE void do_encode_qp(const unsigned char c, char *output) {
  unsigned char c2;
  output[0] = '=';
  c2 = (c >> 4) & 0xf;
  output[1] = (c2 > 9) ? c2 + 'A' - 10 : c2 + '0';
  c2 = c & 0xf;
  output[2] = (c2 > 9) ? c2 + 'A' - 10 : c2 + '0';
}


static INLINE void encode_qp(const unsigned char c, char *output) {
  // Direct assignment is faster than memcpy for 3 bytes
  const char *qp_entry = qp_table[(unsigned int) c];
  output[0] = qp_entry[0];
  output[1] = qp_entry[1];
  output[2] = qp_entry[2];
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

#define CR 13
#define LF 10
#define MAX_LINE_LENGTH 72
#define NEEDS_ENCODE(x) (x < 33 || x > 126 || x == 61)

static PyObject*
b2a_qp(PyObject *self, PyObject *args, PyObject *kwargs) {
  Py_buffer input_buf;
  PyObject *ret;
  char *input, *output, c;
  int input_len, i, j, x, output_len, line_len, max_x;

  static char *kwlist[] = {"string", "encode_leading_dot", NULL};
  int encode_leading_dot = 1;
  
  // get the input string without copying it
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s*|i", kwlist,
                                   &input_buf, &encode_leading_dot))
    return NULL;
  input = input_buf.buf;
  input_len = input_buf.len;
  
  assert(input_len >= 0);
  
  // Special case for empty input - return empty bytes directly
  if (input_len == 0) {
    PyBuffer_Release(&input_buf);
    return PyBytes_FromStringAndSize("", 0);
  }
  
  if (input_len > PY_SSIZE_T_MAX / 2) {
    PyBuffer_Release(&input_buf);
    return PyErr_NoMemory();
  }
  
  // get a string to work on, in a format we can return directly
  // without more copying - start with a string twice as large, could
  // be more careful here and use less memory
  output_len = input_len*2;
  output_len = roundUp4k(output_len);
  
  // Special case for empty input
  if (output_len == 0) output_len = 1;

  ret = PyBytes_FromStringAndSize(NULL, output_len);

  if (!ret) {
    PyBuffer_Release(&input_buf);
    return NULL;
  }

  output = PyBytes_AS_STRING(ret);
  
  if (!output) {
    PyBuffer_Release(&input_buf);
    Py_DECREF(ret);
    return NULL;
  }

  j = 0;
  line_len = 0;
  for (i = 0; i < input_len; i++) {
    // check that output doesn't need to be resized - we need at least
    // three characters so we can write out an escape
    if (unlikely(j+3 > output_len)) {
      // get another 4k and realloc the string
      output_len = roundUp4k(j+3);
      if (_PyBytes_Resize(&ret, output_len) == -1) {
        PyBuffer_Release(&input_buf);
        Py_DECREF(ret); 
        return NULL;
      }
      output = PyBytes_AS_STRING(ret);
    }
    
    c = input[i];
    if (unlikely(c == '.' && line_len == 0 && encode_leading_dot)) {
      // not actually part of QP encoding but SMTP needs this - encode
      // leading . on line
      encode_qp(c, output+j);
      j+=3;
      line_len+=3;
    } else if (likely(!NEEDS_ENCODE(c))) {
      // see if we can memcpy a bunch of the string all at once -
      // faster than doing it char by char
      max_x = MAX_LINE_LENGTH-line_len;

      // make sure this won't run us over our input or output limits
      if (unlikely(i+max_x >= input_len))
        max_x = input_len-i-1;
      if (unlikely(j+max_x >= output_len))
        max_x = output_len-j-1;
      
      // Optimized scan for consecutive non-encoded characters
      for(x = 1; x < max_x; x++) {
        char next_char = input[i+x];
        if (unlikely(NEEDS_ENCODE(next_char))) {
          // special-case spaces here since they're very common, we
          // can memcpy them unless they're at the end or followed by
          // a CR
          if (likely(next_char == ' ' && x+1 < max_x && input[i+x+1] != CR))
            continue;
          break;
        }
      }
      
      memcpy(output+j, input+i, x);
      j += x;
      line_len += x;
      i += x-1;
      
    } else if (c == ' ' || c == '\t') {
      // space or tab is ok unless the next sequence is a CRLF or at the end
      if (unlikely(i+2 > input_len || (input[i+1] == CR && input[i+2] == LF))) {
        encode_qp(c, output+j);
        j+=3;
        line_len+=3;
      } else {
        output[j++] = c;
        line_len++;
      }
    } else if (unlikely(c == CR && i+1 < input_len && input[i+1] == LF)) {
      // CRLF can go as-is
      memcpy(output+j, "\r\n", 2);
      j += 2;
      i++;
      line_len = 0;
    } else {
      // encode all other chars
      encode_qp(c, output+j);
      j+=3;
      line_len+=3;      
    }

    // soft line break at max
    if (unlikely(line_len >= MAX_LINE_LENGTH)) {
      memcpy(output+j, "=\r\n", 3);
      j += 3;
      line_len = 0;
    }
     
  }

  // shorten the string by assigning to size directly - safe since we
  // handled empty case above
  Py_SET_SIZE(ret, j);
                  
  PyBuffer_Release(&input_buf);
  return ret;
}




#if PY_MAJOR_VERSION < 3
PyMODINIT_FUNC initzoomascii(void) {
  (void) Py_InitModule("zoomascii", ZoomMethods);
}
#else
PyMODINIT_FUNC
PyInit_zoomascii(void)
{
    return PyModule_Create(&zoomasciimodule);
}
#endif
