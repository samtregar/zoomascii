import timeit
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

def _base_swapcase():
    for data in corpus:
        res = data.swapcase()

def _zoom_swapcase():
    for data in corpus:
        res = zoomascii.swapcase(data)

def _base_hex():
    for data in corpus:
        res = binascii.b2a_hex(data)

def _zoom_hex():
    for data in corpus:
        res = zoomascii.b2a_hex(data)

def _base_qp():
    for data in corpus:
        res = binascii.b2a_qp(data)

def _zoom_qp():
    for data in corpus:
        res = zoomascii.b2a_qp(data)

base_n = 2000

class BenchTests(unittest.TestCase):
    def test_swapcase(self):
        n = base_n
        t = timeit.timeit(_base_swapcase, number=n)
        print("base swapcase: %d in %0.1fs - %0.2f/s" % (n, t, n/t))
        t1 = timeit.timeit(_zoom_swapcase, number=n)
        print("zoom swapcase: %d in %0.1fs - %0.2f/s" % (n, t1, n/t1))
        self.assertTrue(t > t1)

    def test_qp(self):
        n = base_n * 2
        t1 = timeit.timeit(_zoom_qp, number=n)
        print("zoom qp: %d in %0.1fs - %0.2f/s" % (n, t1, n/t1))
        
        t = timeit.timeit(_base_qp, number=n)
        print("base qp: %d in %0.1fs - %0.2f/s" % (n, t, n/t))
        self.assertTrue(t > t1)
