#!/usr/bin/env python3
"""
Compare benchmark results between runs to detect performance regressions.

Compares the two most recent benchmark result files for each mode,
or specific files via --new/--old, and produces a PASS/FAIL verdict.

The primary concern: never accept a change that worsens an existing
regression against master.

Usage:
    ./compare_benchmarks.py                    # compare latest vs previous
    ./compare_benchmarks.py --list             # list all available runs
    ./compare_benchmarks.py --new F --old F    # compare specific files
    ./compare_benchmarks.py --verbose          # show all queries
    ./compare_benchmarks.py --strict           # no noise tolerance
"""

import argparse
import json
import math
import os
import re
import sys
from datetime import datetime

# All 8 benchmark modes
ALL_MODES = [
    "benchmark_uncached",
    "benchmark_cached",
    "readstream_uncached",
    "readstream_cached",
    "random_backwards_uncached",
    "random_backwards_cached",
    "munro_uncached",
    "munro_cached",
]

# ANSI escape codes
BOLD = "\033[1m"
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
RESET = "\033[0m"

DEFAULT_THRESHOLD = 0.005
DEFAULT_ABS_TOLERANCE_MS = 2.0
REGRESSION_BOUNDARY = 1.01


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Compare benchmark results between runs.")
    parser.add_argument("--list", action="store_true",
                        help="List all available result files per mode")
    parser.add_argument("--new",
                        help="Path to the new result JSON file (for single-mode comparison)")
    parser.add_argument("--old",
                        help="Path to the old result JSON file (for single-mode comparison)")
    parser.add_argument("--baseline",
                        help="Patch git hash to use as the baseline (old) run for comparison")
    parser.add_argument("--verbose", action="store_true",
                        help="Show all queries, not just notable ones")
    parser.add_argument("--strict", action="store_true",
                        help="No noise tolerance (threshold=0)")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD,
                        help=f"Noise threshold for ratio changes (default: {DEFAULT_THRESHOLD})")
    parser.add_argument("--abs-tolerance", type=float, default=DEFAULT_ABS_TOLERANCE_MS,
                        help=f"Ignore changes where absolute ms difference is below this (default: {DEFAULT_ABS_TOLERANCE_MS}ms)")
    parser.add_argument("--results-dir", default="prefetch_results",
                        help="Directory containing result JSON files")
    return parser.parse_args()


def find_result_files(results_dir, mode):
    """Find all JSON result files for a given mode, sorted newest first."""
    if not os.path.exists(results_dir):
        return []

    prefix = f"{mode}_"
    files = []
    for f in os.listdir(results_dir):
        if f.startswith(prefix) and f.endswith(".json"):
            files.append(os.path.join(results_dir, f))

    # Sort by modification time, newest first
    files.sort(key=os.path.getmtime, reverse=True)
    return files


def find_result_by_patch_hash(results_dir, mode, patch_hash, exclude_path=None):
    """Find the most recent result file for a mode with a given patch_hash."""
    files = find_result_files(results_dir, mode)
    for path in files:
        if path == exclude_path:
            continue
        data = load_result(path)
        file_hash = data.get("patch_hash", "")
        # Support prefix matching (short hashes)
        if file_hash.startswith(patch_hash) or patch_hash.startswith(file_hash):
            return path
    return None


def load_result(path):
    """Load a JSON result file."""
    with open(path, "r") as f:
        return json.load(f)


def extract_timestamp_raw(path):
    """Extract the YYYYMMDD_HHMMSS timestamp from a result filename."""
    basename = os.path.basename(path)
    m = re.search(r'(\d{8}_\d{6})\.json$', basename)
    return m.group(1) if m else basename


def format_timestamp(raw_ts):
    """Convert YYYYMMDD_HHMMSS to human-readable 'YYYY-MM-DD HH:MM:SS'."""
    try:
        dt = datetime.strptime(raw_ts, "%Y%m%d_%H%M%S")
        return dt.strftime("%Y-%m-%d %H:%M:%S")
    except ValueError:
        return raw_ts


