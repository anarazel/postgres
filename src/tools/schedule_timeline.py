#!/usr/bin/env python3

"""Draw the timeline of a pg_regress schedule run.

pg_regress and pg_isolation_regress write a run log, schedule.jsonl, into
their output directory: one JSON object per line describing the run and
every test in it (see write_run_log() in src/test/regress/pg_regress.c).
This script turns such a log into a graphviz timeline, with one box per
test placed by the time it ran, an arrow from each test to the test whose
completion let it start, the critical path in red, and a strip at the
left showing what kept the pending tests waiting at any moment:

    src/tools/schedule_timeline.py --svg build/testrun/regress/regress

A directory argument stands for the schedule*.jsonl files in it.  Each run
log becomes <name>.dot next to it, or the file named by -o.  All boxes have
their position already, so there is no layout left for graphviz to do:
--svg renders the dot file with "neato -n2".
"""

import argparse
import bisect
import json
import os
import shutil
import subprocess
import sys

# Layout constants, in points; the positions in the graph are absolute.
YSCALE = 30.0  # points per second before stretching
MIN_HEIGHT = 24.0  # a box is at least two lines of text tall
CHAR_WIDTH = 3.8  # width of a character at font size 7
MIN_WIDTH = 36.0
LANE_GAP = 6.0
STRIP_WIDTH = 12.0  # the bottleneck strip at the left

# How the arrow to the test we waited for is drawn, per reason.  "slot" and
# "budget" mean that we waited for capacity rather than for that particular
# test, so those arrows are only drawn along the critical path.
EDGE_STYLE = {
    "after": "solid",
    "after *": "solid, color=gray",
    "notwith": "dashed",
    "notwith *": "dashed, color=gray",
    "resource": "dotted",
    "budget": "solid, color=skyblue",
    "slot": "solid, color=lightgray",
}
WEAK_REASONS = ("slot", "budget")

# What kept the tests that had not started yet from starting, and the color
# of the strip while it did.
LIMIT_TEXT = {
    "none": "nothing left to start",
    "cap": "concurrency limit reached",
    "connections": "connection budget exhausted",
    "workers": "parallel worker budget exhausted",
    "budget": "connection budget exhausted",
    "constraints": "all of them blocked by constraints",
}
LIMIT_COLOR = {
    "cap": "gray75",
    "connections": "skyblue",
    "workers": "palegreen",
    "budget": "skyblue",
    "constraints": "orange",
}


def load_run_log(path):
    """Read a run log; returns the run and its tests, in schedule order."""
    run = None
    tests = []
    states = []
    with open(path, "r") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            if rec.get("type") == "run":
                run = rec
            elif rec.get("type") == "test":
                tests.append(rec)
            elif rec.get("type") == "state":
                states.append(rec)
            else:
                sys.exit("%s:%d: unexpected record" % (path, lineno))
    if run is None:
        sys.exit("%s: no run record" % path)
    run["path"] = path
    run["states"] = states
    return run, tests


def directives(test):
    """The directives of a test, for its tooltip."""
    out = []
    if test["after_all"]:
        out.append("after: *")
    if test["after"]:
        out.append("after: " + " ".join(test["after"]))
    if test["before_all"]:
        out.append("before: *")
    if test["notwith_all"]:
        out.append("notwith: *")
    if test["notwith"]:
        out.append("notwith: " + " ".join(test["notwith"]))
    if test["shared"]:
        out.append("shared: " + " ".join(test["shared"]))
    if test["exclusive"]:
        out.append("exclusive: " + " ".join(test["exclusive"]))
    if test["conns"] != 1:
        out.append("%d connections" % test["conns"])
    if not out:
        return "no constraints"
    return " ".join(out)


def split_label(test):
    """The second line of a box: how long the test took."""
    return "%.0f ms" % test["duration_ms"]


def quote(text):
    """Escape a string for a graphviz attribute."""
    return text.replace("\\", "\\\\").replace('"', '\\"')


