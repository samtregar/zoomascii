# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Python Version Compatibility

This module supports both Python 2.7 and Python 3.5+, with compatibility handled through preprocessor macros in the C extension code.

## Development Commands

### Building and Installing
- `python3 setup.py build` - Build the C extension module
- `python3 setup.py install` - Install the package locally
- `python3 setup.py develop` - Install in development mode (editable install)

### Testing
- `python3 setup.py test` - Run all tests (builds extension and runs both basic and benchmark tests)
- `python3 -m unittest tests.basic` - Run only basic functionality tests
- `python3 -m unittest tests.bench` - Run only benchmark tests
- `python3 bin/fuzz_test.py` - Run fuzz/stress testing (requires faker library) to find segfaults and memory leaks

### Distribution
- `python3 setup.py sdist` - Create source distribution
- `python3 setup.py bdist_wheel` - Create wheel distribution
- `python3 setup.py clean` - Clean build artifacts

## Architecture Overview

This is a Python C extension module that provides faster implementations of ASCII string processing functions, optimized for speed over memory usage.

### Core Components

**C Extension (`src/zoomascii.c`, `src/zoomascii.h`)**
- Main implementation in C for performance-critical functions
- Uses precomputed lookup tables and memory optimization techniques
- Supports both Python 2.7 and 3.x through compatibility macros
- Currently implements `b2a_qp` (Quoted-Printable encoding) and `swapcase`

**Python Package Structure**
- `setup.py` - Standard setuptools configuration with C extension definition
- `tests/` - Unit tests and benchmarks
- `data/` - Test corpus files for benchmarking

### Key Functions

**b2a_qp (Quoted-Printable Encoding)**
- RFC 2045 compliant implementation
- ~15x faster than `binascii.b2a_qp`
- Optional `encode_leading_dot` parameter (default: True) for SMTP compatibility
  - When True: Encodes leading periods on lines (required for SMTP)
  - When False: Passes through leading periods unchanged
- Uses precompiled lookup tables for performance (initialized at compile time)
- Scans runs of pass-through characters 16 bytes at a time with SSE2 intrinsics (`#ifdef __SSE2__`, present on all x86-64; scalar lookup-table fallback for other architectures), storing each 16-byte block to the output as it is classified rather than doing a separate memcpy
- Spaces and tabs are ordinary run characters; only a trailing one before CRLF or end of input is backed off and encoded
- The output buffer starts at 2x the input and is grown on demand; the hot loop checks for room once per iteration with a 128-byte margin (`OUTPUT_SLACK`) that covers a full 72-char run, a 16-byte vector store overshoot and a soft line break
- The result is truncated with `Py_SET_SIZE` rather than shrunk with realloc on purpose: freeing a block the same size as was allocated lets glibc raise its mmap threshold, so repeated large calls are served from the heap. Shrinking first made every call mmap/mremap/munmap and page-fault, which cost more than half the runtime
- Properly handles CRLF line endings (designed for text mode operation)

**swapcase**
- ASCII-only swapcase implementation
- ~10x faster than Python's builtin `swapcase()`
- Uses lookup table for character case conversion
- **Note**: This is a proof-of-concept function and not important for optimization - no need to make it faster

### Test Data
The `data/` directory contains various text files used for testing and benchmarking:
- HTML files (UTF-8 demo, example, nytimes.html) with both LF and CRLF line endings
- Lorem ipsum text files with different line endings
- Total corpus size: ~472KB used for benchmark measurements

### Testing Tools
**bin/bench.py** - Standalone benchmark script
- Run with: `PYTHONPATH=. python3 bin/bench.py`
- Measures operations per second across the full test corpus
- Use this to validate performance optimizations

**bin/bench_zoom.py** - Tight benchmark of `b2a_qp` only
- Run with: `PYTHONPATH=. python3 bin/bench_zoom.py`
- Reports best-of-10 ops/sec - prefer this over single bench.py runs when comparing optimizations, since single runs can vary by ±20% with machine load
- `--baseline DIRECTORY` loads a saved extension build in the same interpreter, verifies identical corpus output for both leading-dot settings, and alternates timing samples; `--per-file` adds individual input results. Save the baseline before editing with `python3 setup.py build_ext --force --build-lib DIRECTORY`, then rebuild the current extension in place.

**bin/bench_chart.py** - Regenerates the README benchmark chart
- Run with: `python3 bin/bench_chart.py ZOOM BINASCII QUOPRI` using the three ops/sec numbers from `bin/bench.py` (best of a few pinned runs)
- Writes `docs/benchmarks/b2a_qp.svg` and `b2a_qp-dark.svg`; README.md selects between them with a `<picture>` element for the viewer's theme
- Rerun it whenever the benchmark numbers in the README change

**bin/verify_correct.py** - Correctness harness for `b2a_qp`
- Run with: `PYTHONPATH=. python3 bin/verify_correct.py`
- Round-trips the corpus, ~35 edge cases, and 400 random inputs through `binascii.a2b_qp`
- Checks RFC 2045 line-length and trailing-whitespace rules plus SMTP leading-dot encoding
- Run this after any change to the C encoding logic

