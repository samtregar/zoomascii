"""Render the README benchmark chart from bin/bench.py numbers.

Usage:
    python3 bin/bench_chart.py ZOOM BINASCII QUOPRI

where the three arguments are the ops/sec bin/bench.py printed for zoom
qp, base qp and quopri qp (take the best of a few pinned runs, they are
noisy). Writes docs/benchmarks/b2a_qp.svg (light) and
docs/benchmarks/b2a_qp-dark.svg; README.md picks between them with a
<picture> element so GitHub shows the right one for the viewer's theme.
"""
import datetime
import os
import sys

THEMES = {
    'light': dict(surface='#fcfcfb', ink='#0b0b0b', ink2='#52514e',
                  muted='#898781', grid='#e1e0d9', axis='#c3c2b7',
                  accent='#2a78d6', dim='#c3c2b7'),
    'dark':  dict(surface='#1a1a19', ink='#ffffff', ink2='#c3c2b7',
                  muted='#898781', grid='#2c2c2a', axis='#383835',
                  accent='#3987e5', dim='#4a4a47'),
}

FONT = 'system-ui, -apple-system, "Segoe UI", Helvetica, Arial, sans-serif'

W, H = 720, 250
LEFT, RIGHT, TOP = 200, 130, 78
BAR = 22          # bar thickness, capped at 24px
SLOT = 40         # row pitch, leaves air around each bar
RADIUS = 4


def fmt(v):
    return ('%.1f' % v) if v < 10 else ('{:,}'.format(int(round(v))))


def tick_step(top_value, max_intervals=6):
    """Round 1, 2 or 5 times a power of ten, so labels like "10,000"
    stay well apart however large the fastest encoder gets."""
    magnitude = 1
    while magnitude * 10 <= top_value / max_intervals:
        magnitude *= 10
    for multiple in (1, 2, 5, 10):
        if multiple * magnitude * max_intervals >= top_value:
            return multiple * magnitude


def render(theme, rows, subtitle):
    t = THEMES[theme]
    top_value = max(v for _, v, _ in rows)
    step = tick_step(top_value)
    # clean axis maximum: next tick above the largest bar
    axis_max = (int(top_value // step) + 1) * step
    plot_w = W - LEFT - RIGHT
    x0 = LEFT
    baseline_y = TOP + SLOT * len(rows)

    def x(v):
        return x0 + plot_w * v / axis_max

    out = []
    out.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
               'viewBox="0 0 %d %d" role="img" aria-labelledby="t d">' % (W, H, W, H))
    out.append('<title id="t">b2a_qp throughput</title>')
    out.append('<desc id="d">%s</desc>' % '; '.join(
        '%s %s per second' % (name, fmt(v)) for name, v, _ in rows))
    out.append('<rect width="%d" height="%d" fill="%s"/>' % (W, H, t['surface']))
    out.append('<g font-family=\'%s\'>' % FONT)
    out.append('<text x="24" y="32" font-size="16" font-weight="600" fill="%s">'
               'Quoted-printable encoding speed</text>' % t['ink'])
    out.append('<text x="24" y="52" font-size="12" fill="%s">%s</text>'
               % (t['ink2'], subtitle))

    # hairline gridlines and axis ticks
    for tick in range(0, axis_max + 1, step):
        gx = x(tick)
        out.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s" stroke-width="1"/>'
                   % (gx, TOP - 6, gx, baseline_y, t['grid']))
        out.append('<text x="%.1f" y="%d" font-size="11" text-anchor="middle" fill="%s" '
                   'style="font-variant-numeric: tabular-nums">%s</text>'
                   % (gx, baseline_y + 16, t['muted'], '{:,}'.format(tick)))
    out.append('<text x="%.1f" y="%d" font-size="11" text-anchor="middle" fill="%s">'
               'encodes per second (higher is better)</text>'
               % (x0 + plot_w / 2, baseline_y + 34, t['muted']))
    # baseline
    out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="1"/>'
               % (x0, TOP - 6, x0, baseline_y, t['axis']))

    for n, (name, value, emphasis) in enumerate(rows):
        y = TOP + SLOT * n + (SLOT - BAR) / 2
        w = x(value) - x0
        fill = t['accent'] if emphasis else t['dim']
        if w >= RADIUS:
            # square at the baseline, 4px rounded data-end
            path = ('M%.1f,%.1f h%.1f a%d,%d 0 0 1 %d,%d v%d a%d,%d 0 0 1 -%d,%d h-%.1f z'
                    % (x0, y, w - RADIUS, RADIUS, RADIUS, RADIUS, RADIUS,
                       BAR - 2 * RADIUS, RADIUS, RADIUS, RADIUS, RADIUS, w - RADIUS))
            out.append('<path d="%s" fill="%s"/>' % (path, fill))
        else:
            out.append('<rect x="%d" y="%.1f" width="%.2f" height="%d" fill="%s"/>'
                       % (x0, y, max(w, 1.0), BAR, fill))
        out.append('<text x="%d" y="%.1f" font-size="13" text-anchor="end" fill="%s" '
                   'font-weight="%s">%s</text>'
                   % (x0 - 12, y + BAR / 2 + 4.5, t['ink'], '600' if emphasis else '400', name))
        label = fmt(value) + ' /s'
        if not emphasis:
            label += '  (%dx slower)' % round(top_value / value)
        out.append('<text x="%.1f" y="%.1f" font-size="12" fill="%s" '
                   'style="font-variant-numeric: tabular-nums">%s</text>'
                   % (x0 + w + 8, y + BAR / 2 + 4, t['ink2'], label))
    out.append('</g></svg>')
    return '\n'.join(out) + '\n'


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    zoom, base, quopri = (float(a) for a in sys.argv[1:])
    rows = [
        ('zoomascii.b2a_qp', zoom, True),
        ('binascii.b2a_qp', base, False),
        ('quopri (pure Python)', quopri, False),
    ]
    when = datetime.date.today().strftime('%B %Y')
    subtitle = ('bin/bench.py on the 472 KB corpus in data/ · Python %d.%d, one core · %s'
                % (sys.version_info[0], sys.version_info[1], when))
    out_dir = os.path.join(os.path.dirname(__file__), '..', 'docs', 'benchmarks')
    for theme, fname in (('light', 'b2a_qp.svg'), ('dark', 'b2a_qp-dark.svg')):
        path = os.path.join(out_dir, fname)
        with open(path, 'w') as fh:
            fh.write(render(theme, rows, subtitle))
        print('wrote', os.path.normpath(path))


if __name__ == '__main__':
    main()
