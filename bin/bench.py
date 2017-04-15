import timeit
import unittest
import zoomascii
import binascii
import cStringIO
from os import listdir, path

# get the pure-python code
import quopri
quopri.b2a_qp = None

corpus = []
data_dir = path.dirname(__file__) + '/../data'
for fname in listdir(data_dir):
    with open(data_dir + '/' + fname, 'r') as fh:
        data = ''.join(fh.readlines())
        corpus.append(data)

def _quopri_qp():
    for data in corpus:
        data_fh = cStringIO.StringIO(data)
        out_fh = cStringIO.StringIO()        
        quopri.encode(data_fh, out_fh, False)

def _base_qp():
    for data in corpus:
        res = binascii.b2a_qp(data)

def _zoom_qp():
    for data in corpus:
        res = zoomascii.b2a_qp(data)


n = 1000
t1 = timeit.timeit(_zoom_qp, number=n)
print("zoom qp: %d in %0.1fs - %0.2f/s" % (n, t1, n/t1))
t = timeit.timeit(_base_qp, number=n)
print("base qp: %d in %0.1fs - %0.2f/s" % (n, t, n/t))

n = n/100
t = timeit.timeit(_quopri_qp, number=n)
print("quopri qp: %d in %0.1fs - %0.2f/s" % (n, t, n/t))


