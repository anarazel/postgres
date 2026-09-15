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

Given the server's log, each test is also annotated with the CPU, lock
wait and I/O wait time of its backends and with the rest of its wall time,
which is time the server spent idle, waiting for the test's client.  The
log of a temp instance is found through the run log; for a run against an
existing server, pass --server-log.  That server needs

    log_disconnections = on
    log_line_prefix = '%m %b[%p] %q%a '
    track_io_timing = on

and a build that reports resource usage when a session disconnects and
when a parallel worker exits.  All sessions of a test are added up, so a
test that reconnects, and the several sessions of an isolation spec, need
nothing special; parallel workers are added to the test of their leader.
"""

import argparse
import bisect
import json
import os
import re
import shutil
import subprocess
import sys
import time

# Layout constants, in points; the positions in the graph are absolute.
YSCALE = 30.0  # points per second before stretching
MIN_HEIGHT = 24.0  # a box is at least two lines of text tall
CHAR_WIDTH = 3.8  # width of a character at font size 7
MIN_WIDTH = 36.0
LANE_GAP = 6.0
STRIP_WIDTH = 12.0  # the bottleneck strip at the left
CPU_WIDTH = 24.0  # the machine-wide CPU strip left of it

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

# log_line_prefix '%m %b[%p] %q%a ': timestamp, backend type, pid, and the
# application name, which only session processes have.
LOG_LINE = re.compile(
    r"^(?P<ts>\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+) \S+ "
    r"(?P<btype>[^\[]*)\[(?P<pid>\d+)\] "
    r"(?P<app>.*?)"
    r"(?:DEBUG[1-5]?|LOG|INFO|NOTICE|WARNING|ERROR|FATAL|PANIC|STATEMENT"
    r"|DETAIL|HINT|CONTEXT|QUERY|LOCATION):  "
    r"(?P<msg>.*)$"
)
RESOURCES = re.compile(
    r" cpu=(?P<user>[\d.]+)/(?P<system>[\d.]+)"
    r" lock_wait=(?P<lock>[\d.]+) io_wait=(?P<io>[\d.]+)"
)
LEADER = re.compile(r" leader=(?P<pid>\d+)")


def parse_timestamp(stamp):
    """Seconds since the epoch of a %m timestamp, read as local time."""
    whole, frac = stamp.split(".")
    return time.mktime(time.strptime(whole, "%Y-%m-%d %H:%M:%S")) + float(
        "0." + frac
    )


def test_of_app(app):
    """The test an application_name belongs to.

    "pg_regress/<test>" and "isolation/<spec>[/<session>]" both name the
    test in their second component.
    """
    parts = app.split("/")
    if len(parts) < 2 or not parts[1]:
        return None
    return parts[1]


class ServerLog:
    """The resource usage the server logged, per session and worker.

    Sessions report theirs when they disconnect, parallel workers when they
    exit; a worker only names its leader, so the leader's own lines are what
    tells us which test it belongs to.
    """

    def __init__(self, path):
        self.path = path
        self.sessions = []  # test sessions, in log order
        self.workers = []  # parallel worker exits, in log order
        self.apps = {}  # pid -> [(timestamp, application name)]
        self.unnamed = 0  # worker exits whose leader we never saw
        self._read()

    def _read(self):
        with open(self.path, "r", errors="replace") as f:
            for line in f:
                m = LOG_LINE.match(line)
                if m is None:
                    continue  # continuation line, or another prefix
                msg = m.group("msg")
                interesting = msg.startswith(
                    "disconnection:"
                ) or msg.startswith("parallel worker exit:")
                if not interesting and not m.group("app"):
                    continue

                ts = parse_timestamp(m.group("ts"))
                pid = int(m.group("pid"))
                app = m.group("app").strip()
                if app:
                    self.apps.setdefault(pid, []).append((ts, app))
                if not interesting:
                    continue

                res = RESOURCES.search(msg)
                if res is None:
                    continue  # a server without the resource reporting
                entry = {
                    "ts": ts,
                    "pid": pid,
                    "app": app,
                    "cpu": float(res.group("user")) + float(res.group("system")),
                    "lock": float(res.group("lock")),
                    "io": float(res.group("io")),
                }
                if msg.startswith("disconnection:"):
                    self.sessions.append(entry)
                else:
                    leader = LEADER.search(msg)
                    entry["leader"] = int(leader.group("pid")) if leader else None
                    self.workers.append(entry)

    def app_of_pid(self, pid, ts):
        """The application name a pid had around ts.

        Pids are reused, so take the name from the line closest in time.
        """
        seen = self.apps.get(pid)
        if not seen:
            return None
        return min(seen, key=lambda entry: abs(entry[0] - ts))[1]

    def entries(self):
        """Every entry, as (test name, kind, entry)."""
        for entry in self.sessions:
            name = test_of_app(entry["app"])
            yield name, "session", entry
        for entry in self.workers:
            app = entry["app"]
            if not app and entry["leader"] is not None:
                app = self.app_of_pid(entry["leader"], entry["ts"]) or ""
            name = test_of_app(app)
            if name is None:
                self.unnamed += 1
            yield name, "worker", entry


def load_cpu(path):
    """The machine's CPU usage over time, from a "vmstat -n -t 1" log.

    Returns [(start, end, busy, user, system, iowait)], busy being everything
    but idle, in percent.  A sample covers the interval ending at its
    timestamp, and the first one after the header averages over the time since
    boot, so it is dropped.  The samples describe the whole machine, not just
    this run: on a shared machine they overstate what the tests did.
    """
    samples = []
    cols = None
    need = 0
    for line in open(path, errors="replace"):
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "r" and "id" in fields:
            # the header's last token names the timezone, not a column
            cols = {name: i for i, name in enumerate(fields)}
            need = max(cols.get(n, -1) for n in ("id", "us", "sy", "wa")) + 1
            continue
        if cols is None or len(fields) < need + 2:
            continue  # the banner, or a log without timestamps
        try:
            ts = parse_timestamp(fields[-2] + " " + fields[-1] + ".0")
            busy = 100 - int(fields[cols["id"]])
            samples.append((ts, busy, int(fields[cols["us"]]),
                            int(fields[cols["sy"]]), int(fields[cols["wa"]])))
        except (ValueError, KeyError, IndexError):
            continue
    # the first sample is the average since boot; the rest each cover the
    # second before their timestamp
    return [(ts - 1.0, ts, busy, user, system, iowait)
            for ts, busy, user, system, iowait in samples[1:]]


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
                rec.setdefault("sessions", 0)
                rec.setdefault("workers", 0)
                for key in ("cpu", "lock", "io"):
                    rec.setdefault(key, 0.0)
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


def annotate(runs, log):
    """Add the resource usage the server logged to the tests of runs.

    runs is a list of (run, tests) sharing one server log.  A test name can
    occur in more than one of them, so a session goes to the run whose test
    was running when the session ended.
    """
    byname = {}
    for run, tests in runs:
        for test in tests:
            byname.setdefault(test["name"], []).append(test)

    attributed = 0
    for name, kind, entry in log.entries():
        if name is None:
            continue
        candidates = byname.get(name)
        if not candidates:
            continue
        test = candidates[0]
        if len(candidates) > 1:
            # prefer the one that was running, else the one that ran last
            # before this
            test = min(
                candidates,
                key=lambda t: (
                    not t["start"] <= entry["ts"] <= t["end"],
                    abs(entry["ts"] - t["end"]),
                ),
            )
        test[kind + "s"] += 1
        test["cpu"] += entry["cpu"]
        test["lock"] += entry["lock"]
        test["io"] += entry["io"]
        attributed += 1

    covered = sum(
        1 for _, tests in runs for t in tests if t["sessions"] or t["workers"]
    )
    total = sum(len(tests) for _, tests in runs)
    print(
        "%s: %d of %d session and worker records attributed, "
        "%d of %d tests covered"
        % (
            log.path,
            attributed,
            len(log.sessions) + len(log.workers),
            covered,
            total,
        ),
        file=sys.stderr,
    )
    if log.unnamed:
        print(
            "%s: %d worker exits could not be traced to a leader"
            % (log.path, log.unnamed),
            file=sys.stderr,
        )
    return attributed > 0


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


def split_label(test, have_stats):
    """The second line of a box: how the test spent its wall time."""
    wall = test["duration_ms"]
    label = "%.0f ms" % wall
    if not have_stats or not (test["sessions"] or test["workers"]):
        return label
    # shares of the wall time, so that the label stays about as wide as a
    # test name; the tooltip has the milliseconds
    parts = ["cpu %.0f%%" % (100.0 * 1000.0 * test["cpu"] / wall)]
    for key in ("lock", "io"):
        share = 100.0 * 1000.0 * test[key] / wall
        if share >= 5.0:
            parts.append("%s %.0f%%" % (key, share))
    return label + "  " + " ".join(parts)


def quote(text):
    """Escape a string for a graphviz attribute."""
    return text.replace("\\", "\\\\").replace('"', '\\"')


def layout(tests, labels, strip, cpu=False):
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
    left = (CPU_WIDTH + LANE_GAP if cpu else 0.0) + (
        STRIP_WIDTH + LANE_GAP if strip else 0.0)
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


def caption_of(run, tests, have_stats, have_cpu=False):
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
    if have_cpu:
        caption.append(
            "The bars at the far left are the whole machine's CPU usage per "
            "second, full width = every core busy (hover for the split)."
        )
    if have_stats:
        caption.append(
            "The box fill is the share of the wall time the test's backends "
            "spent on CPU: white = none, saturated blue = fully CPU bound."
        )
        caption.append(
            "Labels give the backends' CPU as a share of the wall time, above "
            "100% with parallel workers, and lock and IO wait shares of 5% or "
            "more; hover for the milliseconds and the idle rest."
        )
    caption.append("Hover for durations, directives, and what was pending.")
    return caption


def box_of(run, tests, i, label, have_stats):
    """The tooltip of a test's box, and how much of its time was CPU."""
    test = tests[i]
    tooltip = "%s: started at %.2f s, %.0f ms" % (
        test["name"],
        test["start"] - run["start"],
        test["duration_ms"],
    )
    ratio = None
    if have_stats and (test["sessions"] or test["workers"]):
        wall = test["duration_ms"]
        cpu = 1000.0 * test["cpu"]
        ratio = min(cpu / wall, 1.0) if wall > 0 else 0.0
        tooltip += (
            "; cpu %.0f ms (%.0f%%), lock wait %.0f ms, io wait %.0f ms, "
            "other %.0f ms, %d session%s"
            % (
                cpu,
                100.0 * ratio,
                1000.0 * test["lock"],
                1000.0 * test["io"],
                max(wall - cpu - 1000.0 * (test["lock"] + test["io"]), 0.0),
                test["sessions"],
                "" if test["sessions"] == 1 else "s",
            )
        )
        if test["workers"]:
            tooltip += " and %d parallel worker%s" % (
                test["workers"],
                "" if test["workers"] == 1 else "s",
            )
    tooltip += "; %d of %d tests still pending then, %s; %s" % (
        test["pending_at_start"],
        len(tests),
        LIMIT_TEXT.get(test["limit"], test["limit"]),
        directives(test),
    )
    if test["status"] != "ok":
        tooltip += "; FAILED"
    return {"tooltip": tooltip, "ratio": ratio}


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


