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
- ~3x faster than `binascii.b2a_qp`
- Optional `encode_leading_dot` parameter (default: True) for SMTP compatibility
  - When True: Encodes leading periods on lines (required for SMTP)
  - When False: Passes through leading periods unchanged
- Uses precompiled lookup tables for performance (initialized at compile time)
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

**bin/fuzz_test.py** - Stress testing utility
- Generates random data using the faker library
- Tests for segfaults, memory leaks, and correctness
- Validates output against `binascii.a2b_qp` for correctness
- Runs 100,000 test iterations with various input sizes

### Performance Notes
- Module trades memory for speed through precomputed lookup tables
- Recent optimizations replaced runtime initialization with compile-time lookup tables
- Benchmarks show significant performance improvements over standard library equivalents:
  - b2a_qp: ~3x faster than binascii.b2a_qp
  - swapcase: ~10x faster than Python's builtin
- Benchmark results are measured in operations per second across a 472KB corpus
- Run benchmarks with: `PYTHONPATH=. python3 bin/bench.py`