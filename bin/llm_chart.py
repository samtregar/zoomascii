"""Chart b2a_qp speed at each speed-up since the last human-only release.

Usage:
    taskset -c 2 python3 bin/llm_chart.py --measure
    python3 bin/llm_chart.py

--measure builds every commit in MILESTONES with the running Python,
times them in alternating rounds against binascii.b2a_qp on the
bin/bench.py corpus, and saves the results in
docs/benchmarks/llm_speedups.json. Old and new builds are timed in the
same process, so the numbers compare with each other; they will not
match figures quoted from other days or machines.

Without --measure the saved results are redrawn. Either way this
writes docs/benchmarks/llm_speedups.svg (light) and
llm_speedups-dark.svg, and prints the table LLM.md shows under the
chart.

When a new speed-up lands, add a row to MILESTONES and run --measure.
"""
import argparse
import binascii
import datetime
import importlib.util
import io
import json
import os
import platform
import subprocess
import sys
import tarfile
import tempfile
import timeit
from importlib.machinery import EXTENSION_SUFFIXES

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
OUT_DIR = os.path.join(ROOT, 'docs', 'benchmarks')
RESULTS = os.path.join(OUT_DIR, 'llm_speedups.json')

# One row per commit that made b2a_qp faster, oldest first. The first
# row is the baseline: the last release written without an LLM.
#   commit, date, maker, label on the chart, full name, release
MILESTONES = [
    ('1ab699a', '2025-03-04', 'human', 'Human only', 'Human only (Sam Tregar)', '1.0'),
    ('ab67a56', '2025-08-16', 'anthropic', 'Opus 4.1', 'Claude Opus 4.1', None),
    ('afb42dd', '2026-06-09', 'anthropic', 'Fable 5', 'Claude Fable 5', '1.1'),
    ('7c75f1c', '2026-09-01', 'anthropic', 'Fable 5.1', 'Claude Fable 5.1', '1.2'),
    ('36849b5', '2026-09-05', 'openai', 'GPT-6 Astra', 'GPT-6 Astra', '1.3'),
    ('d94dc1b', '2026-09-22', 'anthropic', 'Opus 5.5', 'Claude Opus 5.5', '1.4'),
    ('cc3ec95', '2026-10-14', 'google', 'Gemini 4', 'Gemini 4', '1.5'),
    ('0119bd0', '2026-11-09', 'anthropic', 'Fable 5.2', 'Claude Fable 5.2', '1.6'),
]

# Colour follows the maker, in this fixed order, whichever makers a
# given chart happens to contain.
MAKERS = [
    ('human', 'Human'),
    ('anthropic', 'Claude (Anthropic)'),
    ('openai', 'GPT (OpenAI)'),
    ('google', 'Gemini (Google)'),
]

THEMES = {
    'light': dict(surface='#fcfcfb', ink='#0b0b0b', ink2='#52514e',
                  muted='#898781', grid='#e1e0d9', axis='#c3c2b7',
                  human='#898781', anthropic='#2a78d6', openai='#eb6834',
                  google='#1baf7a'),
    'dark':  dict(surface='#1a1a19', ink='#ffffff', ink2='#c3c2b7',
                  muted='#898781', grid='#2c2c2a', axis='#383835',
                  human='#898781', anthropic='#3987e5', openai='#d95926',
                  google='#199e70'),
}

FONT = 'system-ui, -apple-system, "Segoe UI", Helvetica, Arial, sans-serif'

W = 760                       # wide enough for a full date under every point
LEFT, RIGHT = 52, 24          # plot edges
FIRST_X = LEFT + 84           # room for the first point's label
LAST_X = W - RIGHT - 24       # and for the last point's date
PLOT_TOP, PLOT_H = 112, 270
DOT = 5                       # marker radius


# ---------------------------------------------------------------- measuring