**bin/fuzz_test.py** - Stress testing utility
- Generates random data using the faker library
- Tests for segfaults, memory leaks, and correctness
- Validates output against `binascii.a2b_qp` for correctness
- Runs 100,000 test iterations with various input sizes

### Performance Notes
- Module trades memory for speed through precomputed lookup tables
- Benchmarks show significant performance improvements over standard library equivalents:
  - b2a_qp: ~20x faster than binascii.b2a_qp (9,618.74 vs 470.87 ops/sec; pure-Python quopri 9.81 ops/sec; best of three bin/bench.py runs pinned to CPU 2, Ryzen 7 PRO 7840U, Python 3.11.6, September 5, 2026)
  - swapcase: ~10x faster than Python's builtin
- Benchmark results are measured in operations per second across a 472KB corpus
- Run benchmarks with: `PYTHONPATH=. python3 bin/bench.py` (or `bin/bench_zoom.py` for low-noise comparisons)

### Optimization History

**September 2026** - SIMD bounds and escape handling, ~1.33x on the raw corpus versus b59e211 (6,067 to 8,072 ops/sec, median of 11 alternating 2,000-run samples pinned to CPU 2, Ryzen 7 PRO 7840U, Python 3.11.6, GCC 13.2):
- Compute the final legal 16-byte load position once per plain run, combining input and line limits into one inner-loop bounds check
- Check the printable ASCII range with wrapping addition and one signed SIMD comparison, still excluding `=` and including tab
- Handle an escape or CRLF immediately after copying the preceding plain run, avoiding another outer-loop iteration
- Preserve exact output, the SSE2 requirement, the portable scalar fallback, and allocation behavior
- Verified against the original and scalar builds over 20,000 randomized comparisons, plus all byte values in each SIMD lane, line boundaries, and buffer growth; Valgrind reports no memory errors. The unchanged repository fuzzer also passed all 100,000 cases, including binary inputs up to 1 MB.
- The README chart uses fresh best-of-three runs of the separate bin/bench.py benchmark, which normalizes line endings; these before/after measurements preserve the raw input

**September 2026** - loop restructure, ~2.4x on the corpus (~1,900 to ~4,500 ops/sec pinned to one core; HTML files 2.5-3.2x, lorem files unchanged since they were already run-bound):
- Let spaces and tabs *start* a run, not just continue one. Before, every leading indentation space in the HTML corpus took its own outer-loop iteration through the space/tab branch. The back-off now handles a run that is a single space by dropping to the encode path
- Store the 16 input bytes to the output inside the SSE2 scan loop instead of a separate variable-length memcpy after the scan (the stores past the run end are harmless with the output slack)
- Replace the per-byte `j+3 > output_len` check and the three `max_x` clamps with one `j + OUTPUT_SLACK > output_len` check per iteration
- Encode consecutive non-plain bytes (multi-byte UTF-8) in a tight inner loop instead of one outer iteration each
- `qp_table` entries are 4 bytes so an escape is a single 32-bit store
- Output is byte-for-byte identical to the previous version (checked over the corpus plus 3,200 random/edge inputs)
- Also fixed: a failed `_PyBytes_Resize` was followed by `Py_DECREF(ret)` on a pointer the resize had already nulled, and the result was not NUL-terminated

**June 2026** - what got b2a_qp from ~3x to ~5x faster than binascii (~1,630 to ~2,450 ops/sec):
- Replaced the `NEEDS_ENCODE` comparison chain and per-character space special-case in the run scan with a 256-byte `qp_plain` lookup table; spaces/tabs join runs with a one-char back-off before CR or end of input (+16%)
- SSE2 vectorized run scanning, 16 bytes per compare via movemask/ctz (+30% more)

Tried and rejected - don't re-attempt these without new evidence:
- **Allocating the worst-case output (3.1x) up front and shrinking with `_PyBytes_Resize` at the end**: removes the per-iteration room check but the shrink-then-free pattern keeps glibc's mmap threshold low, so every call on a >128KB input pays mmap + mremap + munmap + page faults: 56% of runtime in the kernel, ~40% slower than growing a 2x buffer and truncating with `Py_SET_SIZE`.
- **Compiler flags** (`-O3`, `-march=native`): ~1% change, within benchmark noise. The hot loop is branch-bound, not helped by auto-vectorization.
- **AVX2 32-bytes-per-iteration scan**: ~12% *slower* than SSE2 (~2,150 vs ~2,450 ops/sec). Runs of plain characters are too short to amortize the wider vectors - lines are capped at 72 chars and the HTML corpus is full of `=` characters (attribute syntax) that terminate runs early.
- **Compiling the SSE2 code with `-mavx2`**: +5% from VEX encoding, but not portable in a distributed package without runtime CPU dispatch; not worth the packaging complexity.
