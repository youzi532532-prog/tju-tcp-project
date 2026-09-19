#!/usr/bin/env python3
"""Render Reno trace charts as SVG using only the Python standard library."""

import argparse
import csv
import html
from pathlib import Path


WIDTH = 1200
HEIGHT = 760
LEFT = 85
RIGHT = 30
TOP = 45
PANEL_HEIGHT = 280
PANEL_GAP = 90
COLORS = {
    "cwnd": "#1769aa",
    "ssthresh": "#c62828",
    "rwnd": "#2e7d32",
    "flight_size": "#6a1b9a",
}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    return parser.parse_args()


def load_rows(path):
    rows = []
    with path.open(newline="", encoding="ascii") as handle:
        for row in csv.DictReader(handle):
            rows.append(
                {
                    "timestamp_us": int(row["timestamp_us"]),
                    "event": row["event"],
                    "cwnd": int(row["cwnd"]),
                    "ssthresh": int(row["ssthresh"]),
                    "rwnd": int(row["rwnd"]),
                    "flight_size": int(row["flight_size"]),
                }
            )
    if not rows:
        raise ValueError("trace has no data rows")
    return rows


def points(rows, field, x_scale, y_scale, top):
    return " ".join(
        f"{LEFT + (row['timestamp_us'] - rows[0]['timestamp_us']) * x_scale:.2f},"
        f"{top + PANEL_HEIGHT - row[field] * y_scale:.2f}"
        for row in rows
    )


def panel(svg, rows, top, fields, title, duration_us):
    maximum = max(max(row[field] for field in fields) for row in rows)
    maximum = max(maximum, 1)
    plot_width = WIDTH - LEFT - RIGHT
    x_scale = plot_width / max(duration_us, 1)
    y_scale = PANEL_HEIGHT / maximum

    svg.append(
        f'<text x="{LEFT}" y="{top - 15}" class="title">{html.escape(title)}</text>'
    )
    svg.append(
        f'<rect x="{LEFT}" y="{top}" width="{plot_width}" height="{PANEL_HEIGHT}" '
        'class="plot"/>'
    )
    for step in range(6):
        y = top + PANEL_HEIGHT - PANEL_HEIGHT * step / 5
        value = maximum * step / 5
        svg.append(f'<line x1="{LEFT}" y1="{y:.2f}" x2="{WIDTH-RIGHT}" y2="{y:.2f}" class="grid"/>')
        svg.append(f'<text x="{LEFT-10}" y="{y+4:.2f}" text-anchor="end" class="tick">{value/1024:.1f}</text>')
    for step in range(6):
        x = LEFT + plot_width * step / 5
        seconds = duration_us * step / 5 / 1_000_000
        svg.append(f'<text x="{x:.2f}" y="{top+PANEL_HEIGHT+24}" text-anchor="middle" class="tick">{seconds:.3f}</text>')

    for field in fields:
        svg.append(
            f'<polyline points="{points(rows, field, x_scale, y_scale, top)}" '
            f'stroke="{COLORS[field]}" class="series"/>'
        )

    for row in rows:
        if row["event"] not in {"RTO", "FAST_RETRANSMIT"}:
            continue
        x = LEFT + (row["timestamp_us"] - rows[0]["timestamp_us"]) * x_scale
        color = "#d32f2f" if row["event"] == "RTO" else "#ef6c00"
        svg.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top+PANEL_HEIGHT}" stroke="{color}" class="event"/>')
        svg.append(f'<text x="{x+4:.2f}" y="{top+14}" fill="{color}" class="event-label">{row["event"]}</text>')


def main():
    args = parse_args()
    rows = load_rows(args.trace)
    duration_us = rows[-1]["timestamp_us"] - rows[0]["timestamp_us"]
    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">',
        "<style>",
        "text{font-family:Arial,sans-serif;fill:#222}.title{font-size:18px;font-weight:600}.tick{font-size:11px}.plot{fill:#fff;stroke:#777}.grid{stroke:#ddd;stroke-width:1}.series{fill:none;stroke-width:2}.event{stroke-width:1.5;stroke-dasharray:5 4}.event-label{font-size:10px}.legend{font-size:13px}",
        "</style>",
        '<rect width="100%" height="100%" fill="#fafafa"/>',
    ]
    panel(svg, rows, TOP, ["cwnd", "ssthresh"], "cwnd / ssthresh (KiB)", duration_us)
    second_top = TOP + PANEL_HEIGHT + PANEL_GAP
    panel(svg, rows, second_top, ["cwnd", "rwnd", "flight_size"], "cwnd / rwnd / FlightSize (KiB)", duration_us)

    legend_y = HEIGHT - 25
    legend_x = LEFT
    for field in ["cwnd", "ssthresh", "rwnd", "flight_size"]:
        svg.append(f'<line x1="{legend_x}" y1="{legend_y}" x2="{legend_x+28}" y2="{legend_y}" stroke="{COLORS[field]}" class="series"/>')
        svg.append(f'<text x="{legend_x+35}" y="{legend_y+4}" class="legend">{field}</text>')
        legend_x += 175
    svg.append(f'<text x="{WIDTH/2}" y="{HEIGHT-55}" text-anchor="middle" class="tick">time since trace start (seconds)</text>')
    svg.append("</svg>")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(svg) + "\n", encoding="ascii")
    print(f"wrote {args.output} ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
