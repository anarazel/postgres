#!/usr/bin/env python3
"""
Measure system-wide resource usage during a command (e.g. a Postgres test run).

Uses SYSTEM-WIDE cumulative counters (psutil.cpu_times, disk_io_counters) for
both the running time series and the absolute totals, so short-lived children
spawned by a test runner are not missed. Caveat: concurrent activity from
unrelated processes is also included, so run on an otherwise-idle machine.
A single figure with CPU/memory/IO subplots is saved.
"""
import argparse
import csv
import subprocess
import time
import sys
import psutil

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    HAVE_PLT = True
except ImportError:
    HAVE_PLT = False


def read_dirty_bytes():
    """Linux: dirty kernel page cache from /proc/meminfo, in bytes. Else None."""
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("Dirty:"):
                    return int(line.split()[1]) * 1024  # kB -> bytes
    except (OSError, ValueError, IndexError):
        pass
    return None


def fmt_span(label, start, peak, end, scale, unit):
    return (f"{label:<12} start {start/scale:.2f} {unit}, "
            f"peak {peak/scale:.2f} {unit} (delta {(peak-start)/scale:+.2f} {unit}), "
            f"end {end/scale:.2f} {unit}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interval", type=float, default=1.0, help="sample interval (s)")
    ap.add_argument("--prefix", default="resmon", help="output file prefix")
    ap.add_argument("command", nargs=argparse.REMAINDER, help="command to run")
    args = ap.parse_args()
    if not args.command:
        ap.error("no command given")
    print(f"measuring command {args.command}", file=sys.stderr)
    have_dirty = read_dirty_bytes() is not None

    proc = subprocess.Popen(args.command)

    # System-wide baselines (cumulative counters -> take deltas at the end).
    cpu0 = psutil.cpu_times()
    disk0 = psutil.disk_io_counters()
    vm0 = psutil.virtual_memory()
    have_cached = hasattr(vm0, "cached")

    rows = []
    # start / peak / end trackers for the "context" metrics
    start_used = vm0.used
    peak_used = vm0.used
    start_avail = vm0.available
    peak_avail = vm0.available  # track max; we also want min, keep both
    min_avail = vm0.available
    d0 = read_dirty_bytes() if have_dirty else None
    start_dirty = d0 if have_dirty else 0
    peak_dirty = start_dirty
    last_used = start_used
    last_avail = start_avail
    last_dirty = start_dirty

    start = time.time()
    psutil.cpu_times_percent(interval=None)  # prime

    while True:
        try:
            proc.wait(timeout=args.interval)
            break  # process exited
        except subprocess.TimeoutExpired:
            pass  # still running -> sample

        t = time.time() - start
        cp = psutil.cpu_times_percent(interval=None)
        vm = psutil.virtual_memory()
        d = psutil.disk_io_counters()
        dirty = read_dirty_bytes() if have_dirty else -1

        peak_used = max(peak_used, vm.used)
        peak_avail = max(peak_avail, vm.available)
        min_avail = min(min_avail, vm.available)
        last_used, last_avail = vm.used, vm.available
        if have_dirty:
            peak_dirty = max(peak_dirty, dirty)
            last_dirty = dirty

        row = {
            "t": round(t, 3),
            "cpu_user_pct": cp.user,
            "cpu_sys_pct": cp.system,
            "mem_total": vm.total,
            "mem_used": vm.used,
            "mem_available": vm.available,
            "mem_cached": vm.cached if have_cached else -1,
            "mem_dirty": dirty,
            "read_bytes": d.read_bytes if d else -1,
            "write_bytes": d.write_bytes if d else -1,
            "cpu_idle_pct": cp.idle,
            "cpu_iowait_pct": getattr(cp, "iowait", -1),
        }
        rows.append(row)

    wall = time.time() - start
    cpu1 = psutil.cpu_times()
    disk1 = psutil.disk_io_counters()

    # ---- absolute system-wide totals over the run window ----
    sys_user = cpu1.user - cpu0.user
    sys_system = cpu1.system - cpu0.system
    sys_read = (disk1.read_bytes - disk0.read_bytes) if disk0 and disk1 else -1
    sys_write = (disk1.write_bytes - disk0.write_bytes) if disk0 and disk1 else -1

    # ---- write CSV ----
    csv_path = f"{args.prefix}.csv"
    if rows:
        with open(csv_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)

    # ---- summary (all system-wide) ----
    print(f"wall:               {wall:.3f} s")
    print(f"cpu user:           {sys_user:.3f} s")
    print(f"cpu system:         {sys_system:.3f} s")
    print(f"mem total:          {vm0.total/1e9:.2f} GB")
    print(fmt_span("mem used:", start_used, peak_used, last_used, 1e9, "GB"))
    # available: the meaningful peak is the *minimum* (lowest headroom)
    print(f"mem available: start {start_avail/1e9:.2f} GB, "
          f"min {min_avail/1e9:.2f} GB (delta {(min_avail-start_avail)/1e9:+.2f} GB), "
          f"end {last_avail/1e9:.2f} GB")
    if have_dirty:
        print(fmt_span("mem dirty:", start_dirty, peak_dirty, last_dirty, 1e6, "MB"))
    if sys_read >= 0:
        print(f"disk read:          {sys_read/1e6:.1f} MB")
        print(f"disk write:         {sys_write/1e6:.1f} MB")
    print(f"csv:                {csv_path}")

    # ---- plot (single figure, stacked subplots) ----
    if HAVE_PLT and rows:
        have_io = any(r["read_bytes"] >= 0 for r in rows)
        have_iowait = any(r["cpu_iowait_pct"] >= 0 for r in rows)
        n = 3 if have_io else 2
        ts_ = [r["t"] for r in rows]
        fig, axes = plt.subplots(n, 1, figsize=(9, 3 * n), sharex=True)

        axes[0].plot(ts_, [r["cpu_user_pct"] for r in rows], label="user")
        axes[0].plot(ts_, [r["cpu_sys_pct"] for r in rows], label="system")
        axes[0].plot(ts_, [r["cpu_idle_pct"] for r in rows], label="idle")
        if have_iowait:
            axes[0].plot(ts_, [r["cpu_iowait_pct"] for r in rows], label="iowait")
        axes[0].set_ylabel("CPU %"); axes[0].set_title("CPU usage"); axes[0].legend()

        g = 1e9
        ax_mem = axes[1]
        ax_mem.plot(ts_, [r["mem_used"] / g for r in rows], label="used")
        ax_mem.plot(ts_, [r["mem_available"] / g for r in rows], label="available")
        if have_cached:
            ax_mem.plot(ts_, [r["mem_cached"] / g for r in rows], label="cached")
        ax_mem.set_ylabel("GB")
        ax_mem.set_title(f"Memory (total {rows[0]['mem_total'] / g:.1f} GB)")

        if have_dirty:
            ax_dirty = ax_mem.twinx()
            ax_dirty.plot(ts_, [r["mem_dirty"] / 1e6 for r in rows],
                          color="tab:red", linestyle="--", label="dirty")
            ax_dirty.set_ylabel("dirty (MB)", color="tab:red")
            ax_dirty.tick_params(axis="y", labelcolor="tab:red")
            h1, l1 = ax_mem.get_legend_handles_labels()
            h2, l2 = ax_dirty.get_legend_handles_labels()
            ax_mem.legend(h1 + h2, l1 + l2, loc="upper left")
        else:
            ax_mem.legend(loc="upper left")

        if have_io:
            # per-interval rate (MB/s) so small bursts stay visible
            rt, rrd, rwr = [], [], []
            for prev, cur in zip(rows, rows[1:]):
                dt = cur["t"] - prev["t"]
                if dt <= 0:
                    continue
                rt.append(cur["t"])
                rrd.append((cur["read_bytes"] - prev["read_bytes"]) / 1e6 / dt)
                rwr.append((cur["write_bytes"] - prev["write_bytes"]) / 1e6 / dt)
            ax_io = axes[2]
            ax_io.plot(rt, rrd, label="read rate")
            ax_io.plot(rt, rwr, label="write rate")
            ax_io.set_ylabel("MB/s"); ax_io.set_title("Disk IO")

            # cumulative totals on a secondary axis
            base_r, base_w = rows[0]["read_bytes"], rows[0]["write_bytes"]
            ax_cum = ax_io.twinx()
            ax_cum.plot(ts_, [(r["read_bytes"] - base_r) / 1e6 for r in rows],
                        color="tab:green", linestyle="--", label="read total")
            ax_cum.plot(ts_, [(r["write_bytes"] - base_w) / 1e6 for r in rows],
                        color="tab:purple", linestyle="--", label="write total")
            ax_cum.set_ylabel("cumulative MB")

            h1, l1 = ax_io.get_legend_handles_labels()
            h2, l2 = ax_cum.get_legend_handles_labels()
            ax_io.legend(h1 + h2, l1 + l2, loc="upper left")

        axes[-1].set_xlabel("time (s)")
        fig.tight_layout()
        png = f"{args.prefix}.png"
        fig.savefig(png, dpi=120)
        plt.close(fig)
        print(f"plot:               {png}")
    elif not HAVE_PLT:
        print("plot:               skipped (pip install matplotlib)")

if __name__ == "__main__":
    main()