def compute_ratio(query_data):
    """Compute patch_on / master ratio using min times."""
    master_times = query_data.get("master", {}).get("times", [])
    patch_on_times = query_data.get("patch_on", {}).get("times", [])
    if not master_times or not patch_on_times:
        return None
    return min(patch_on_times) / min(master_times)


def get_patch_on_min(query_data):
    """Get the min patch_on time in ms."""
    times = query_data.get("patch_on", {}).get("times", [])
    return min(times) if times else None


def get_master_min(query_data):
    """Get the min master time in ms."""
    times = query_data.get("master", {}).get("times", [])
    return min(times) if times else None


def compute_geomean(ratios):
    """Compute geometric mean of a list of ratios."""
    if not ratios:
        return None
    log_sum = sum(math.log(r) for r in ratios)
    return math.exp(log_sum / len(ratios))


def do_list(results_dir):
    """List all available result files grouped by mode."""
    found_any = False
    for mode in ALL_MODES:
        files = find_result_files(results_dir, mode)
        if not files:
            continue
        found_any = True
        print(f"\n{BOLD}{mode}{RESET}:")
        for i, path in enumerate(files):
            ts = format_timestamp(extract_timestamp_raw(path))
            data = load_result(path)
            patch_hash = data.get("patch_hash", "?")
            master_hash = data.get("master_hash", "?")
            # Compute geomean for this run
            ratios = []
            for qid, qdata in data.get("queries", {}).items():
                r = compute_ratio(qdata)
                if r is not None:
                    ratios.append(r)
            gm = compute_geomean(ratios)
            gm_str = f"geomean={gm:.3f}x" if gm else "geomean=N/A"
            print(f"  #{i+1:<3} {ts}  patch={patch_hash}  master={master_hash}  {gm_str}")

    if not found_any:
        print(f"No result files found in {results_dir}/")


def compare_mode(old_data, new_data, mode, threshold, verbose):
    """Compare two result sets for a single mode.

    Returns a list of per-query comparison dicts and the per-mode geomean info.
    """
    old_queries = old_data.get("queries", {})
    new_queries = new_data.get("queries", {})

    # Use the union of query IDs from both
    all_qids = list(dict.fromkeys(
        list(old_queries.keys()) + list(new_queries.keys())
    ))

    comparisons = []
    old_ratios = []
    new_ratios = []

    for qid in all_qids:
        old_qdata = old_queries.get(qid)
        new_qdata = new_queries.get(qid)

        old_ratio = compute_ratio(old_qdata) if old_qdata else None
        new_ratio = compute_ratio(new_qdata) if new_qdata else None
        old_ms = get_patch_on_min(old_qdata) if old_qdata else None
        new_ms = get_patch_on_min(new_qdata) if new_qdata else None
        old_master_ms = get_master_min(old_qdata) if old_qdata else None
        new_master_ms = get_master_min(new_qdata) if new_qdata else None

        if old_ratio is not None:
            old_ratios.append(old_ratio)
        if new_ratio is not None:
            new_ratios.append(new_ratio)

        if old_ratio is None and new_ratio is None:
            continue

        delta = None
        abs_delta_ms = None
        if old_ratio is not None and new_ratio is not None:
            delta = new_ratio - old_ratio
        if old_ms is not None and new_ms is not None:
            abs_delta_ms = abs(new_ms - old_ms)

        name = ""
        if new_qdata:
            name = new_qdata.get("name", "")
        elif old_qdata:
            name = old_qdata.get("name", "")

        comparisons.append({
            "qid": qid,
            "name": name,
            "mode": mode,
            "old_ratio": old_ratio,
            "new_ratio": new_ratio,
            "delta": delta,
            "abs_delta_ms": abs_delta_ms,
            "old_ms": old_ms,
            "new_ms": new_ms,
            "new_master_ms": new_master_ms,
        })

    old_geomean = compute_geomean(old_ratios)
    new_geomean = compute_geomean(new_ratios)

    return comparisons, old_geomean, new_geomean


