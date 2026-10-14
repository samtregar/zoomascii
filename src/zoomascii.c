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

// Reads relative to the block start reach back to the start of a plain
// run carried over from the previous block, which starts less than a
// line before the block. They reach forward to the end of a copy
// starting at offset 63.
#define HISTORY 80
#define BLOCK_READ (BLOCK + COPY)

// Output space needed between checks, one per block: a plain run
// carried over from the previous block (up to 72 bytes), then up to
// 88 bytes that may all need escaping (an escape run started at offset
// 63 can continue up to 24 escapes into the next block before the line
// fills), soft line breaks for all that, 5 more for re-encoding a
// trailing space, and 80 bytes of copy overshoot.
#define OUTPUT_SLACK 512

#if defined(__GNUC__) || defined(__clang__)
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
  const unsigned char *input, *base;
  char *output, *output_limit, *obase;
  Py_ssize_t input_len, output_len, j, b, b_limit, cur, line_end, e;
  uint64_t mask;
  unsigned int dot, c;
  // the end of the input gets copied here, so the block code can always
  // read HISTORY bytes before a block and BLOCK_READ from its start.
  // The padding is CRs, which need encoding - that stops plain runs at
  // the end of the input - and which also stop runs of escapes. The
  // tail can take up to 3 blocks: it starts less than BLOCK_READ before
  // the end.
  unsigned char tail[HISTORY + 2 * BLOCK + BLOCK_READ];

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
  output_limit = output + output_len - OUTPUT_SLACK;

  // a leading '.' is compared against this, which never matches a byte
  // when leading dots are left alone
  dot = encode_leading_dot ? '.' : 256;

// not actually part of QP encoding but SMTP needs this - encode a
// leading . on a line. This is used right after a line starts. The dot
// isn't in the mask, so skipping over it here is all it takes.
#define LEADING_DOT() do {                              \
    if (base[cur] == dot) {                             \
      memcpy(obase + cur, "=2E", 4);                    \
      obase += 2;                                       \
      line_end -= 2;                                    \
      cur++;                                            \
    }                                                   \
  } while (0)

// soft line break before input position pos
#define SOFT_BREAK(pos) do {                            \
    memcpy(obase + (pos), "=\r\n", 4);                  \
    obase += 3;                                         \
    line_end = (pos) + MAX_LINE_LENGTH;                 \
  } while (0)

