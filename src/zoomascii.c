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

// b2a_qp works through the input in 64-byte blocks, classifying each
// block into a bitmask of the bytes that can't pass through as-is.
// It then walks from one set bit to the next, so each plain run costs
// one fixed-size copy and each escape one table store, and runs never
// have to be rescanned.
#define BLOCK 64

// A plain run is always shorter than a line, which is up to 72 input
// bytes. A run that fills a line is copied in one 80-byte chunk; runs
// ending at an escape or CRLF are usually short, so they copy 32 bytes
// first and the remaining 48 only when needed.
#define COPY 80

// Reads reach back from the block start to the start of a plain run
// carried over from the previous block, which starts less than a line
// before the block. They reach forward to the end of a copy starting
// at offset 63.
#define HISTORY 80
#define BLOCK_READ (BLOCK + COPY)

// The most output a block can add: 64 escapes and the soft line breaks
// that go with them (3 bytes for every 72).
#define BLOCK_OUTPUT 200

// Output space needed on top of BLOCK_OUTPUT for each block that runs
// between two checks: a plain run carried over from before the first
// block (up to 72 bytes), up to 25 bytes past the last block that may
// all need escaping (an escape run that started inside it continues
// until its line fills), soft line breaks for both, 5 more for
// re-encoding a trailing space, and 80 bytes of copy overshoot.
#define OUTPUT_SLACK 512

// The end of the input is encoded from a copy, which takes up to three
// blocks. There are no checks on the output while that happens.
#define TAIL_ROOM (OUTPUT_SLACK + 2 * BLOCK_OUTPUT)

// The copy holds the last TAIL_DATA bytes of the input: the less than
// BLOCK_READ bytes that are left to encode and HISTORY before them. It
// is followed by TAIL_PAD bytes of padding, which is as far as the
// block the input ends in can read.
#define TAIL_DATA (HISTORY + BLOCK_READ)
#define TAIL_PAD BLOCK_READ

// Inputs up to SMALL_INPUT bytes are encoded into a buffer on the
// stack and the result is made from that, at its exact size. That
// costs a copy, but small allocations are much cheaper than large ones
// (with glibc, about 100 cycles less under 1KB) and a small result
// doesn't keep a 4KB buffer alive. The buffer has room for text of
// that size, if not for the worst case.
#define SMALL_INPUT 896
#define SMALL_OUTPUT 2048

#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__) && !defined(NO_ASM_CTZ)
// GCC sign-extends the result of __builtin_ctzll before using it as an
// index and, when tuning for generic x86-64, also clears the register
// first. Both are wasted work in the hot loop. This is the same
// instruction on its own: it runs as tzcnt where that exists and as
// bsf elsewhere, which agree on anything but zero.
static inline size_t ctz64(uint64_t x) {
  size_t n;
  __asm__("rep bsf {%1, %0|%0, %1}" : "=r"(n) : "rm"(x) : "cc");
  return n;
}
#elif defined(__GNUC__) || defined(__clang__)
#define ctz64(x) ((unsigned int)__builtin_ctzll(x))
#elif defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
static __inline int ctz64(uint64_t x) {
  unsigned long index;
  _BitScanForward64(&index, x);
  return (int)index;
}
#else
static int ctz64(uint64_t x) {
  int n = 0;
  while (!(x & 1)) {
    x >>= 1;
    n++;
  }
  return n;
}
#endif

