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

### Continuous Integration
`.github/workflows/wheels.yml` runs on pushes to master, `v*` tags, pull requests, and manual dispatch:
- **wheels**: cibuildwheel (pinned) builds and tests wheels on native GitHub runners for Linux x86-64/i686 and ARM64, Windows x64/x86/ARM64, and macOS ARM64/Intel, plus Linux s390x (big-endian) under QEMU for one Python version, since emulation is slow. Each wheel is tested with `tests/basic.py` and `bin/verify_correct.py` from a temp directory. Free-threaded builds (`cp3??t`) are skipped because the module doesn't declare GIL-free support
- The SSE2 path is compiled on Linux x86-64 and macOS Intel; ARM, s390x, and Windows (MSVC never defines `__SSE2__`) use the portable SWAR path
- **portable**: builds with `CFLAGS=-U__SSE2__` on Linux, fails if any `pmovmskb` instruction is present, and runs the tests
- **sdist**: builds the source distribution, then installs and tests it
- Wheels and sdist are uploaded as workflow artifacts; nothing publishes to PyPI yet

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
- ~48x faster than `binascii.b2a_qp`
- Optional `encode_leading_dot` parameter (default: True) for SMTP compatibility
  - When True: Encodes leading periods on lines (required for SMTP)
  - When False: Passes through leading periods unchanged
- Uses precompiled lookup tables for performance (initialized at compile time)
- Classifies the input in 64-byte blocks into a bitmask of bytes needing encoding (`escape_mask`: four SSE2 compares + movemask under `#ifdef __SSE2__`, present on all x86-64; other architectures get `escape_mask8`, the same test done 8 bytes at a time with 64-bit SWAR arithmetic), then walks from set bit to set bit with ctz. Plain runs ending at an escape or CRLF copy 32 bytes first and the remaining 48 only when longer than 32 bytes; runs that fill a line copy 80 bytes; each escape is one 4-byte `qp_table` store (whose fourth byte also flags plain/CR bytes to stop escape runs); nothing is rescanned
- Positions are input addresses: `p` is the current block, `cur` the next input byte to encode and `ocur` where it goes in the output, so a plain run is copied straight from one to the other and moving to the next block only advances `p`. `line_end` is the input address where the line fills (escapes pull it in by 2). A plain run reaching the block end carries over with `cur` behind `p` rather than being copied twice, so the block reads `HISTORY` bytes back and `BLOCK_READ` forward
- Bytes needing encoding that follow each other are handled two ways. One or two more (the rest of a two or three byte UTF-8 character) re-enter the main path at `adjacent`, one mask bit at a time. Three or more go to a loop driven by the `qp_table` stop flag that encodes two per iteration and carries on across soft line breaks; when it ends, the mask is cut back to `cur`. Short runs stay out of that loop because the next escape would have to wait for it to find out where the run ended
- Input and output limits are checked together, for a stretch of blocks at a time: `p_limit` is the block where either the tail takes over or the output room worked out at the last check runs out (`BLOCK_OUTPUT` per block on top of `OUTPUT_SLACK`, derivation in the source). The hot loop compares `p` against it and nothing else
- The last <144 bytes are encoded from a stack copy (`tail`): the last 224 bytes of the input followed by 144 CRs. Padding stops plain runs and escape runs, and the lone-CR path is where the end of input is detected, so the hot loop has no end-of-input check. The copy is made at function entry, long before it is read: reading it back straight after writing it stalls on store forwarding (~50 cycles)
- Spaces and tabs are ordinary run characters; one that turns out to precede a CRLF or the end of input has already been copied, so it is rewritten in place as `=20`/`=09` (keeping a soft break that followed it). Leading dots are encoded inline wherever a line starts
- Inputs up to 896 bytes (`SMALL_INPUT`) are encoded into a 2KB buffer on the stack and returned as a bytes object of the exact size; if the output outgrows the buffer it moves to the heap. glibc takes ~100 cycles longer for allocations over ~1KB than under, and a short result no longer holds on to 4KB
- Larger inputs get an output buffer of 2x the input. If that runs out it grows once, to the worst case for the rest of the input
- That buffer is truncated with `Py_SET_SIZE` rather than shrunk with realloc on purpose: freeing a block the same size as was allocated lets glibc raise its mmap threshold, so repeated large calls are served from the heap. Shrinking first made every call mmap/mremap/munmap and page-fault, which cost more than half the runtime
- A bytes or str argument passed by position (with an optional bool) is read directly; anything else goes through `PyArg_ParseTupleAndKeywords`, which costs ~90 cycles
- Compiler steering, all measured: on GCC/x86-64 `ctz64` is a one-instruction inline asm (`rep bsf`), because GCC wraps `__builtin_ctzll` in a sign extension and a register clear (3-4% on HTML); `in_end` is `volatile` so it stays in memory instead of taking a register or being recomputed per block; rarely-needed values like `p + BLOCK` are computed where they are used, since GCC otherwise sets them up for every block
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
- `--sizes 100,1000,10000` adds the first that many bytes of the largest corpus file, with more encodes per sample. The corpus is 95% two large files, so this is the only view of the fixed cost of a call, which is most of the time for inputs under ~1KB

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
  - b2a_qp: ~48x faster than binascii.b2a_qp (22,889.56 vs 474.70 ops/sec; pure-Python quopri 10.05 ops/sec; best of three bin/bench.py runs pinned to CPU 2, Ryzen 7 PRO 7840U, Python 3.11.6, November 9, 2026)
  - swapcase: ~10x faster than Python's builtin
