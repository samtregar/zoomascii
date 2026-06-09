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
- ~5x faster than `binascii.b2a_qp`
- Optional `encode_leading_dot` parameter (default: True) for SMTP compatibility
  - When True: Encodes leading periods on lines (required for SMTP)
  - When False: Passes through leading periods unchanged
- Uses precompiled lookup tables for performance (initialized at compile time)
- Scans runs of pass-through characters 16 bytes at a time with SSE2 intrinsics (`#ifdef __SSE2__`, present on all x86-64; scalar lookup-table fallback for other architectures)
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
  - b2a_qp: ~5x faster than binascii.b2a_qp (~2,450 vs ~460 ops/sec on the corpus, June 2026)
  - swapcase: ~10x faster than Python's builtin
- Benchmark results are measured in operations per second across a 472KB corpus
- Run benchmarks with: `PYTHONPATH=. python3 bin/bench.py` (or `bin/bench_zoom.py` for low-noise comparisons)

### Optimization History (June 2026)
What got b2a_qp from ~3x to ~5x faster than binascii (~1,630 to ~2,450 ops/sec):
- Replaced the `NEEDS_ENCODE` comparison chain and per-character space special-case in the run scan with a 256-byte `qp_plain` lookup table; spaces/tabs join runs with a one-char back-off before CR or end of input (+16%)
- SSE2 vectorized run scanning, 16 bytes per compare via movemask/ctz (+30% more)

Tried and rejected - don't re-attempt these without new evidence:
- **Compiler flags** (`-O3`, `-march=native`): ~1% change, within benchmark noise. The hot loop is branch-bound, not helped by auto-vectorization.
- **AVX2 32-bytes-per-iteration scan**: ~12% *slower* than SSE2 (~2,150 vs ~2,450 ops/sec). Runs of plain characters are too short to amortize the wider vectors - lines are capped at 72 chars and the HTML corpus is full of `=` characters (attribute syntax) that terminate runs early.
- **Compiling the SSE2 code with `-mavx2`**: +5% from VEX encoding, but not portable in a distributed package without runtime CPU dispatch; not worth the packaging complexity.