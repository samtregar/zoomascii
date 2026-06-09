"""Tight benchmark of zoomascii.b2a_qp only: best-of-N to reduce noise."""
import timeit
from os import listdir, path

import zoomascii

corpus = []
data_dir = path.dirname(__file__) + '/../data'
for fname in sorted(listdir(data_dir)):
    with open(data_dir + '/' + fname, 'rb') as fh:
        corpus.append(fh.read())

def _zoom_qp():
    for data in corpus:
        zoomascii.b2a_qp(data)

n = 200
times = timeit.repeat(_zoom_qp, number=n, repeat=10)
best = min(times)
print("zoom qp best: %0.2f ops/s (median %0.2f)" % (
    n / best, n / sorted(times)[len(times) // 2]))
