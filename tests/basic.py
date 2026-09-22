import random
import unittest
import zoomascii
import binascii

from os import listdir, path

corpus = []
data_dir = path.dirname(__file__) + '/../data'
for fname in listdir(data_dir):
    with open(data_dir + '/' + fname, 'rb') as fh:
        data = fh.read()
        corpus.append(data)


def reference_qp(data, encode_leading_dot):
    """Encode one byte at a time, preserving zoomascii's line wrapping."""
    result = []
    values = bytearray(data)
    line_len = 0
    i = 0
    while i < len(data):
        if data[i:i + 2] == b'\r\n':
            result.append(b'\r\n')
            line_len = 0
            i += 2
            continue
        c = values[i]
        plain = c == 9 or (32 <= c <= 126 and c != 61)
        if c in (9, 32) and (i + 1 == len(data) or
                             data[i + 1:i + 3] == b'\r\n'):
            plain = False
        if c == 46 and line_len == 0 and encode_leading_dot:
            plain = False
        piece = data[i:i + 1]
        if not plain:
            piece = b'=' + binascii.hexlify(piece).upper()
        result.append(piece)
        line_len += len(piece)
        i += 1
        if line_len >= 72:
            result.append(b'=\r\n')
            line_len = 0
    return b''.join(result)


class BasicTests(unittest.TestCase):
    def assert_qp_matches_reference(self, data):
        for dots in (False, True):
            self.assertEqual(zoomascii.b2a_qp(data, dots),
                             reference_qp(data, dots))

    def test_qp_vector_lanes(self):
        # Every byte value in every SIMD lane, with both a short tail
        # and enough following input for additional vector loads.
        for offset in range(16):
            for value in range(256):
                for tail in (b'', b'z' * 80):
                    data = b'a' * offset + bytes(bytearray([value])) + tail
                    self.assert_qp_matches_reference(data)

    def test_qp_run_boundaries(self):
        # Runs ending at escapes, whitespace, CRLF, and soft line breaks.
        tails = (b'=', b'===', b'\xc3\xa9\xe2\x82\xac', b'\r', b'\n',
                 b'\r\n', b' \r\n', b'\t\r\n', b'  ', b'\t', b'.')
        for offset in range(65, 81):
            for tail in tails:
                self.assert_qp_matches_reference(b'a' * offset + tail)
                self.assert_qp_matches_reference(
                    b'a' * offset + tail + b'.next\r\n. ')

    def test_qp_block_edges(self):
        # The encoder works in 64-byte blocks and carries plain runs,
        # escape runs and CRLFs across block edges. Slide each piece
        # past every block and line position.
        pieces = (b' \r\n.', b'\t\r\n', b'\r\n', b'\r\r\n', b' ',
                  b'\xc3\xa9' * 30, b'=' * 25 + b'.')
        for offset in range(200):
            for piece in pieces:
                self.assert_qp_matches_reference(
                    b'a' * offset + piece + b'b.' * (offset % 5))

    def test_qp_input_lengths(self):
        # The end of the input is encoded from a padded copy. Check
        # every length through several blocks, ending in each kind of
        # character.
        for length in range(1, 300):
            for last in (b'a', b' ', b'\t', b'=', b'\r', b'.', b'\xff'):
                for fill in (b'a' * 300, b'abc=\r\n' * 50):
                    data = fill[:length - 1] + last
                    self.assert_qp_matches_reference(data)

    def test_qp_random_structure(self):
        # Random mixes of the pieces that change the encoder's state.
        rng = random.Random(2026)
        pieces = (b'a', b'b' * 15, b'c' * 40, b'd' * 71, b' ', b'\t', b'=',
                  b'.', b'\r\n', b'\r', b'\n', b' \r\n', b'\xc3\xa9',
                  b'\xe2\x82\xac' * 4, b'\x00\xff')
        for trial in range(1000):
            data = b''.join(rng.choice(pieces)
                            for _ in range(rng.randint(1, 40)))
            self.assert_qp_matches_reference(data)

    def test_qp_output_growth(self):
        for length in (1365, 2048, 4095, 4096, 4097, 65536):
            self.assert_qp_matches_reference(b'=' * length)

    def test_qp_releases_input_buffer(self):
        for data in (b'', b'short', b'a=\r\n' * 1000):
            value = bytearray(data)
            self.assertEqual(zoomascii.b2a_qp(value), reference_qp(data, True))
            value.extend(b'x')  # Fails if the encoder still holds a buffer.
            value = bytearray(data)
            view = memoryview(value)
            self.assertEqual(zoomascii.b2a_qp(view), reference_qp(data, True))
            del view
            value.extend(b'x')

    def test_qp_basic(self):
        self.assertEqual(zoomascii.b2a_qp("dude"), b'dude')
        self.assertEqual(zoomascii.b2a_qp("dude\t\r\n"), b"dude=09\r\n")
        self.assertEqual(zoomascii.b2a_qp("dude   "), b"dude  =20")

        # test dot encoding option
        self.assertEqual(zoomascii.b2a_qp("dude   \r\n.foo"),
                         b"dude  =20\r\n=2Efoo")
        self.assertEqual(zoomascii.b2a_qp("dude   \r\n.foo",
                                          encode_leading_dot=False),
                         b"dude  =20\r\n.foo")

        # worst case string expansion
        self.assertEqual(zoomascii.b2a_qp("=" * 10),
                         b'=3D' * 10)
        self.assertEqual(zoomascii.b2a_qp("=" * 100),
                         b'=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=\r\n=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=\r\n=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=\r\n=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=\r\n=3D=3D=3D=3D')

    def test_qp_edge_cases(self):
        # Empty string
        self.assertEqual(zoomascii.b2a_qp(""), b'')
        
        # Single characters that need encoding (using bytes input)
        self.assertEqual(zoomascii.b2a_qp(b"\x00"), b'=00')
        self.assertEqual(zoomascii.b2a_qp(b"\x01"), b'=01')
        self.assertEqual(zoomascii.b2a_qp(b"\x1F"), b'=1F')  # below 33
        self.assertEqual(zoomascii.b2a_qp(b"\x7F"), b'=7F')  # above 126
        self.assertEqual(zoomascii.b2a_qp(b"\xFF"), b'=FF')  # high byte
        
        # All characters that should NOT be encoded (33-126 except =)
        # Test in smaller chunks to avoid line length limits
        for start in range(33, 127, 20):
            end = min(start + 20, 127)
            safe_chars = ''.join(chr(i) for i in range(start, end) if i != 61)
            encoded = zoomascii.b2a_qp(safe_chars)
            # Should decode back correctly even if line breaks added
            self.assertEqual(binascii.a2b_qp(encoded), safe_chars.encode('ascii'))
        
        # Line length edge cases - check that decode works correctly
        exactly_72 = 'A' * 72
        result = zoomascii.b2a_qp(exactly_72)
        self.assertEqual(binascii.a2b_qp(result), exactly_72.encode('ascii'))
        
        # Just over 72 chars should trigger soft line break
        over_72 = 'A' * 73
        result = zoomascii.b2a_qp(over_72)
        self.assertIn(b'=\r\n', result)  # Should contain soft line break
        self.assertEqual(binascii.a2b_qp(result), over_72.encode('ascii'))

    def test_qp_line_endings(self):
        # Standalone CR (should be encoded)
        self.assertEqual(zoomascii.b2a_qp("\r"), b'=0D')
        
        # Standalone LF (should be encoded)  
        self.assertEqual(zoomascii.b2a_qp("\n"), b'=0A')
        
        # CRLF pairs (should pass through)
        self.assertEqual(zoomascii.b2a_qp("\r\n"), b'\r\n')
        self.assertEqual(zoomascii.b2a_qp("test\r\nline"), b'test\r\nline')
        
        # Mixed line endings
        self.assertEqual(zoomascii.b2a_qp("a\rb\nc\r\nd"), b'a=0Db=0Ac\r\nd')

    def test_qp_spaces_and_tabs(self):
        # Trailing spaces (should be encoded)
        self.assertEqual(zoomascii.b2a_qp("test "), b'test=20')
        self.assertEqual(zoomascii.b2a_qp("test  "), b'test =20')
        
        # Trailing tabs (should be encoded)
        self.assertEqual(zoomascii.b2a_qp("test\t"), b'test=09')
        
        # Spaces before CRLF (should be encoded)
        self.assertEqual(zoomascii.b2a_qp("test \r\n"), b'test=20\r\n')
        self.assertEqual(zoomascii.b2a_qp("test\t\r\n"), b'test=09\r\n')
        
        # Internal spaces (should NOT be encoded)
        self.assertEqual(zoomascii.b2a_qp("test test"), b'test test')

    def test_qp_corpus_compatibility(self):
        # can't test for equality of output for zoomascii VS binascii
        # - QP isn't a deterministic format and zoomascii will make
        # different decisions about the data
        for data in corpus:
            zoom_encoded = zoomascii.b2a_qp(data)
            self.assertEqual(binascii.a2b_qp(zoom_encoded), data)

    def test_qp_error_handling(self):
        # Test with None (should raise TypeError)
        with self.assertRaises(TypeError):
            zoomascii.b2a_qp(None)
            
        # Test with int (should raise TypeError)
        with self.assertRaises(TypeError):
            zoomascii.b2a_qp(123)

    def test_qp_large_input(self):
        # Test with large input to verify memory handling
        large_input = "A" * 100000
        result = zoomascii.b2a_qp(large_input)
        # Should contain soft line breaks
        self.assertIn(b'=\r\n', result)
        # Should decode back correctly
        self.assertEqual(binascii.a2b_qp(result), large_input.encode('ascii'))

    def test_swapcase_basic(self):
        self.assertEqual(zoomascii.swapcase("dude"), b'DUDE')
        self.assertEqual(zoomascii.swapcase("HELLO"), b'hello')
        self.assertEqual(zoomascii.swapcase("MiXeD"), b'mIxEd')

    def test_swapcase_edge_cases(self):
        # Empty string
        self.assertEqual(zoomascii.swapcase(""), b'')
        
        # Single characters
        self.assertEqual(zoomascii.swapcase("a"), b'A')
        self.assertEqual(zoomascii.swapcase("Z"), b'z')
        
        # Numbers and symbols (should be unchanged)
        self.assertEqual(zoomascii.swapcase("123"), b'123')
        self.assertEqual(zoomascii.swapcase("!@#$%"), b'!@#$%')
        
        # Mixed content
        self.assertEqual(zoomascii.swapcase("Hello123World!"), b'hELLO123wORLD!')
        
        # Whitespace (should be unchanged)
        self.assertEqual(zoomascii.swapcase("  \t\n\r  "), b'  \t\n\r  ')

    def test_swapcase_error_handling(self):
        # Test with None (should raise TypeError)
        with self.assertRaises(TypeError):
            zoomascii.swapcase(None)
            
        # Test with int (should raise TypeError)  
        with self.assertRaises(TypeError):
            zoomascii.swapcase(123)

    def test_swapcase_large_input(self):
        # Test with large input
        large_input = ("Hello World! " * 10000).encode('ascii')
        result = zoomascii.swapcase(large_input)
        expected = large_input.swapcase()
        self.assertEqual(result, expected)

    def test_swapcase_corpus_compatibility(self):
        for data in corpus:
            self.assertEqual(zoomascii.swapcase(data), data.swapcase())
            
