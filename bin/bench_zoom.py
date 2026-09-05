"""Benchmark b2a_qp, optionally alternating with a saved baseline build.

Before editing the encoder, save a baseline with:
    python3 setup.py build_ext --build-lib /tmp/qp-before --force
Then rebuild in place and compare on one CPU:
    python3 setup.py build_ext --inplace --force
    PYTHONPATH=. taskset -c 2 python3 bin/bench_zoom.py \
        --baseline /tmp/qp-before --per-file
"""
import argparse
import timeit
from os import listdir, path

import zoomascii

corpus = []
names = []
data_dir = path.dirname(__file__) + '/../data'
for fname in sorted(listdir(data_dir)):
    with open(data_dir + '/' + fname, 'rb') as fh:
        corpus.append(fh.read())
        names.append(fname)


def benchmark(encoders, payloads, number, repeat):
    def run(encoder):
        def encode():
            for data in payloads:
                encoder(data)
        return encode

    timers = [timeit.Timer(run(encoder)) for encoder in encoders]
    for timer in timers:
        timer.timeit(number=20)
    samples = [[] for timer in timers]
    # Alternate which build runs first so gradual changes in CPU speed
    # or load do not consistently favor one implementation.
    for trial in range(repeat):
        order = range(len(timers))
        if trial % 2:
            order = reversed(order)
        for index in order:
            samples[index].append(timers[index].timeit(number=number))
    return [(number / min(times), number / sorted(times)[len(times) // 2])
            for times in samples]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', help='directory containing a saved extension build')
    parser.add_argument('--per-file', action='store_true', help='also measure each corpus file')
    parser.add_argument('--number', type=int, default=200, help='encodes per sample (default: 200)')
    parser.add_argument('--repeat', type=int, default=10, help='samples per build (default: 10)')
    args = parser.parse_args()
    if args.number < 1 or args.repeat < 1:
        parser.error('--number and --repeat must be positive')

    encoders = [zoomascii.b2a_qp]
    labels = ['zoom qp']
    if args.baseline:
        import importlib.util
        from importlib.machinery import EXTENSION_SUFFIXES
        libraries = [path.join(args.baseline, 'zoomascii' + suffix)
                     for suffix in EXTENSION_SUFFIXES
                     if path.isfile(path.join(args.baseline, 'zoomascii' + suffix))]
        if len(libraries) != 1:
            parser.error('--baseline must contain exactly one compatible zoomascii extension')
        spec = importlib.util.spec_from_file_location('baseline.zoomascii', libraries[0])
        baseline = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(baseline)
        encoders.insert(0, baseline.b2a_qp)
        labels.insert(0, 'baseline')
        for data in corpus:
            for dots in (False, True):
                if baseline.b2a_qp(data, dots) != zoomascii.b2a_qp(data, dots):
                    parser.error('current and baseline encoders produced different output')

    cases = [('corpus', corpus)]
    if args.per_file:
        cases.extend((name, [data]) for name, data in zip(names, corpus))
    for name, payloads in cases:
        results = benchmark(encoders, payloads, args.number, args.repeat)
        for label, (best, median) in zip(labels, results):
            print('%s %s best: %.2f ops/s (median %.2f)' % (name, label, best, median))
        if args.baseline:
            print('%s median speedup: %.3fx' % (name, results[1][1] / results[0][1]))


if __name__ == '__main__':
    main()