def write_dot(run, tests, out, have_stats, cpu=()):
    """Write the timeline of one run log as graphviz input."""
    labels = [split_label(t, have_stats) for t in tests]
    strip = strip_intervals(run, tests)
    pos, lane, lane_x, lane_width = layout(tests, labels, bool(strip), bool(cpu))
    strip_x = CPU_WIDTH + LANE_GAP if cpu else 0.0
    onpath = critical_path(tests)

    out.write("// timeline of a %s run; render with: neato -n2 -Tsvg\n"
              % run["driver"])
    out.write("digraph schedule {\n")
    out.write("\tnode [shape=box, fontsize=7, fixedsize=shape];\n")
    out.write("\tedge [arrowsize=0.6];\n")
    out.write("\tlabelloc=t;\n\tlabeljust=l;\n")
    out.write('\tlabel="%s\\l";\n'
              % "\\l".join(quote(c)
                           for c in caption_of(run, tests, have_stats,
                                                    bool(cpu))))

    for i, test in enumerate(tests):
        box = box_of(run, tests, i, labels[i], have_stats)
        top = pos(test["start"])
        bottom = pos(test["end"])
        if box["ratio"] is not None:
            fill = ', style=filled, fillcolor="0.58 %.3f 1.000"' % (
                0.04 + 0.76 * box["ratio"]
            )
        elif have_stats:
            fill = ", style=filled, fillcolor=white"
        else:
            fill = ""
        out.write(
            '\t"%s" [label="%s\\n%s", tooltip="%s", pos="%.1f,%.1f!", '
            "width=%.3f, height=%.3f%s%s];\n"
            % (
                quote(test["name"]),
                quote(test["name"]),
                quote(labels[i]),
                quote(box["tooltip"]),
                lane_x[lane[i]],
                -(top + bottom) / 2,
                lane_width[lane[i]] / 72.0,
                (bottom - top) / 72.0,
                ", color=red, penwidth=2" if onpath[i] else "",
                fill,
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
                strip_x + STRIP_WIDTH / 2,
                -(top + bottom) / 2,
                STRIP_WIDTH / 72.0,
                (bottom - top) / 72.0,
                LIMIT_COLOR.get(key[0], "white"),
            )
        )
    first = min(t["start"] for t in tests)
    last = max(t["end"] for t in tests)
    for n, (start, end, busy, user, system, iowait) in enumerate(cpu):
        if end < first or start > last:
            continue                      # before or after the run
        top = pos(max(start, first))
        bottom = pos(min(end, last))
        if bottom - top < 1.0:
            continue
        width = max(CPU_WIDTH * busy / 100.0, 0.5)
        out.write(
            '\tcpu%d [label="", tooltip="%s", pos="%.1f,%.1f!", '
            "width=%.3f, height=%.3f, style=filled, fillcolor=%s, "
            "color=none];\n"
            % (
                n,
                quote("%d%% busy: %d%% user, %d%% system, %d%% iowait"
                      % (busy, user, system, iowait)),
                width / 2,
                -(top + bottom) / 2,
                width / 72.0,
                (bottom - top) / 72.0,
                "gray40" if busy >= 90 else "gray60",
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
    parser.add_argument("--server-log",
                        help="server log to take resource usage from, "
                        "instead of the one the run log names")
    parser.add_argument("--no-server-log", action="store_true",
                        help="do not annotate with resource usage")
    parser.add_argument("--vmstat",
                        help="\"vmstat -n -t 1\" log to take the machine's "
                        "CPU usage from (default: vmstat.log beside the run "
                        "log)")
    args = parser.parse_args()

    paths = [log for arg in args.runlog for log in run_logs_of(arg)]
    if args.output and len(paths) > 1:
        sys.exit("--output does not work with more than one run log")

    runs = [load_run_log(path) for path in paths]

    # runs sharing a server log have to be attributed together, as a test
    # name can occur in several of them
    have_stats = {}
    if not args.no_server_log:
        bylog = {}
        for run, tests in runs:
            logpath = args.server_log or run["server_log"]
            if logpath:
                bylog.setdefault(logpath, []).append((run, tests))
            else:
                print(
                    "%s: no server log recorded, not annotating; pass "
                    "--server-log for a run against an existing server"
                    % run["path"],
                    file=sys.stderr,
                )
        for logpath, group in bylog.items():
            try:
                log = ServerLog(logpath)
            except OSError as e:
                print("could not read %s: %s" % (logpath, e), file=sys.stderr)
                continue
            annotated = annotate(group, log)
            for run, _ in group:
                have_stats[run["path"]] = annotated

    for run, tests in runs:
        if not tests:
            print("%s: no tests" % run["path"], file=sys.stderr)
            continue
        stats = have_stats.get(run["path"], False)
        vmstat = args.vmstat or os.path.join(os.path.dirname(run["path"]),
                                             "vmstat.log")
        try:
            cpu = load_cpu(vmstat)
        except OSError:
            cpu = []
        if not cpu and args.vmstat:
            print("%s: no timestamped samples, is it \"vmstat -n -t 1\"?"
                  % vmstat, file=sys.stderr)
        if args.output == "-":
            # only one of the two can go to stdout
            write_dot(run, tests, sys.stdout, stats, cpu)
            continue
        dotpath = args.output or os.path.splitext(run["path"])[0] + ".dot"
        with open(dotpath, "w") as f:
            write_dot(run, tests, f, stats, cpu)
        print("wrote %s" % dotpath, file=sys.stderr)
        if args.svg:
            render_with_neato(dotpath)


if __name__ == "__main__":
    main()
