#!/usr/bin/env python3
"""Summarize hypr-kinetic-scroll metrics files side by side.

Usage: analyze.py before.jsonl [after.jsonl ...] [--all]

Each input is the file written by plugin:kinetic-scroll:metrics_file: one JSON
object per gesture. Cost metrics are nanoseconds spent inside the plugin on
Hyprland's main thread. Behaviour metrics describe the fling itself.

Aggregation: per file, over launched gestures, report median and p95 (p95 of
per-gesture maxima for the *_max rows). Lower is better for every cost and
latency row; render_dt stddev/max is cadence jitter (lower is better);
timer_share is the fraction of momentum frames that came from the watchdog
instead of the render hook (should be ~0).
"""

import json
import statistics
import sys


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"{path}: skipping bad line: {e}", file=sys.stderr)
    return rows


def pct(values, p):
    if not values:
        return float("nan")
    s = sorted(values)
    k = (len(s) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def med(values):
    return statistics.median(values) if values else float("nan")


ROWS = [
    # (label, unit, extractor over a launched gesture, aggregator label)
    ("axis cost / event", "us", lambda g: g["axis_ns"]["mean"] / 1e3),
    ("axis cost max", "us", lambda g: g["axis_ns"]["max"] / 1e3),
    ("step cost / frame", "us", lambda g: g["step_ns"]["mean"] / 1e3),
    ("step cost max", "us", lambda g: g["step_ns"]["max"] / 1e3),
    ("lift -> launch", "ms", lambda g: g["lift_to_launch_ms"]),
    ("lift -> first emit", "ms", lambda g: g["lift_to_first_emit_ms"]),
    ("render dt mean", "ms", lambda g: g["render_dt_ms"]["mean"]),
    ("render dt stddev", "ms", lambda g: g["render_dt_ms"]["stddev"]),
    ("render dt max", "ms", lambda g: g["render_dt_ms"]["max"]),
    ("timer share", "%", lambda g: 100.0 * g["steps_timer"] / max(1, g["steps_timer"] + g["steps_render"])),
    ("fling duration", "ms", lambda g: g["fling_ms"]),
    ("launch |v|", "u/ms", lambda g: abs(g["launch_v"])),
    ("travel |v|", "u", lambda g: g["travel_v"]),
    ("lift tail", "ms", lambda g: g["tail_ms"]),
    ("samples in window", "n", lambda g: len(g["samples"])),
]


def summarize(path, rows):
    launched = [g for g in rows if g.get("launched")]
    out = {"file": path, "gestures": len(rows), "launched": len(launched)}
    for label, unit, fn in ROWS:
        vals = []
        for g in launched:
            try:
                v = fn(g)
            except (KeyError, TypeError, ZeroDivisionError):
                continue
            if v is not None and v == v and v >= 0:
                vals.append(v)
        out[label] = (med(vals), pct(vals, 0.95), unit)
    if rows:
        last = rows[-1]["global"]
        idle = last["idle_axis_ns"]
        out["idle axis cost / event"] = (idle["mean"] / 1e3, idle["max"] / 1e3, "us")
        out["idle axis events"] = idle["n"]
    stops = {}
    for g in rows:
        stops[g.get("stop", "?")] = stops.get(g.get("stop", "?"), 0) + 1
    out["stops"] = stops
    return out


def main(argv):
    paths = [a for a in argv[1:] if not a.startswith("--")]
    if not paths:
        print(__doc__)
        return 2
    sums = [summarize(p, load(p)) for p in paths]

    w = 22
    print(f"{'metric':<{w}}" + "".join(f"{s['file'][-28:]:>30}" for s in sums))
    print(f"{'gestures / launched':<{w}}" + "".join(f"{s['gestures']:>20}/{s['launched']:<9}" for s in sums))
    print(f"{'':<{w}}" + "".join(f"{'median':>15}{'p95':>15}" for _ in sums))
    for label, unit, _ in ROWS:
        line = f"{label + ' [' + unit + ']':<{w}}"
        for s in sums:
            m, p, _ = s[label]
            line += f"{m:>15.3f}{p:>15.3f}"
        print(line)
    line = f"{'idle axis [us] mean/max':<{w}}"
    for s in sums:
        if "idle axis cost / event" in s:
            m, p, _ = s["idle axis cost / event"]
            line += f"{m:>15.3f}{p:>15.3f}"
        else:
            line += f"{'-':>15}{'-':>15}"
    print(line)
    print()
    for s in sums:
        print(f"{s['file']}: stop reasons {s['stops']}, idle axis events {s.get('idle axis events', 0)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