- Benchmark results are measured in operations per second across a 472KB corpus
- Run benchmarks with: `PYTHONPATH=. python3 bin/bench.py` (or `bin/bench_zoom.py` for low-noise comparisons)

### Optimization History

**November 9, 2026** - leaner loop state, cheaper calls and quicker escape runs, ~1.10x on the raw corpus versus bd3886c (19,049 to 20,915 ops/sec median, 19,627 to 21,343 best across 11 alternating 2,000-run bench_zoom samples pinned to CPU 2, Ryzen 7 PRO 7840U, Python 3.11.6, GCC 13.2; per file: lorem_crfl.txt 1.25x, lorem.txt 1.24x, example.html 1.19x, nytimes_crlf.html 1.10x, UTF-8-demo.html 1.09x, nytimes.html 1.08x). Short inputs gain more (`--sizes`, the first N bytes of nytimes_crlf.html, timings include the Python call): 1.50x at 50 bytes, 1.55x at 200, 1.34x at 500, 1.20x at 1,000, 1.10x at 3,000:
- Keep input addresses and an output pointer (`cur`, `ocur`, `line_end`) instead of block-relative indexes. Moving to the next block updates one pointer instead of five, and copies and escape stores need no address arithmetic
- Check the input and output limits once per stretch of blocks (`p_limit`) instead of both for every block
- `ctz64` as inline asm on GCC/x86-64, dropping two instructions GCC adds around `__builtin_ctzll` on every escape
- nytimes.html per encode (perf stat): 557.8k to 502.8k instructions, ~100k to ~90k cycles. IPC stays around 5.4
- Per call: read bytes/str arguments directly (~90 cycles); encode inputs up to 896 bytes on the stack and return an exact-size result (glibc spent ~100 cycles on the 4KB allocation; ~80 saved after the extra copy, and less memory held); build the tail with fixed-size inline copies at function entry (it was a `rep stos`, a libc call and a ~50-cycle store-forwarding stall; ~30 saved). Cycles per call on plain ASCII text, net of the calling loop (perf stat): 326 to 88 for 1 byte, 341 to 156 for 100, 436 to 249 for 500, 542 to 398 for 1,000, 930 to 734 for 3,000
- Escape runs: the loop for long runs encodes two bytes per iteration, and runs of two or three bytes go through the mask instead (the `adjacent` label). On generated samples (perf stat cycles): CJK text 1.17x, Russian 1.14x, French 1.10x. The French sample was 3% *slower* than before until short runs left the loop: it was waiting on a dependency chain (mask, ctz, `cur`, loop exit, mask again), not on instruction count
- When 2x the input isn't enough the output now grows once, to the worst case for what is left, instead of 4KB at a time
- Output is byte-identical to bd3886c: 186,000 inputs for the SSE2 build and 129,000 for the scalar build through two differential harnesses (every length to 1,300, escape runs of every length at every block and line offset, corpus slices, random mixes, inputs up to 1.5MB), plus the pure-Python reference encoder on the small ones. The unchanged repository fuzzer passed all 100,000 cases. AddressSanitizer is clean on both builds, and catches a deliberately wrong output-room calculation; valgrind reports nothing in the extension. New unit tests cover escape runs, the stack buffer and its overflow, output growth and argument forms; five deliberately broken builds fail them. Tests pass on Python 3.8, 3.9, 3.10, 3.11 and 3.14
- Measuring: single `perf stat` runs differed by 3-5% between invocations, so builds were compared in interleaved rounds taking the minimum. User-mode counts miss the mmap-threshold trap; kernel cycles and page faults per call were checked separately and are unchanged