#ifdef __SSE2__
// Bit n is set if p[n] needs to be encoded, for 16 bytes. Each _mm_*
// intrinsic compiles to a single instruction operating on all 16 bytes
// of a 128-bit register at once.
static inline uint64_t escape_mask16(const unsigned char *p) {
  __m128i v = _mm_loadu_si128((const __m128i*)p);
  // Wrapping addition maps bytes 32..126 to signed -128..-34, so one
  // signed comparison finds control bytes and bytes >= 127 ...
  __m128i esc = _mm_cmpgt_epi8(_mm_add_epi8(v, _mm_set1_epi8(96)),
                               _mm_set1_epi8(-34));
  // ... add '=' ...
  esc = _mm_or_si128(esc, _mm_cmpeq_epi8(v, _mm_set1_epi8(61)));
  // ... and take tab back out: andnot(a, b) = (NOT a) AND b
  esc = _mm_andnot_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8(9)), esc);
  // movemask packs the top bit of each byte lane into a 16-bit int
  return (unsigned int)_mm_movemask_epi8(esc);
}
#else
// The same for 8 bytes, using ordinary 64-bit arithmetic. Each test
// leaves its answer in the top bit of each byte. They work on the low 7
// bits, so no addition can carry into the next byte.
static inline uint64_t escape_mask8(const unsigned char *p) {
  const uint64_t ones = 0x0101010101010101ULL;
  const uint64_t high = 0x8080808080808080ULL;
  // byte n goes in bits 8n..8n+7 whatever the endianness; compilers
  // turn this into a single load
  uint64_t w = (uint64_t)p[0] | (uint64_t)p[1] << 8 |
    (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24 |
    (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
    (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
  uint64_t low = w & ~high;
  uint64_t from32 = low + 0x60 * ones;             // 32..127
  uint64_t is127 = low + ones;                     // 127
  uint64_t not61 = (low ^ 0x3D * ones) + ~high;    // anything but '='
  uint64_t not9 = (low ^ 0x09 * ones) + ~high;     // anything but tab
  // and bytes >= 128 are never plain
  uint64_t plain = ((from32 & ~is127 & not61) | ~not9) & ~w;
  // the top bits are at 8n + 7. The multiply moves bit 8n to 56 + n
  // without any of the partial products overlapping.
  return (((~plain & high) >> 7) * 0x0102040810204080ULL) >> 56;
}
#endif

// Bit n is set if p[n] needs to be encoded: everything outside 32..126
// except tab, plus '='. Spaces and tabs before a line break are handled
// separately since that needs lookahead.
static inline uint64_t escape_mask(const unsigned char *p) {
#ifdef __SSE2__
  return escape_mask16(p) | escape_mask16(p + 16) << 16 |
    escape_mask16(p + 32) << 32 | escape_mask16(p + 48) << 48;
#else
  uint64_t mask = 0;
  int k;

  for (k = 0; k < BLOCK; k += 8)
    mask |= escape_mask8(p + k) << k;
  return mask;
#endif
}

static PyObject*
b2a_qp(PyObject *self, PyObject *args, PyObject *kwargs) {
  Py_buffer input_buf;
  PyObject *ret;
  const unsigned char *input, *input_end, *p, *cur, *pe;
  char *output, *ocur;
  Py_ssize_t input_len, output_len, j, n;
  size_t e;
  // positions that can be past the end of the input are kept as plain
  // addresses
  uintptr_t line_end, p_limit;
  // Where the input ends, once the tail is in use. This is only read
  // when a lone CR turns up. It is volatile to keep it in memory: the
  // compiler would otherwise spend a register on it, or work it out
  // again for every block.
  volatile uintptr_t in_end;
  uint64_t mask;
  unsigned int dot, c;
  // the end of the input gets copied here, so the block code can always
  // read HISTORY bytes before a block and BLOCK_READ from its start.
  // The padding is CRs, which need encoding - that stops plain runs at
  // the end of the input - and which also stop runs of escapes.
  unsigned char tail[TAIL_DATA + TAIL_PAD];
  char small[SMALL_OUTPUT];

  static char *kwlist[] = {"string", "encode_leading_dot", NULL};
  int encode_leading_dot = 1;
  int have_buffer = 0;

  // Get the input string without copying it. The usual call passes a
  // bytes or str object, and maybe a bool, by position. Those are read
  // directly: the general argument parser takes as long as encoding a
  // few hundred bytes.
  input = NULL;
  if (kwargs == NULL && PyTuple_GET_SIZE(args) >= 1 &&
      (PyTuple_GET_SIZE(args) == 1 ||
       (PyTuple_GET_SIZE(args) == 2 &&
        PyBool_Check(PyTuple_GET_ITEM(args, 1))))) {
    PyObject *string = PyTuple_GET_ITEM(args, 0);

    if (PyBytes_CheckExact(string)) {
      input = (const unsigned char *)PyBytes_AS_STRING(string);
      input_len = PyBytes_GET_SIZE(string);
    }
#if PY_MAJOR_VERSION >= 3
    else if (PyUnicode_CheckExact(string)) {
      input = (const unsigned char *)PyUnicode_AsUTF8AndSize(string,
                                                             &input_len);
      if (!input)
        return NULL;
    }
#endif
    if (input && PyTuple_GET_SIZE(args) == 2)
      encode_leading_dot = PyTuple_GET_ITEM(args, 1) == Py_True;
  }
  if (!input) {
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s*|i", kwlist,
                                     &input_buf, &encode_leading_dot))
      return NULL;
    input = input_buf.buf;
    input_len = input_buf.len;
    have_buffer = 1;
  }

  assert(input_len >= 0);

  // Special case for empty input - return empty bytes directly
  if (input_len == 0) {
    if (have_buffer)
      PyBuffer_Release(&input_buf);
    return PyBytes_FromStringAndSize("", 0);
  }

  if (input_len > PY_SSIZE_T_MAX / 2) {
    if (have_buffer)
      PyBuffer_Release(&input_buf);
    return PyErr_NoMemory();
  }

  input_end = input + input_len;

  // Copy the end of the input, followed by the padding. This is done
  // long before it is needed because reading something back right
  // after writing it in pieces of a different size has to wait for the
  // writes to reach the cache. Both are fixed-size copies the compiler
  // can inline when the input is long enough.
  if (input_len >= TAIL_DATA)
    memcpy(tail, input_end - TAIL_DATA, TAIL_DATA);
  else
    memcpy(tail + TAIL_DATA - input_len, input, input_len);
  memset(tail + TAIL_DATA, CR, TAIL_PAD);

  if (input_len <= SMALL_INPUT) {
    ret = NULL;
    output = small;
    output_len = SMALL_OUTPUT;
  } else {
    // get a string to work on, in a format we can return directly
    // without more copying - start with a string twice as large, could
    // be more careful here and use less memory
    output_len = roundUp4k(input_len * 2);
    ret = PyBytes_FromStringAndSize(NULL, output_len);
    if (!ret) {
      if (have_buffer)
        PyBuffer_Release(&input_buf);
      return NULL;
    }
    output = PyBytes_AS_STRING(ret);
  }

  // a leading '.' is compared against this, which never matches a byte
  // when leading dots are left alone
  dot = encode_leading_dot ? '.' : 256;

#define ADDR(q) ((uintptr_t)(q))

// not actually part of QP encoding but SMTP needs this - encode a
// leading . on a line. This is used right after a line starts. The dot
// isn't in the mask, so skipping over it here is all it takes.
#define LEADING_DOT() do {                              \
    if (unlikely(*cur == dot)) {                        \
      memcpy(ocur, "=2E", 4);                           \
      ocur += 3;                                        \
      line_end -= 2;                                    \
      cur++;                                            \
    }                                                   \
  } while (0)

// soft line break before the input byte at cur
#define SOFT_BREAK() do {                               \
    memcpy(ocur, "=\r\n", 4);                           \
    ocur += 3;                                          \
    line_end = ADDR(cur) + MAX_LINE_LENGTH;             \
  } while (0)

