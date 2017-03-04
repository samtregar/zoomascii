import timeit
import unittest
import zoomascii
from os import listdir, path

corpus = []
data_dir = path.dirname(__file__) + '/../data'
for fname in listdir(data_dir):
    with open(data_dir + '/' + fname, 'r') as fh:
        data = ''.join(fh.readlines())
        corpus.append(data)

def _base_swapcase():
    for data in corpus:
        res = data.swapcase()

def _zoom_swapcase():
    for data in corpus:
        res = zoomascii.swapcase(data)

n = 2000

class BenchTests(unittest.TestCase):
    def test_swapcase(self):
        t = timeit.timeit(_base_swapcase, number=n)
        print "base swapcase: %d in %0.1fs - %0.2f/s" % (n, t, n/t)
        t1 = timeit.timeit(_zoom_swapcase, number=n)
        print "zoom swapcase: %d in %0.1fs - %0.2f/s" % (n, t1, n/t1)
        self.assertTrue(t > t1)