**October 13, 2026** - short-run copy split, single-load multi-escape loop, and direct loop transitions, ~1.13x on the raw corpus versus 86dc2e3 (15,484 to 17,519 ops/sec median, 16,845 to 19,338 best across 11 alternating 2,000-run bench_zoom samples pinned to CPU 2, Ryzen 7 PRO 7840U, Python 3.11.6, GCC 13.2; per file: UTF-8-demo.html 1.36x, nytimes_crlf.html 1.17x, example.html 1.09x, nytimes.html 1.08x, lorem_crfl.txt 1.03x, lorem.txt 1.02x):
- Plain runs ending at an escape or CRLF (<32 bytes ~84% of the time in HTML, ~98% in UTF-8) copy 32 bytes first and only copy the remaining 48 bytes when `e - cur > 32`; runs that fill a line still copy 80 bytes and fall directly into the post-wrap run without re-running `ctz` when `mask != 0`, or jump straight to `next_block` when `mask == 0`
- Encode the escape-stop flag (plain chars and CR) in the unused 4th byte of `qp_table` (`0x80`), removing `qp_plain` and the separate `c == CR` branch from the multi-byte escape loop; track `out` against `obase + line_end` (`line_out_end`, invariant across escapes) and stay in the multi-escape loop across soft line breaks while `cur < BLOCK`
- Test the next mask bit on single escapes with `mask >> (cur & 63) & 1` (compiles to a single `bt` instruction) and jump directly to `next_escape` when `mask` remains non-zero
- Count `b` up from `-input_len` and handle a leading `CRLF` at byte 0 before the main loop, eliminating `b + e > 0` and `input`/`input_len` from the hot loop and freeing registers
- Corpus per 2,000 encodes (perf stat): 631M to 542M cycles (-14%), 2.92B to 2.66B instructions, 419M to 393M branches, branch misses -46%

**September 22, 2026** - block bitmask rewrite, ~1.9x on the raw corpus versus 36849b5 (7,606 to 14,560 ops/sec, median of 11 alternating 2,000-run bench_zoom samples pinned to CPU 2, Ryzen 7 PRO 7840U, Python 3.11.6, GCC 13.2; per file: nytimes.html 2.14x, nytimes_crlf.html 1.80x, example.html 1.56x, lorem files 1.14-1.21x, UTF-8-demo.html 1.18x):
- Classify each 64-byte block once into a 64-bit escape mask and walk it with ctz, instead of starting a fresh SIMD scan at every run (which reclassified bytes and paid per-run setup)
- Copy each plain run with one fixed 80-byte copy and each escape with one table store; no data-dependent copy loops
- Track position with `obase`/`line_end` instead of an output index and line length, so a plain run updates no bookkeeping
- A run reaching the block end carries into the next block (negative `cur`) instead of spending an iteration copying up to the boundary
- A single escape skips the escape loop (next-bit test on the mask); trailing whitespace is rewritten after the fact instead of checked for on every run; leading dots are handled where lines start
- The end of input is found through CR padding in a stack copy of the last <144 bytes, so the hot loop has no end-of-input checks
- nytimes.html per encode (perf stat): 897k to 610k instructions, 224k to 127k cycles. IPC is ~4.8, so instruction count is what matters
- The portable fallback classifies 8 bytes at a time with 64-bit SWAR arithmetic (checked exhaustively: every byte value in every lane next to every other value): 2.1x faster than the old scalar path on the corpus, but ~10% slower on UTF-8-demo.html
- Output is byte-identical to 36849b5: 100,000 random inputs plus every tail length and block alignment of the corpus, for both the SSE2 and scalar builds. New unit tests cover block edges, every input length through 300, and random mixes of state-changing pieces; each was checked against deliberately broken builds. Valgrind is clean on both builds. The unchanged repository fuzzer passed all 100,000 cases.

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
- **AVX2 in the block design** (32-byte classification plus ymm copies, whole module built with `-mavx2`, September 2026): ~3% fewer cycles on nytimes.html and ~10% *more* on UTF-8-demo.html. Still not worth runtime dispatch.
- **Counted loop for runs of escapes** (length from the mask, capped by a divide-by-3 line fit): fewer instructions on UTF-8 text, but the corpus came out 1.3% slower (HTML files ~5% slower). The loop that checks a stop flag per byte stays (now the high bit of `qp_table`'s fourth byte, which replaced `qp_plain`).
- **Classifying without the tab test** (November 2026; tabs would become events, with a switch back to the full test for tab-heavy text): 9% fewer instructions, but only 1-3% fewer cycles on tab-free HTML (8-9% on plain text). The SIMD classification is not what limits HTML; the integer work per escape is. Not worth a second classifier and the mode logic.
- **BMI and AVX2 again, on the November 2026 loop**: building with `-mbmi -mbmi2 -mlzcnt` (tzcnt, blsr, shlx) gains 0-2%. Adding `-mavx2` on top is 8-9% *slower* on the nytimes files and 21% slower on UTF-8-demo.html. There is nothing here worth runtime dispatch.
- **An input-to-output offset instead of an output pointer** (`out = in + delta`): GCC computes `cur + delta` into a register for every copy, which gives back what the fewer updates save.
- **Layout hints on the CR test**: with or without `unlikely`, LF and CRLF text measure the same.
- **Known and left alone**: inputs under 144 bytes are encoded entirely from the tail, right after it is written, and lose ~50 cycles to store forwarding. Avoiding it means building the last partial 16-byte chunk in a register, which needs variable byte shifts SSE2 doesn't have.
- **Benchmark caveat**: the corpus repeats, so the branch predictor learns it (~200 mispredicts per nytimes.html encode). Branchy variants therefore look better here than on fresh email bodies; when results tie, prefer fixed-size, branch-free work.
