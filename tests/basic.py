import unittest
import zoomascii

from os import listdir, path

corpus = []
data_dir = path.dirname(__file__) + '/../data'
for fname in listdir(data_dir):
    with open(data_dir + '/' + fname, 'rb') as fh:
        data = ''.join(fh.readlines())
        corpus.append(data)


class BasicTests(unittest.TestCase):
    def test_swapcase(self):
        self.assertEqual(zoomascii.swapcase("dude"), 'DUDE')

        for data in corpus:
            self.assertEqual(zoomascii.swapcase(data), data.swapcase())
            