def load_corpus():
    """The corpus exactly as bin/bench.py reads it: text mode, so CRLF
    files arrive with LF line endings."""
    data_dir = os.path.join(ROOT, 'data')
    corpus = []
    for fname in sorted(os.listdir(data_dir)):
        with open(os.path.join(data_dir, fname), 'r', encoding='utf-8') as fh:
            corpus.append(fh.read().encode('utf-8'))
    return corpus


def build(commit, workdir):
    """Build one commit's extension out of tree and return its b2a_qp."""
    src = os.path.join(workdir, 'src-' + commit)
    lib = os.path.join(workdir, 'lib-' + commit)
    archive = subprocess.run(['git', '-C', ROOT, 'archive', '--format=tar', commit],
                             check=True, stdout=subprocess.PIPE).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        try:
            tar.extractall(src, filter='data')
        except TypeError:       # Python without extraction filters
            tar.extractall(src)
    done = subprocess.run([sys.executable, 'setup.py', '-q', 'build_ext', '--force',
                           '--build-lib', lib], cwd=src,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if done.returncode:
        sys.exit('building %s failed:\n%s' % (commit, done.stdout.decode()))
    for suffix in EXTENSION_SUFFIXES:
        path = os.path.join(lib, 'zoomascii' + suffix)
        if os.path.isfile(path):
            break
    else:
        sys.exit('no extension was built for %s' % commit)
    spec = importlib.util.spec_from_file_location('at_%s.zoomascii' % commit, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.b2a_qp


def cpu_name():
    try:
        with open('/proc/cpuinfo') as fh:
            for line in fh:
                if line.startswith('model name'):
                    return line.split(':', 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def measure(rounds):
    corpus = load_corpus()
    with tempfile.TemporaryDirectory(prefix='zoomascii-llm-chart-') as workdir:
        encoders = [binascii.b2a_qp]
        for commit, _, _, _, name, _ in MILESTONES:
            print('building %s (%s)' % (commit, name))
            encoder = build(commit, workdir)
            for data in corpus:
                if binascii.a2b_qp(encoder(data)) != data:
                    sys.exit('%s does not round-trip the corpus' % commit)
            encoders.append(encoder)

        def passes(encoder):
            def run():
                for data in corpus:
                    encoder(data)
            return run

        timers = [timeit.Timer(passes(encoder)) for encoder in encoders]
        # Each sample runs as many passes as take 0.2s or more, so the
        # fastest builds are not timed over a few milliseconds.
        numbers = [timer.autorange()[0] for timer in timers]
        times = [[] for timer in timers]
        # Alternate the order so drifting CPU speed or load does not
        # consistently favour the builds timed first.
        for trial in range(rounds):
            order = list(range(len(timers)))
            if trial % 2:
                order.reverse()
            for index in order:
                times[index].append(timers[index].timeit(number=numbers[index])
                                    / numbers[index])
            print('round %d of %d' % (trial + 1, rounds))

    def rates(samples):
        return dict(best=round(1 / min(samples), 2),
                    median=round(1 / sorted(samples)[len(samples) // 2], 2))

    return dict(
        measured=datetime.date.today().isoformat(),
        python=platform.python_version(),
        cpu=cpu_name(),
        samples=rounds,
        binascii=rates(times[0]),
        builds=dict((row[0], rates(samples))
                    for row, samples in zip(MILESTONES, times[1:])),
    )


# ------------------------------------------------------------------ drawing

def fmt_multiple(value):
    return ('%.1fx' % value) if value < 9.95 else ('%dx' % round(value))


def fmt_date(iso):
    date = datetime.date(*(int(part) for part in iso.split('-')))
    return '%s %d, %d' % (date.strftime('%b'), date.day, date.year)


def text_width(text, size):
    """Rough width of sans-serif text, good enough to space a legend."""
    ems = sum(0.3 if char in ' .,:;()il' else 0.68 if char.isupper() or char in 'mw'
              else 0.56 for char in text)
    return ems * size


def tick_step(top_value, max_intervals=6):
    """Round 1, 2 or 5 times a power of ten."""
    magnitude = 1
    while magnitude * 10 <= top_value / max_intervals:
        magnitude *= 10
    for multiple in (1, 2, 5, 10):
        if multiple * magnitude * max_intervals >= top_value:
            return multiple * magnitude


def points(results):
    """(row, encodes per second, multiple of binascii) per milestone."""
    base = results['binascii']['best']
    return [(row, results['builds'][row[0]]['best'],
             results['builds'][row[0]]['best'] / base) for row in MILESTONES]


def render(theme, results):
    t = THEMES[theme]
    pts = points(results)
    top_value = max(multiple for _, _, multiple in pts)
    # headroom above the highest point for its two-line label
    step = tick_step(top_value * 1.15)
    axis_max = step
    while axis_max < top_value * 1.15:
        axis_max += step
    baseline_y = PLOT_TOP + PLOT_H
    pitch = (LAST_X - FIRST_X) / float(len(pts) - 1)
    height = baseline_y + 74

    def x(index):
        return FIRST_X + pitch * index

    def y(value):
        return baseline_y - PLOT_H * value / axis_max

    when = datetime.date(*(int(part) for part in results['measured'].split('-')))
    python = '.'.join(results['python'].split('.')[:2])
    subtitle = ('Times faster than binascii.b2a_qp · 472 KB corpus, Python %s, one core'
                ' · measured %s' % (python, when.strftime('%B %Y')))

    out = []
    out.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
               'viewBox="0 0 %d %d" role="img" aria-labelledby="t d">' % (W, height, W, height))
    out.append('<title id="t">b2a_qp speed-ups since the last human-only release</title>')
    out.append('<desc id="d">%s</desc>' % '; '.join(
        '%s, %s: %s faster than binascii.b2a_qp' % (row[4], fmt_date(row[1]),
                                                     fmt_multiple(multiple))
        for row, _, multiple in pts))
    out.append('<rect width="%d" height="%d" fill="%s"/>' % (W, height, t['surface']))
    out.append('<g font-family=\'%s\'>' % FONT)
    out.append('<text x="24" y="32" font-size="16" font-weight="600" fill="%s">'
               'b2a_qp speed-ups since the last human-only release</text>' % t['ink'])
    out.append('<text x="24" y="52" font-size="12" fill="%s">%s</text>'
               % (t['ink2'], subtitle))

    # legend: every maker on the chart, in MAKERS order
    present = set(row[2] for row, _, _ in pts)
    legend_x = 24
    for key, name in MAKERS:
        if key not in present:
            continue
        out.append('<circle cx="%.1f" cy="78" r="%d" fill="%s"/>'
                   % (legend_x + DOT, DOT, t[key]))
        out.append('<text x="%.1f" y="82" font-size="12" fill="%s">%s</text>'
                   % (legend_x + 2 * DOT + 6, t['ink2'], name))
        legend_x += 2 * DOT + 6 + text_width(name, 12) + 20

    # hairline gridlines and y ticks
    for tick in range(0, axis_max + 1, step):
        gy = y(tick)
        out.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" stroke-width="1"/>'
                   % (LEFT, gy, W - RIGHT, gy, t['axis'] if tick == 0 else t['grid']))
        out.append('<text x="%d" y="%.1f" font-size="11" text-anchor="end" fill="%s" '
                   'style="font-variant-numeric: tabular-nums">%s</text>'
                   % (LEFT - 8, gy + 4, t['muted'], '%dx' % tick if tick else '0'))

    # x ticks: the commit's date, and the release it shipped in
    for index, (row, _, _) in enumerate(pts):
        out.append('<text x="%.1f" y="%d" font-size="11" text-anchor="middle" fill="%s">%s</text>'
                   % (x(index), baseline_y + 18, t['muted'], fmt_date(row[1])))
        if row[5]:
            out.append('<text x="%.1f" y="%d" font-size="11" text-anchor="middle" fill="%s">'
                       'v%s</text>' % (x(index), baseline_y + 33, t['muted'], row[5]))
    out.append('<text x="%.1f" y="%d" font-size="11" text-anchor="middle" fill="%s">'
               'Commits that made b2a_qp faster, in order (not to time scale)</text>'
               % ((LEFT + W - RIGHT) / 2.0, baseline_y + 58, t['muted']))

    # each rise is drawn in the colour of whoever built it
    for index in range(1, len(pts)):
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" '
                   'stroke-width="2" stroke-linecap="round"/>'
                   % (x(index - 1), y(pts[index - 1][2]), x(index), y(pts[index][2]),
                      t[pts[index][0][2]]))
    for index, (row, _, multiple) in enumerate(pts):
        out.append('<circle cx="%.1f" cy="%.1f" r="%d" fill="%s" stroke="%s" stroke-width="2"/>'
                   % (x(index), y(multiple), DOT, t[row[2]], t['surface']))

    # Labels sit above and to the left of their point. On a rising line
    # that corner is always empty, so they need no collision handling.
    # The surface-coloured outline keeps gridlines from striking through.
    halo = ('stroke="%s" stroke-width="4" stroke-linejoin="round" paint-order="stroke"'
            % t['surface'])
    for index, (row, _, multiple) in enumerate(pts):
        lx, ly = x(index) + 2, y(multiple) - DOT - 8
        out.append('<text x="%.1f" y="%.1f" font-size="12" font-weight="600" text-anchor="end" '
                   'fill="%s" %s>%s</text>' % (lx, ly - 15, t['ink'], halo, row[3]))
        out.append('<text x="%.1f" y="%.1f" font-size="12" text-anchor="end" fill="%s" %s '
                   'style="font-variant-numeric: tabular-nums">%s</text>'
                   % (lx, ly, t['ink2'], halo, fmt_multiple(multiple)))
    out.append('</g></svg>')
    return '\n'.join(out) + '\n'


def table(results):
    """The markdown table LLM.md carries under the chart."""
    lines = ['| Date | Commit | Built by | Release | Encodes/sec | vs binascii | Step |',
             '|---|---|---|---|---:|---:|---:|']
    previous = None
    for row, rate, multiple in points(results):
        commit, date, _, _, name, release = row
        lines.append('| %s | `%s` | %s | %s | %s | %s | %s |' % (
            fmt_date(date), commit, name, release or '-', '{:,}'.format(int(round(rate))),
            fmt_multiple(multiple), '%.2fx' % (rate / previous) if previous else '-'))
        previous = rate
    lines.append('')
    lines.append('binascii.b2a_qp: %s encodes/sec' % '{:,}'.format(
        int(round(results['binascii']['best']))))
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--measure', action='store_true',
                        help='rebuild and time every milestone before drawing')
    parser.add_argument('--rounds', type=int, default=15,
                        help='samples per build (default: 15)')
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error('--rounds must be positive')

    if args.measure:
        results = measure(args.rounds)
        with open(RESULTS, 'w') as fh:
            json.dump(results, fh, indent=2)
            fh.write('\n')
        print('wrote', os.path.relpath(RESULTS))
    else:
        with open(RESULTS) as fh:
            results = json.load(fh)
    missing = [row[0] for row in MILESTONES if row[0] not in results['builds']]
    if missing:
        sys.exit('no saved result for %s; run with --measure' % ', '.join(missing))

    for theme, fname in (('light', 'llm_speedups.svg'), ('dark', 'llm_speedups-dark.svg')):
        path = os.path.join(OUT_DIR, fname)
        with open(path, 'w') as fh:
            fh.write(render(theme, results))
        print('wrote', os.path.relpath(path))
    print()
    print(table(results))


if __name__ == '__main__':
    main()
