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
    def test_qp(self):
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


        # can't test for equality of output for zoomascii VS binascii
        # - QP isn't a deterministic format and zoomascii will make
        # different decisions about the data
        for data in corpus:
            zoom_encoded = zoomascii.b2a_qp(data)
            self.assertEqual(binascii.a2b_qp(zoom_encoded),
                             data)

    def test_swapcase(self):
        self.assertEqual(zoomascii.swapcase("dude"), b'DUDE')

        for data in corpus:
            self.assertEqual(zoomascii.swapcase(data), data.swapcase())
            