def classify_comparison(comp, threshold, abs_tolerance_ms):
    """Classify a single query comparison.

    Returns one of:
        'worsened_regression' - was regressed, got worse
        'new_regression' - crossed from ok to regressed
        'improved_regression' - was regressed, got better
        'notable_better' - improved beyond threshold, not a regression issue
        'notable_worse' - worsened beyond threshold, but not crossing into regression
        'noise' - change within noise threshold or absolute tolerance
        'missing' - one side has no data
    """
    old_r = comp["old_ratio"]
    new_r = comp["new_ratio"]
    delta = comp["delta"]
    abs_delta_ms = comp.get("abs_delta_ms")

    if old_r is None or new_r is None:
        return "missing"

    if abs(delta) <= threshold:
        return "noise"

    # If the absolute change in ms is tiny, treat as noise regardless of ratio
    if abs_delta_ms is not None and abs_delta_ms <= abs_tolerance_ms:
        return "noise"

    was_regressed = old_r > REGRESSION_BOUNDARY
    now_regressed = new_r > REGRESSION_BOUNDARY

    if was_regressed and delta > threshold:
        return "worsened_regression"
    if not was_regressed and now_regressed:
        return "new_regression"
    if was_regressed and delta < -threshold:
        return "improved_regression"
    if delta > threshold:
        return "notable_worse"
    return "notable_better"


def format_ratio(r):
    """Format a ratio for display."""
    if r is None:
        return "   N/A  "
    return f"{r:.3f}x"


def format_ms(ms):
    """Format a millisecond value for display."""
    if ms is None:
        return "N/A"
    return f"{ms:.3f}"


def print_mode_table(mode, old_ts, new_ts, comparisons, threshold, abs_tolerance_ms, verbose):
    """Print per-mode comparison table."""
    print(f"\n{BOLD}{mode}{RESET} (new: {format_timestamp(new_ts)}, old: {format_timestamp(old_ts)})")

    # Filter what to show
    if verbose:
        to_show = comparisons
    else:
        to_show = [c for c in comparisons
                    if classify_comparison(c, threshold, abs_tolerance_ms) not in ("noise", "missing")]

    if not to_show:
        print("  (no notable changes)")
        return

    # Header
    print(f"  {'Query':<8} {'master':>10} {'old patch':>10} {'new patch':>10} {'Old ratio':>10} {'New ratio':>10}  Status")
    print(f"  {'─'*8} {'─'*10} {'─'*10} {'─'*10} {'─'*10} {'─'*10}  {'─'*16}")

    for comp in to_show:
        cat = classify_comparison(comp, threshold, abs_tolerance_ms)
        status = ""
        color = ""
        if cat == "worsened_regression":
            status = "WORSE regression"
            color = RED
        elif cat == "new_regression":
            status = "NEW regression"
            color = RED
        elif cat == "improved_regression":
            status = "improved"
            color = GREEN
        elif cat == "notable_better":
            status = "better"
            color = GREEN
        elif cat == "notable_worse":
            status = "worse"
            color = RED
        elif cat == "noise":
            status = "~"
            color = ""

        line = (f"  {comp['qid']:<8} {format_ms(comp.get('new_master_ms')):>10} "
                f"{format_ms(comp.get('old_ms')):>10} {format_ms(comp.get('new_ms')):>10} "
                f"{format_ratio(comp['old_ratio']):>10} "
                f"{format_ratio(comp['new_ratio']):>10}")
        if color:
            print(f"{line}  {color}{status}{RESET}")
        elif status:
            print(f"{line}  {status}")
        else:
            print(line)


