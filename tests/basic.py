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


class BasicTests(unittest.TestCase):
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
            
