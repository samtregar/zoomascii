#include <Python.h>
#include "zoomascii.h"

#ifdef __SSE2__
#include <emmintrin.h>
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


// used to round up the output buffer sizes to 4k so we always
// have room to work without frequent reallocs and checks
static Py_ssize_t roundUp4k(Py_ssize_t numToRound) {
  Py_ssize_t multiple = 4 * 1024;
  Py_ssize_t remainder = numToRound % multiple;
  if (remainder == 0)
    return numToRound;
  return numToRound + multiple - remainder;
}

#define CR 13
#define LF 10
#define MAX_LINE_LENGTH 72
#define OUTPUT_SLACK 128

static PyObject*
b2a_qp(PyObject *self, PyObject *args, PyObject *kwargs) {
  Py_buffer input_buf;
  PyObject *ret;
  const unsigned char *input;
  char *output;
  Py_ssize_t input_len, output_len, i, j, x, room, avail;
  int line_len;
  unsigned char c;

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
  output_len = roundUp4k(input_len * 2);

  ret = PyBytes_FromStringAndSize(NULL, output_len);
  if (!ret) {
    PyBuffer_Release(&input_buf);
    return NULL;
  }
  output = PyBytes_AS_STRING(ret);

  i = 0;
  j = 0;
  line_len = 0;
  while (i < input_len) {
    // make sure one more iteration can't run off the end of the
    // output: a plain run and the escapes following it share the
    // MAX_LINE_LENGTH budget. The 16-byte vector stores below can
    // overshoot the run by up to 16 bytes, and an =XX escape or soft
    // line break is 3 bytes plus a NUL. Checking
    // once per iteration with a generous margin keeps the branch
    // predictable and out of the inner loops.
    if (unlikely(j + OUTPUT_SLACK > output_len)) {
      // get another 4k and realloc the string
      output_len = roundUp4k(j + OUTPUT_SLACK);
      if (_PyBytes_Resize(&ret, output_len) == -1) {
        PyBuffer_Release(&input_buf);
        return NULL;
      }
      output = PyBytes_AS_STRING(ret);
    }

    c = input[i];

    if (likely(qp_plain[c])) {
      const unsigned char *p = input + i;
      char *q = output + j;

      // not actually part of QP encoding but SMTP needs this - encode
      // leading . on line
      if (unlikely(c == '.' && line_len == 0 && encode_leading_dot))
        goto encode;

      // copy a run of plain characters straight through. The run is
      // bounded by the space left on the line (line_len is always
      // < MAX_LINE_LENGTH here, so room >= 1) and by the end of input.
      room = MAX_LINE_LENGTH - line_len;
      avail = input_len - i;
      x = 0;

#ifdef __SSE2__
      // classify and copy 16 bytes per iteration. Each _mm_* intrinsic
      // compiles to a single instruction operating on all 16 bytes of
      // a 128-bit register at once. The bytes are stored to the
      // output before we know how many of them belong to the run -
      // that's fine because the output buffer has slack and anything
      // past the run gets overwritten by whatever comes next.
      //
      // Compute the final permitted load position once: each load
      // must fit in the input and start before the line is full.
      // This leaves just one bounds check per vector. A tail shorter
      // than 16 bytes falls through to the scalar loop below.
      Py_ssize_t vector_limit = avail - 16;
      if (vector_limit >= room)
        vector_limit = room - 1;
      while (x <= vector_limit) {
        __m128i v = _mm_loadu_si128((const __m128i*)(p + x));
        __m128i plain;
        unsigned int mask;

        _mm_storeu_si128((__m128i*)(q + x), v);

        // Wrapping addition maps bytes 32..126 to signed -128..-34.
        // One signed comparison then checks both ends of the ASCII
        // range; control bytes and bytes >= 127 fail the comparison.
        plain = _mm_cmplt_epi8(_mm_add_epi8(v, _mm_set1_epi8(96)),
                               _mm_set1_epi8(-33));
        // andnot(a, b) = (NOT a) AND b - knock '=' back out of the
        // plain set ...
        plain = _mm_andnot_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8(61)), plain);
        // ... and OR tab back in
        plain = _mm_or_si128(plain, _mm_cmpeq_epi8(v, _mm_set1_epi8(9)));

        // movemask packs the top bit of each byte lane into a 16-bit
        // int: bit n set means byte n is plain. XOR flips it so a
        // set bit means "needs encoding" and mask == 0 means all 16
        // bytes are plain.
        mask = _mm_movemask_epi8(plain) ^ 0xFFFF;
        if (mask) {
          // count-trailing-zeros gives the index of the lowest set
          // bit, i.e. the offset of the first byte needing encoding
          x += __builtin_ctz(mask);
          goto scanned;
        }
        x += 16;
      }
#endif
      {
        Py_ssize_t limit = room < avail ? room : avail;
        while (x < limit && qp_plain[p[x]]) {
          q[x] = p[x];
          x++;
        }
      }

#ifdef __SSE2__
    scanned:
#endif
      // the 16-at-a-time strides can overshoot the line-length limit
      if (x > room)
        x = room;

      // back off a trailing space or tab that lands before a CRLF or
      // the end of the input so it gets encoded instead. x >= 1 here
      // since p[0] is plain. If the run was that single space, x drops
      // to 0 and the character is encoded below.
      if (unlikely((p[x-1] == ' ' || p[x-1] == '\t') &&
                   (x == avail ||
                    (p[x] == CR && x + 1 < avail && p[x+1] == LF)))) {
        x--;
        if (x == 0)
          goto encode;
      }

      i += x;
      j += x;
      line_len += x;

      // A run normally ends at a byte needing encoding. Handle it
      // immediately while there is still room on this line.
      if (x == room || i == input_len)
        goto line_break;
      c = input[i];
    }

    if (c == CR && i + 1 < input_len && input[i+1] == LF) {
      // CRLF can go as-is and resets the line
      output[j] = CR;
      output[j+1] = LF;
      j += 2;
      i += 2;
      line_len = 0;
      continue;
    }
  encode:
    // encode all other chars as =XX. The table entries are 4 bytes
    // (with a NUL) so this is a single 32-bit store; the extra byte
    // is overwritten by the next output. Keep going while the
    // following bytes also need encoding (multi-byte UTF-8 text)
    // rather than paying the outer loop overhead for each one; the
    // line-length check below is what bounds the output written.
    for (;;) {
      memcpy(output + j, qp_table[c], 4);
      j += 3;
      i++;
      line_len += 3;
      if (line_len >= MAX_LINE_LENGTH || i >= input_len)
        break;
      c = input[i];
      if (qp_plain[c] || c == CR)
        break;
    }

  line_break:
    // soft line break at max
    if (unlikely(line_len >= MAX_LINE_LENGTH)) {
      memcpy(output + j, "=\r\n", 4);
      j += 3;
      line_len = 0;
    }
  }

  // shorten the string by assigning to size directly - safe since we
  // handled the empty case above. Not shrinking the allocation keeps
  // the buffer size stable across calls, which lets glibc serve
  // repeated large allocations from the heap instead of mmap'ing and
  // page-faulting a fresh region every call.
  output[j] = '\0';
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