def layout(tests, labels, strip):
    """Place the boxes: a column per concurrent test, time downwards.

    The time axis is a monotone compression of the clock, so that the order
    of all starts and ends and which tests overlapped stay exact while every
    box is tall enough to read.  Returns (the y position of a time, the lane
    of each test, the x and width of each lane); with strip, the lanes leave
    room for the bottleneck strip at the left.
    """
    times = sorted([t["start"] for t in tests] + [t["end"] for t in tests])
    ys = [0.0] * len(times)
    ends = {}
    for i, test in enumerate(tests):
        ends.setdefault(test["end"], []).append(i)

    def pos_upto(value, upto):
        return ys[bisect.bisect_left(times, value, 0, upto)]

    for k in range(1, len(times)):
        ys[k] = ys[k - 1] + (times[k] - times[k - 1]) * YSCALE
        for i in ends.get(times[k], ()):
            top = pos_upto(tests[i]["start"], k)
            ys[k] = max(ys[k], top + MIN_HEIGHT)

    # each test gets the lowest lane that was free when it started
    lane = [0] * len(tests)
    lane_free = []
    for i in sorted(range(len(tests)), key=lambda i: tests[i]["start"]):
        for l, free in enumerate(lane_free):
            if free <= tests[i]["start"]:
                break
        else:
            l = len(lane_free)
            lane_free.append(0.0)
        lane_free[l] = tests[i]["end"]
        lane[i] = l

    # a column is as wide as the widest text in it
    lane_width = [MIN_WIDTH] * len(lane_free)
    for i, test in enumerate(tests):
        chars = max(len(test["name"]), len(labels[i]))
        lane_width[lane[i]] = max(lane_width[lane[i]], chars * CHAR_WIDTH + 8.0)
    lane_x = []
    left = STRIP_WIDTH + LANE_GAP if strip else 0.0
    for l, width in enumerate(lane_width):
        if l == 0:
            lane_x.append(left + width / 2)
        else:
            lane_x.append(
                lane_x[l - 1] + lane_width[l - 1] / 2 + LANE_GAP + width / 2
            )

    def pos(value):
        # a state can be recorded a little after the last event
        return ys[min(bisect.bisect_left(times, value), len(ys) - 1)]

    return pos, lane, lane_x, lane_width


def critical_path(tests):
    """Which tests are on the critical path: walk back from the last one."""
    byname = {t["name"]: i for i, t in enumerate(tests)}
    onpath = [False] * len(tests)
    i = max(range(len(tests)), key=lambda i: tests[i]["end"])
    while i is not None and not onpath[i]:
        onpath[i] = True
        pred = tests[i]["pred"]
        i = byname.get(pred) if pred else None
    return onpath


def caption_of(run, tests):
    """The lines above the picture: what it is and how to read it."""
    last = max(range(len(tests)), key=lambda i: tests[i]["end"])
    caption = [
        "%s, %s, %d tests, total %.0f ms, started %s."
        % (
            run["driver"],
            os.path.basename(run["schedule"]),
            len(tests),
            1000.0 * (tests[last]["end"] - run["start"]),
            run["start_time"],
        ),
        "Time runs downwards: order and overlap are exact, distances are "
        "not; concurrent tests sit side by side.",
        "An arrow points from a test to the test it waited for: solid = "
        "after, dashed = notwith, dotted = shared resource,",
        "gray = the '*' forms, light gray = only a free slot, blue = the "
        "connection budget.  Red = critical path.",
    ]
    if run["states"]:
        caption.append(
            "The strip at the left shows what kept the pending tests waiting: "
            "gray = the concurrency limit, blue = the connection budget, green "
            "= the worker budget, orange = a constraint (hover for which)."
        )
    caption.append("Hover for durations, directives, and what was pending.")
    return caption


def box_of(run, tests, i):
    """The tooltip of a test's box."""
    test = tests[i]
    tooltip = "%s: started at %.2f s, %.0f ms" % (
        test["name"],
        test["start"] - run["start"],
        test["duration_ms"],
    )
    tooltip += "; %d of %d tests still pending then, %s; %s" % (
        test["pending_at_start"],
        len(tests),
        LIMIT_TEXT.get(test["limit"], test["limit"]),
        directives(test),
    )
    if test["status"] != "ok":
        tooltip += "; FAILED"
    return tooltip


def strip_intervals(run, tests):
    """The bottleneck strip: (start, end, state) for each stretch of time
    during which the same thing kept the same pending test waiting."""
    states = run["states"]
    if not states:
        return []
    end = max(t["end"] for t in tests)
    out = []
    for st in states:
        key = (st["limit"], st["test"], st["blocker"], st["reason"])
        if out and out[-1][2] == key:
            continue
        if out:
            out[-1][1] = st["t"]
        out.append([st["t"], end, key])
    return [(a, b, key) for a, b, key in out if key[0] != "none"]


def strip_tooltip(run, start, end, key):
    limit, test, blocker, reason = key
    text = "%.2f-%.2f s: %s" % (
        start - run["start"],
        end - run["start"],
        LIMIT_TEXT.get(limit, limit),
    )
    if blocker:
        text += "; %s waits for %s (%s)" % (test, blocker, reason)
    elif test:
        text += "; %s is next" % test
    return text


def edge_of(tests, i, onpath):
    """The arrow from test i, as (predecessor, reason, on the critical path).

    None if there is nothing to draw: the first test of the run has no
    predecessor, and one that only waited for capacity gets an arrow only
    where it explains the critical path.
    """
    byname = {t["name"]: j for j, t in enumerate(tests)}
    pred = tests[i]["pred"]
    if not pred or pred not in byname:
        return None
    p = byname[pred]
    red = onpath[i] and onpath[p]
    reason = tests[i]["pred_reason"]
    if reason in WEAK_REASONS and not red:
        return None
    return p, reason, red