def main():
    args = parse_arguments()
    threshold = 0.0 if args.strict else args.threshold
    abs_tolerance_ms = 0.0 if args.strict else args.abs_tolerance

    if args.list:
        do_list(args.results_dir)
        return

    # If --new and --old are specified, compare those two files directly
    if args.new and args.old:
        old_data = load_result(args.old)
        new_data = load_result(args.new)
        mode = new_data.get("mode", os.path.basename(args.new))

        comparisons, old_gm, new_gm = compare_mode(
            old_data, new_data, mode, threshold, args.verbose)

        old_ts = extract_timestamp_raw(args.old)
        new_ts = extract_timestamp_raw(args.new)
        print_mode_table(mode, old_ts, new_ts, comparisons, threshold, abs_tolerance_ms, args.verbose)

        # Print geomean
        print(f"\n{BOLD}GEOMETRIC MEAN (patch_on vs master){RESET}")
        gm_delta = (new_gm - old_gm) if (old_gm and new_gm) else None
        direction = ""
        if gm_delta is not None:
            direction = f"{GREEN}better{RESET}" if gm_delta < -threshold else (
                f"{RED}worse{RESET}" if gm_delta > threshold else "~")
        print(f"  {format_ratio(old_gm):>10} -> {format_ratio(new_gm):>10}  {direction}")

        return

    if args.new or args.old:
        print("Error: --new and --old must be used together", file=sys.stderr)
        sys.exit(2)

    # Default: compare most recent vs previous (or vs --baseline) for all modes
    if args.baseline:
        print(f"Baseline patch hash: {BOLD}{args.baseline}{RESET}\n")

    all_comparisons = []
    all_old_ratios = []
    all_new_ratios = []
    mode_geomeans = []
    modes_processed = 0
    modes_skipped_no_files = []
    modes_skipped_no_baseline = []
    modes_skipped_single = []

    for mode in ALL_MODES:
        files = find_result_files(args.results_dir, mode)

        if args.baseline:
            # New = most recent, Old = most recent with matching patch_hash
            if not files:
                modes_skipped_no_files.append(mode)
                continue
            new_path = files[0]
            old_path = find_result_by_patch_hash(args.results_dir, mode, args.baseline, exclude_path=new_path)
            if not old_path:
                # Baseline matches the newest file itself - self-comparison
                old_path = find_result_by_patch_hash(args.results_dir, mode, args.baseline)
                if not old_path:
                    modes_skipped_no_baseline.append(mode)
                    continue
        else:
            if len(files) < 2:
                modes_skipped_single.append(mode)
                continue
            new_path = files[0]
            old_path = files[1]

        new_data = load_result(new_path)
        old_data = load_result(old_path)

        comparisons, old_gm, new_gm = compare_mode(
            old_data, new_data, mode, threshold, args.verbose)

        old_ts = extract_timestamp_raw(old_path)
        new_ts = extract_timestamp_raw(new_path)
        print_mode_table(mode, old_ts, new_ts, comparisons, threshold, abs_tolerance_ms, args.verbose)

        all_comparisons.extend(comparisons)
        mode_geomeans.append((mode, old_gm, new_gm))
        modes_processed += 1

        # Collect individual ratios for grand geomean
        for comp in comparisons:
            if comp["old_ratio"] is not None:
                all_old_ratios.append(comp["old_ratio"])
            if comp["new_ratio"] is not None:
                all_new_ratios.append(comp["new_ratio"])

    if modes_skipped_no_files:
        print(f"\n{YELLOW}Skipped (no result files):{RESET} {', '.join(modes_skipped_no_files)}")
    if modes_skipped_single:
        print(f"\n{YELLOW}Skipped (< 2 result files):{RESET} {', '.join(modes_skipped_single)}")
    if modes_skipped_no_baseline:
        # Show what hash the newest file has to help diagnose mismatches
        sample_files = find_result_files(args.results_dir, modes_skipped_no_baseline[0])
        actual_hash = ""
        if sample_files:
            actual_hash = load_result(sample_files[0]).get("patch_hash", "")
        hint = ""
        if actual_hash:
            hint = f" (newest results have patch_hash={actual_hash})"
        print(f"\n{YELLOW}Skipped (no results matching baseline {args.baseline}{hint}):{RESET} "
              f"{', '.join(modes_skipped_no_baseline)}")

    if modes_processed == 0:
        print("\nNo modes had enough result files to compare.")
        sys.exit(2)

    # Regression watchlist
    worsened = [c for c in all_comparisons
                if classify_comparison(c, threshold, abs_tolerance_ms) == "worsened_regression"]
    new_regs = [c for c in all_comparisons
                if classify_comparison(c, threshold, abs_tolerance_ms) == "new_regression"]
    improved = [c for c in all_comparisons
                if classify_comparison(c, threshold, abs_tolerance_ms) == "improved_regression"]

    def print_regression_table(title, entries, color, sort_key, sort_reverse=True):
        if not entries:
            return
        print(f"\n{color}{BOLD}{title}{RESET}")
        print(f"  {'Query':<8} {'Mode':<28} {'master':>10} {'patch':>10} {'Old ratio':>10} {'New ratio':>10}")
        print(f"  {'─'*8} {'─'*28} {'─'*10} {'─'*10} {'─'*10} {'─'*10}")
        for c in sorted(entries, key=sort_key, reverse=sort_reverse):
            print(f"  {c['qid']:<8} {c['mode']:<28} {format_ms(c.get('new_master_ms')):>10} {format_ms(c.get('new_ms')):>10} "
                  f"{format_ratio(c['old_ratio']):>10} "
                  f"{color}{format_ratio(c['new_ratio']):>10}{RESET}")

    print_regression_table("WORSENED REGRESSIONS (vs master)", worsened, RED,
                           lambda x: x["delta"], sort_reverse=True)
    print_regression_table("NEW REGRESSIONS (vs master)", new_regs, RED,
                           lambda x: x["new_ratio"], sort_reverse=True)

    print_regression_table("IMPROVED REGRESSIONS (vs master)", improved, GREEN,
                           lambda x: x["delta"], sort_reverse=False)

    notable_better = [c for c in all_comparisons
                      if classify_comparison(c, threshold, abs_tolerance_ms) == "notable_better"]
    print_regression_table("IMPROVED NON-REGRESSIONS (vs master)", notable_better, GREEN,
                           lambda x: x["delta"], sort_reverse=False)

    # Geometric means table
    grand_old_gm = compute_geomean(all_old_ratios)
    grand_new_gm = compute_geomean(all_new_ratios)

    print(f"\n{BOLD}GEOMETRIC MEANS (patch_on vs master){RESET}")
    print(f"  {'Mode':<28} {'Old geomean':>12} {'New geomean':>12}  Direction")
    print(f"  {'─'*28} {'─'*12} {'─'*12}  {'─'*10}")

    for mode, old_gm, new_gm in mode_geomeans:
        gm_delta = (new_gm - old_gm) if (old_gm and new_gm) else None
        direction = ""
        if gm_delta is not None:
            if gm_delta < -threshold:
                direction = f"{GREEN}better{RESET}"
            elif gm_delta > threshold:
                direction = f"{RED}worse{RESET}"
            else:
                direction = "~"
        print(f"  {mode:<28} {format_ratio(old_gm):>12} {format_ratio(new_gm):>12}  {direction}")

    # Grand geomean
    grand_delta = (grand_new_gm - grand_old_gm) if (grand_old_gm and grand_new_gm) else None
    grand_dir = ""
    if grand_delta is not None:
        if grand_delta < -threshold:
            grand_dir = f"{GREEN}better{RESET}"
        elif grand_delta > threshold:
            grand_dir = f"{RED}worse{RESET}"
        else:
            grand_dir = "~"
    print(f"  {'─'*28} {'─'*12} {'─'*12}  {'─'*10}")
    print(f"  {BOLD}{'OVERALL':28}{RESET} {format_ratio(grand_old_gm):>12} {format_ratio(grand_new_gm):>12}  {grand_dir}")

    # Summary counts
    print(f"\n  Worsened regressions: {len(worsened)}")
    print(f"  New regressions:      {len(new_regs)}")
    print(f"  Improved regressions: {len(improved)}")


if __name__ == "__main__":
    main()
