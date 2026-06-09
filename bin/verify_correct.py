"""Verify zoomascii.b2a_qp output decodes correctly and matches expectations.

zoomascii differs from binascii.b2a_qp in allowed ways (line break positions,
leading-dot encoding), so we verify by round-tripping through binascii.a2b_qp
like fuzz_test.py does, across the corpus plus edge cases.
"""
import binascii
import os
import sys

import zoomascii

failures = 0

def check(data, label):
    global failures
    encoded = zoomascii.b2a_qp(data, encode_leading_dot=0)
    decoded = binascii.a2b_qp(encoded)
    if decoded != data:
        failures += 1
        print("ROUNDTRIP FAIL (%s): %r => %r => %r" % (label, data[:80], encoded[:80], decoded[:80]))
        return
    # check line lengths and trailing whitespace rules
    for line in encoded.split(b'\r\n'):
        if len(line) > 76:
            failures += 1
            print("LINE LENGTH FAIL (%s): %d chars: %r" % (label, len(line), line))
        if line.endswith(b' ') or line.endswith(b'\t'):
            failures += 1
            print("TRAILING WS FAIL (%s): %r" % (label, line))
    # leading dot encoding for SMTP
    encoded_dot = zoomascii.b2a_qp(data, encode_leading_dot=1)
    if binascii.a2b_qp(encoded_dot) != data:
        failures += 1
        print("DOT ROUNDTRIP FAIL (%s)" % label)
    for line in encoded_dot.split(b'\r\n'):
        if line.startswith(b'.'):
            failures += 1
            print("LEADING DOT FAIL (%s): %r" % (label, line))

data_dir = os.path.join(os.path.dirname(__file__), '..', 'data')
for fname in sorted(os.listdir(data_dir)):
    with open(os.path.join(data_dir, fname), 'rb') as fh:
        check(fh.read(), fname)

edge_cases = [
    b'',
    b'a',
    b'=',
    b'.',
    b'.leading dot',
    b'line\r\n.dotline\r\n',
    b' ',
    b'\t',
    b'trailing space \r\n',
    b'trailing tab\t\r\n',
    b'ends with space ',
    b'ends with tab\t',
    b'\r\n',
    b'\r',
    b'\n',
    b'\r\r\n',
    b'a' * 71,
    b'a' * 72,
    b'a' * 73,
    b'a' * 1000,
    b'=' * 100,
    bytes(range(256)),
    bytes(range(256)) * 50,
    b'\xff' * 5000,
    b'space then cr \rmore',
    b'space at 71 chars' + b'x' * 53 + b' \r\nnext',
    b'a' * 70 + b' \r\n',
    b'a' * 71 + b' \r\n',
    b'a' * 72 + b' \r\n',
    b'mixed \xc3\xa9 utf8 \xe2\x82\xac text\r\nwith lines\r\n',
    b'. ',
    b'.\r\n.\r\n.',
    b'tab\there',
    b' .dot after space',
]
for idx, data in enumerate(edge_cases):
    check(data, 'edge_%d' % idx)

# pseudo-random binary data, deterministic
import random
rng = random.Random(42)
for trial in range(200):
    n = rng.randint(0, 2000)
    data = bytes(rng.randrange(256) for _ in range(n))
    check(data, 'random_%d' % trial)
# random text-ish data
for trial in range(200):
    n = rng.randint(0, 2000)
    data = bytes(rng.choice(b'abcdefghij .=\r\n\t ') for _ in range(n))
    check(data, 'randtext_%d' % trial)

if failures:
    print("%d FAILURES" % failures)
    sys.exit(1)
print("all correctness checks passed")