def write_dot(run, tests, out):
    """Write the timeline of one run log as graphviz input."""
    labels = [split_label(t) for t in tests]
    strip = strip_intervals(run, tests)
    pos, lane, lane_x, lane_width = layout(tests, labels, bool(strip))
    onpath = critical_path(tests)

    out.write("// timeline of a %s run; render with: neato -n2 -Tsvg\n"
              % run["driver"])
    out.write("digraph schedule {\n")
    out.write("\tnode [shape=box, fontsize=7, fixedsize=shape];\n")
    out.write("\tedge [arrowsize=0.6];\n")
    out.write("\tlabelloc=t;\n\tlabeljust=l;\n")
    out.write('\tlabel="%s\\l";\n'
              % "\\l".join(quote(c)
                           for c in caption_of(run, tests)))

    for i, test in enumerate(tests):
        tooltip = box_of(run, tests, i)
        top = pos(test["start"])
        bottom = pos(test["end"])
        out.write(
            '\t"%s" [label="%s\\n%s", tooltip="%s", pos="%.1f,%.1f!", '
            "width=%.3f, height=%.3f%s];\n"
            % (
                quote(test["name"]),
                quote(test["name"]),
                quote(labels[i]),
                quote(tooltip),
                lane_x[lane[i]],
                -(top + bottom) / 2,
                lane_width[lane[i]] / 72.0,
                (bottom - top) / 72.0,
                ", color=red, penwidth=2" if onpath[i] else "",
            )
        )

        edge = edge_of(tests, i, onpath)
        if edge is None:
            continue
        p, reason, red = edge

        # Time runs downwards: the arrow ends at the bottom of the box it
        # waited for, the moment that test finished.  It starts at the centre
        # of its own box, so that it stays visible when the two boxes touch.
        out.write(
            '\t"%s" -> "%s" [headport=s, style=%s, '
            'tooltip="%s waited for %s (%s)"%s];\n'
            % (
                quote(test["name"]),
                quote(tests[p]["name"]),
                EDGE_STYLE.get(reason, "solid"),
                quote(test["name"]),
                quote(tests[p]["name"]),
                quote(reason),
                ", color=red, penwidth=2" if red else "",
            )
        )
    for n, (start, end, key) in enumerate(strip):
        top = pos(start)
        bottom = pos(end)
        if bottom - top < 1.0:
            continue
        out.write(
            '\tstrip%d [label="", tooltip="%s", pos="%.1f,%.1f!", '
            "width=%.3f, height=%.3f, style=filled, fillcolor=%s, "
            "color=none];\n"
            % (
                n,
                quote(strip_tooltip(run, start, end, key)),
                STRIP_WIDTH / 2,
                -(top + bottom) / 2,
                STRIP_WIDTH / 72.0,
                (bottom - top) / 72.0,
                LIMIT_COLOR.get(key[0], "white"),
            )
        )
    out.write("}\n")


def render_with_neato(dotpath):
    """Turn a dot file into an svg with neato, if it is around."""
    neato = shutil.which("neato")
    if neato is None:
        print("neato not found, not rendering %s" % dotpath, file=sys.stderr)
        return
    svgpath = os.path.splitext(dotpath)[0] + ".svg"
    subprocess.run([neato, "-n2", "-Tsvg", "-o", svgpath, dotpath], check=True)
    print("wrote %s" % svgpath, file=sys.stderr)


def run_logs_of(path):
    """The run logs an argument names: a file, or those in a directory."""
    if os.path.isdir(path):
        logs = sorted(
            os.path.join(path, name)
            for name in os.listdir(path)
            if name.startswith("schedule") and name.endswith(".jsonl")
        )
        if not logs:
            sys.exit("no schedule*.jsonl in \"%s\"" % path)
        return logs
    return [path]


def main():
    parser = argparse.ArgumentParser(
        description="Draw the timeline of a pg_regress schedule run.",
        epilog="A directory argument stands for the schedule*.jsonl in it.",
    )
    parser.add_argument("runlog", nargs="+",
                        help="run log written by pg_regress, or a directory")
    parser.add_argument("-o", "--output",
                        help="write the graph here, \"-\" for stdout "
                        "(only with a single run log)")
    parser.add_argument("--svg", action="store_true",
                        help="also render the dot file with \"neato -n2\"")
    args = parser.parse_args()

    paths = [log for arg in args.runlog for log in run_logs_of(arg)]
    if args.output and len(paths) > 1:
        sys.exit("--output does not work with more than one run log")

    runs = [load_run_log(path) for path in paths]

    for run, tests in runs:
        if not tests:
            print("%s: no tests" % run["path"], file=sys.stderr)
            continue
        if args.output == "-":
            write_dot(run, tests, sys.stdout)
            continue
        dotpath = args.output or os.path.splitext(run["path"])[0] + ".dot"
        with open(dotpath, "w") as f:
            write_dot(run, tests, f)
        print("wrote %s" % dotpath, file=sys.stderr)
        if args.svg:
            render_with_neato(dotpath)


if __name__ == "__main__":
    main()