// switch base over to a copy of the rest of the input in tail. Filling
// all of it with padding first is a fixed-size memset the compiler can
// inline, which is quicker than working out how much padding is needed.
#define USE_TAIL() do {                                         \
    Py_ssize_t pos = input_len + b;                             \
    Py_ssize_t history = pos < HISTORY ? pos : HISTORY;         \
    tail[HISTORY - 1] = 0;                                      \
    memset(tail + HISTORY, CR, sizeof(tail) - HISTORY);         \
    memcpy(tail + HISTORY - history, base - history,            \
           history - b);                                        \
    base = tail + HISTORY;                                      \
    b_limit = PY_SSIZE_T_MAX;                                   \
  } while (0)

  // Positions are relative to the current block, which starts at
  // base; b counts up from -input_len so b + e == 0 at the end of the
  // input. cur is the next input byte to encode. It can be negative
  // when a plain run carries over from the previous block, and it moves
  // past BLOCK when something reads into the next block. obase + k is
  // where input byte k goes in the output if it passes through, so a
  // plain run needs no output bookkeeping; each escape moves it on by 2
  // and each soft line break by 3. line_end is the input position where
  // the current line fills up if everything up to there passes through,
  // so each escape pulls it in by 2.
  b = -input_len;
  b_limit = -BLOCK_READ;
  base = input;
  if (b > b_limit)
    USE_TAIL();
  obase = output;
  cur = 0;
  line_end = MAX_LINE_LENGTH;
  if (unlikely(base[0] == CR && base[1] == LF)) {
    obase[0] = CR;
    obase[1] = LF;
    cur = 2;
    line_end = 2 + MAX_LINE_LENGTH;
  }
  LEADING_DOT();
  mask = escape_mask(base) & (~(uint64_t)0 << cur);

  for (;;) {
    if (mask == 0) {
      if (line_end > BLOCK) {
      next_block:
        // nothing left to encode in this block - move on to the next.
        // Any plain run so far just carries over.
        b += BLOCK;
        base += BLOCK;
        obase += BLOCK;
        cur -= BLOCK;
        line_end -= BLOCK;
        if (unlikely(b > b_limit))
          USE_TAIL();
        // make sure this block can't run off the end of the output
        if (unlikely(obase + cur > output_limit)) {
          // get another 4k and realloc the string
          j = obase + cur - output;
          output_len = roundUp4k(j + OUTPUT_SLACK);
          if (_PyBytes_Resize(&ret, output_len) == -1) {
            PyBuffer_Release(&input_buf);
            return NULL;
          }
          output = PyBytes_AS_STRING(ret);
          output_limit = output + output_len - OUTPUT_SLACK;
          obase = output + j - cur;
        }
        mask = escape_mask(base);
        // bits below cur were handled by the previous block
        if (cur > 0)
          mask &= ~(uint64_t)0 << cur;
        continue;
      }
      e = BLOCK;
    } else {
    next_escape:
      e = ctz64(mask);
    }

    if (unlikely(e >= line_end)) {
      // the line fills up inside the run
      memcpy(obase + cur, base + cur, COPY);
      cur = line_end;
      SOFT_BREAK(cur);
      LEADING_DOT();
      if (mask == 0)
        goto next_block;
    }

    // copy the plain run up to the next byte that needs encoding.
    // Anything past the run gets overwritten by whatever comes next.
    memcpy(obase + cur, base + cur, 32);
    if (unlikely(e - cur > 32))
      memcpy(obase + cur + 32, base + cur + 32, 48);
    cur = e;

    c = base[e];
    if (c == CR) {
      if (base[e+1] == LF) {
        // CRLF can go as-is and resets the line. A space or tab right
        // before it has to be encoded, but it has already been copied
        // through as part of a plain run. Replace it. If it filled its
        // line, the soft line break after it is still needed after the
        // longer encoded form. base[e-1] is always readable: in later
        // blocks it is in HISTORY, and a CRLF at input byte 0 is
        // consumed before the loop, so e == 0 never reaches this in
        // the first block. Without that special case, base[-1] would
        // read before the input on inputs of BLOCK_READ bytes or more.
        if (unlikely(base[e-1] == ' ' || base[e-1] == '\t')) {
          c = base[e-1];
          if (line_end - e == MAX_LINE_LENGTH) {
            memcpy(obase + e - 4, qp_table[c], 4);
            memcpy(obase + e - 1, "=\r\n", 4);
            obase += 2;
          } else {
            memcpy(obase + e - 1, qp_table[c], 4);
            obase += 2;
            line_end -= 2;
            if (e >= line_end)
              SOFT_BREAK(e);
          }
        }
        obase[e] = CR;
        obase[e+1] = LF;
        mask &= mask - 1;
        mask &= mask - 1;
        cur = e + 2;
        line_end = cur + MAX_LINE_LENGTH;
        LEADING_DOT();
        if (mask)
          goto next_escape;
        goto next_block;
      }
      // the tail padding is CRs, so this is where the input ends
      if (b + e >= 0)
        break;
    }

    // encode all other chars as =XX. The table entries are 4 bytes so
    // this is a single 32-bit store; the fourth byte is overwritten by
    // the next output.
    memcpy(obase + e, qp_table[c], 4);
    obase += 2;
    line_end -= 2;
    cur = e + 1;
    if (likely(!(mask >> (cur & 63) & 1)) && likely(cur < line_end)) {
      // the common case: a single escape in the middle of a line
      mask &= mask - 1;
      if (mask)
        goto next_escape;
      continue;
    }

    // Keep going while the following bytes also need encoding
    // (multi-byte UTF-8 text) rather than going back through the mask
    // for each one. This continues across soft line breaks, so it can
    // run past the end of the block, but only to the end of the line
    // the block ends in: after each soft break, the run leaves for the
    // next block once cur >= BLOCK.
    for (;;) {
      char *out = obase + cur;
      char *line_out_end = obase + line_end;
      while (out < line_out_end) {
        int32_t esc, stop;
        memcpy(&esc, qp_table[base[cur]], 4);
        // the stop flag is the high bit of the entry's fourth byte.
        // Building the test mask with the same memory layout works on
        // any endianness and folds to a sign test on little-endian.
        memcpy(&stop, "\0\0\0\x80", 4);
        if (esc & stop) {
          obase = out - cur;
          line_end = line_out_end - obase;
          goto escapes_done;
        }
        memcpy(out, &esc, 4);
        out += 3;
        cur++;
      }
      obase = out - cur;
      SOFT_BREAK(cur);
      LEADING_DOT();
      if (cur >= BLOCK)
        goto next_block;
    }
  escapes_done:
    if (cur >= BLOCK)
      goto next_block;
    mask &= ~(uint64_t)0 << cur;
  }
#undef LEADING_DOT
#undef SOFT_BREAK
#undef USE_TAIL

  // a space or tab at the very end also has to be encoded, as at a CRLF
  c = base[e-1];
  if (c == ' ' || c == '\t') {
    if (line_end - cur == MAX_LINE_LENGTH) {
      memcpy(obase + cur - 4, qp_table[c], 4);
      memcpy(obase + cur - 1, "=\r\n", 4);
      obase += 2;
    } else {
      memcpy(obase + cur - 1, qp_table[c], 4);
      obase += 2;
      if (line_end - cur <= 2) {
        memcpy(obase + cur, "=\r\n", 4);
        obase += 3;
      }
    }
  }
  j = obase + cur - output;

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
