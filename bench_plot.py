#!/usr/bin/env python3
"""Draws bench_results.svg from the CSV files that ./bench writes.

    ./bench -t 16 -s 2 -w 10 > bench_10pct.csv
    ./bench -t 16 -s 2 -w 1  > bench_1pct.csv
    python3 bench_plot.py
"""

import csv

PANELS = [("bench_10pct.csv", "10% learns, 90% lookups"),
          ("bench_1pct.csv", "1% learns, 99% lookups")]
MODES = [("global", "one global lock", "#c0392b"),
         ("striped", "striped locks", "#1f6fb5"),
         ("rwlock", "read-write lock", "#7f8c8d")]

W, H = 780, 330
PW, PH = 300, 200          # plot area of one panel
LEFT = [70, 450]
TOP = 70


def load(path):
    data = {}
    with open(path) as f:
        for row in csv.DictReader(f):
            data.setdefault(row["mode"], {})[int(row["threads"])] = float(row["ops_per_sec"]) / 1e6
    return data


def main():
    out = []
    out.append('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" '
               'font-family="Helvetica, Arial, sans-serif" font-size="12">' % (W, H))
    out.append('<rect width="%d" height="%d" fill="#ffffff"/>' % (W, H))
    out.append('<text x="%d" y="24" font-size="15" font-weight="bold" fill="#222">'
               'MAC table throughput, 1 to 16 threads</text>' % LEFT[0])

    # legend
    x = LEFT[0]
    for _, label, colour in MODES:
        out.append('<line x1="%d" y1="40" x2="%d" y2="40" stroke="%s" stroke-width="3"/>' % (x, x + 22, colour))
        out.append('<text x="%d" y="44" fill="#222">%s</text>' % (x + 28, label))
        x += 150

    for (path, title), left in zip(PANELS, LEFT):
        data = load(path)
        threads = sorted(next(iter(data.values())).keys())
        top = max(v for series in data.values() for v in series.values())
        ymax = 6.0 if top <= 6.0 else (int(top) + 1)

        out.append('<text x="%d" y="%d" fill="#222" font-weight="bold">%s</text>' % (left, TOP - 8, title))
        # grid and y axis
        for i in range(0, int(ymax) + 1):
            y = TOP + PH - PH * i / ymax
            out.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#e3e3e3"/>' % (left, y, left + PW, y))
            out.append('<text x="%d" y="%.1f" text-anchor="end" fill="#555">%d</text>' % (left - 6, y + 4, i))
        out.append('<text x="%d" y="%d" text-anchor="middle" fill="#555" transform="rotate(-90 %d %d)">'
                   'million operations per second</text>' % (left - 40, TOP + PH // 2, left - 40, TOP + PH // 2))

        def px(n):
            return left + PW * threads.index(n) / (len(threads) - 1)

        for n in threads:
            out.append('<text x="%.1f" y="%d" text-anchor="middle" fill="#555">%d</text>' % (px(n), TOP + PH + 18, n))
        out.append('<text x="%d" y="%d" text-anchor="middle" fill="#555">threads</text>' % (left + PW // 2, TOP + PH + 36))
        out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#888"/>' % (left, TOP + PH, left + PW, TOP + PH))

        for mode, _, colour in MODES:
            pts = [(px(n), TOP + PH - PH * data[mode][n] / ymax) for n in threads]
            out.append('<polyline fill="none" stroke="%s" stroke-width="2.5" points="%s"/>' %
                       (colour, " ".join("%.1f,%.1f" % p for p in pts)))
            for p in pts:
                out.append('<circle cx="%.1f" cy="%.1f" r="3.5" fill="%s"/>' % (p[0], p[1], colour))

    out.append('</svg>')
    with open("bench_results.svg", "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote bench_results.svg")


main()