// the line fills up inside the plain run that starts at cur
#define FILL_LINE() do {                                \
    memcpy(ocur, cur, COPY);                            \
    ocur += line_end - ADDR(cur);                       \
    cur = (const unsigned char *)line_end;              \
    SOFT_BREAK();                                       \
    LEADING_DOT();                                      \
  } while (0)

  // p is the current block and cur the next input byte to encode,
  // which goes to ocur in the output. cur can be before the block when
  // a plain run carries over from the previous block, and it moves
  // past the block's end when something reads into the next block.
  // line_end is the input address where the current line fills up if
  // everything up to there passes through, so each escape pulls it in
  // by 2.
  cur = input;
  ocur = output;
  if (unlikely(input_len >= 2 && input[0] == CR && input[1] == LF)) {
    ocur[0] = CR;
    ocur[1] = LF;
    ocur += 2;
    cur += 2;
  }
  line_end = ADDR(cur) + MAX_LINE_LENGTH;
  if (cur != input_end)
    LEADING_DOT();
  in_end = UINTPTR_MAX;
  p = input;
  p_limit = ADDR(p);
  goto first_block;

  for (;;) {
    if (mask == 0) {
      // nothing left to encode in this block
      if (line_end <= ADDR(p) + BLOCK)
        FILL_LINE();
    next_block:
      // Move on to the next block. Any plain run so far just carries
      // over.
      p += BLOCK;
    first_block:
      if (unlikely(ADDR(p) >= p_limit)) {
        // This is as far as the input and the output were known to be
        // good for. See how much further they go.
        Py_ssize_t left = input_end - p;
        Py_ssize_t room, blocks;

        j = ocur - output;
        room = left < BLOCK_READ ? TAIL_ROOM : OUTPUT_SLACK + BLOCK_OUTPUT;
        if (output_len - j < room) {
          // Twice the input wasn't enough. Whatever needed that much
          // encoding probably carries on, so get enough for the rest
          // of the input in the worst case.
          if (left > (PY_SSIZE_T_MAX - j) / 4 - TAIL_ROOM - 4096) {
            Py_XDECREF(ret);
            if (have_buffer)
              PyBuffer_Release(&input_buf);
            return PyErr_NoMemory();
          }
          output_len = roundUp4k(j + left * 3 + left / 8 + TAIL_ROOM);
          if (!ret) {
            // the output has outgrown the buffer on the stack
            ret = PyBytes_FromStringAndSize(NULL, output_len);
            if (ret)
              memcpy(PyBytes_AS_STRING(ret), small, j);
          } else {
            // this frees the string and clears ret if it fails
            _PyBytes_Resize(&ret, output_len);
          }
          if (!ret) {
            if (have_buffer)
              PyBuffer_Release(&input_buf);
            return NULL;
          }
          output = PyBytes_AS_STRING(ret);
          ocur = output + j;
        }
        if (left < BLOCK_READ) {
          // switch over to the copy of the end of the input
          uintptr_t shift = ADDR(tail + TAIL_DATA) - ADDR(input_end);

          p = (const unsigned char *)(ADDR(p) + shift);
          cur = (const unsigned char *)(ADDR(cur) + shift);
          line_end += shift;
          in_end = ADDR(tail + TAIL_DATA);
          p_limit = UINTPTR_MAX;
        } else {
          // blocks the output has room for, and blocks that can be
          // read straight from the input
          room = (output_len - j - OUTPUT_SLACK) / BLOCK_OUTPUT;
          blocks = (left - BLOCK_READ) / BLOCK + 1;
          p_limit = ADDR(p) + (room < blocks ? room : blocks) * BLOCK;
        }
      }
      mask = escape_mask(p);
      // bits below cur were handled by the previous block
      if (cur > p)
        mask &= ~(uint64_t)0 << (cur - p);
      continue;
    }

    pe = p + ctz64(mask);
  next_escape:
    if (unlikely(ADDR(pe) >= line_end))
      FILL_LINE();

    // copy the plain run up to the next byte that needs encoding.
    // Anything past the run gets overwritten by whatever comes next.
    n = pe - cur;
    memcpy(ocur, cur, 32);
    if (unlikely(n > 32))
      memcpy(ocur + 32, cur + 32, 48);
    ocur += n;

  adjacent:
    c = *pe;
    if (unlikely(c == CR)) {
      if (likely(pe[1] == LF)) {
        // CRLF can go as-is and resets the line. A space or tab right
        // before it has to be encoded, but it has already been copied
        // through as part of a plain run. Replace it. If it filled its
        // line, the soft line break after it is still needed after the
        // longer encoded form. pe[-1] is always readable: a CRLF at
        // input byte 0 is consumed before the loop, so pe is never the
        // first byte of the input here.
        if (unlikely(pe[-1] == ' ' || pe[-1] == '\t')) {
          c = pe[-1];
          if (line_end - ADDR(pe) == MAX_LINE_LENGTH) {
            memcpy(ocur - 4, qp_table[c], 4);
            memcpy(ocur - 1, "=\r\n", 4);
            ocur += 2;
          } else {
            memcpy(ocur - 1, qp_table[c], 4);
            ocur += 2;
            if (ADDR(pe) >= line_end - 2) {
              memcpy(ocur, "=\r\n", 4);
              ocur += 3;
            }
          }
        }
        // one 4-byte store; the next output overwrites the zeros
        memcpy(ocur, "\r\n\0", 4);
        ocur += 2;
        mask &= mask - 1;
        mask &= mask - 1;
        cur = pe + 2;
        line_end = ADDR(cur) + MAX_LINE_LENGTH;
        LEADING_DOT();
        continue;
      }
      // the tail padding is CRs, so this is where the input ends
      if (ADDR(pe) >= in_end)
        break;
    }

    // encode all other chars as =XX. The table entries are 4 bytes so
    // this is a single 32-bit store; the fourth byte is overwritten by
    // the next output.
    memcpy(ocur, qp_table[c], 4);
    ocur += 3;
    line_end -= 2;
    cur = pe + 1;
    mask &= mask - 1;
    if (mask) {
      e = ctz64(mask);
      pe = p + e;
      if (likely(ADDR(cur) < line_end)) {
        // the common case: a single escape in the middle of a line
        if (likely(pe != cur))
          goto next_escape;
        // The next byte needs encoding as well. Unless at least three
        // in a row do, take them one at a time: that is the rest of a
        // two or three byte UTF-8 character, which is quicker than
        // getting into and out of the loop below, and the next escape
        // after it doesn't have to wait to find out where this one
        // ended.
        if ((mask >> e & 7) != 7)
          goto adjacent;
      }
    } else if (likely(ADDR(cur) < line_end)) {
      continue;
    }

    // Keep going while the following bytes also need encoding
    // (multi-byte UTF-8 text) rather than going back through the mask
    // for each one. This continues across soft line breaks, so it can
    // run past the end of the block, but only to the end of the line
    // the block ends in: after each soft break, the run leaves for the
    // next block once cur is past this one.
    for (;;) {
      // where the line fills up in the output
      char *oline_end = ocur + (Py_ssize_t)(line_end - ADDR(cur));
      int32_t esc, esc2, stop;
      // The stop flag is the high bit of a table entry's fourth byte.
      // Building the test mask with the same memory layout works on
      // any endianness and folds to a sign test on little-endian.
      memcpy(&stop, "\0\0\0\x80", 4);
      // two at a time while the line has room for both
      while (ocur + 3 < oline_end) {
        memcpy(&esc, qp_table[cur[0]], 4);
        memcpy(&esc2, qp_table[cur[1]], 4);
        if ((esc | esc2) & stop) {
          if (!(esc & stop)) {
            memcpy(ocur, &esc, 4);
            ocur += 3;
            cur++;
          }
          line_end = ADDR(cur) + (oline_end - ocur);
          goto escapes_done;
        }
        memcpy(ocur, &esc, 4);
        memcpy(ocur + 3, &esc2, 4);
        ocur += 6;
        cur += 2;
      }
      // the line has room for one more at most
      if (ocur < oline_end) {
        memcpy(&esc, qp_table[*cur], 4);
        if (esc & stop) {
          line_end = ADDR(cur) + (oline_end - ocur);
          goto escapes_done;
        }
        memcpy(ocur, &esc, 4);
        ocur += 3;
        cur++;
      }
      SOFT_BREAK();
      LEADING_DOT();
      n = cur - p;
      if (n >= BLOCK)
        goto next_block;
    }
  escapes_done:
    n = cur - p;
    if (n >= BLOCK)
      goto next_block;
    // the bits before cur are done
    mask = mask >> n << n;
  }
#undef LEADING_DOT
#undef SOFT_BREAK
#undef FILL_LINE

  // a space or tab at the very end also has to be encoded, as at a CRLF
  c = pe[-1];
  if (c == ' ' || c == '\t') {
    if (line_end - ADDR(pe) == MAX_LINE_LENGTH) {
      memcpy(ocur - 4, qp_table[c], 4);
      memcpy(ocur - 1, "=\r\n", 4);
      ocur += 2;
    } else {
      memcpy(ocur - 1, qp_table[c], 4);
      ocur += 2;
      if (line_end - ADDR(pe) <= 2) {
        memcpy(ocur, "=\r\n", 4);
        ocur += 3;
      }
    }
  }
  j = ocur - output;
#undef ADDR

  if (ret) {
    // shorten the string by assigning to size directly - safe since we
    // handled the empty case above. Not shrinking the allocation keeps
    // the buffer size stable across calls, which lets glibc serve
    // repeated large allocations from the heap instead of mmap'ing and
    // page-faulting a fresh region every call.
    output[j] = '\0';
    Py_SET_SIZE(ret, j);
  } else {
    ret = PyBytes_FromStringAndSize(small, j);
  }

  if (have_buffer)
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
