#!/usr/bin/env python3
"""
Write regression benchmark for PostgreSQL index scan prefetching.

Reproduces a known regression where the patch version is ~2x slower than
master on a cold-start index scan over a table with dirty pages (after
an UPDATE that touches ~1/61 of rows).

Based on microbenchmarks/write-regression.sql.

Usage:
    ./write_regression.py                 # Run benchmark, 3 runs per version
    ./write_regression.py --runs 5        # 5 runs per version
"""

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime
from statistics import mean, median

import psycopg

from benchmark_common import (
    MASTER_BIN, PATCH_BIN,
    MASTER_DATA_DIR, PATCH_DATA_DIR,
    MASTER_SOURCE_DIR, PATCH_SOURCE_DIR,
    MASTER_CONN, PATCH_CONN,
    clear_os_cache,
    extract_execution_time,
    setup_tmpfs_hugepages, copy_binaries_to_tmpfs, cleanup_tmpfs,
)

# --- Configuration ---

OUTPUT_DIR = "write_regression_results"

TABLE_NAME = "t_write_regress"
INDEX_NAME = "idx_write_regress"

# --- Data Loading SQL ---
# Exact reproduction from write-regression.sql with renamed table/index.

DATA_LOADING_STATEMENTS = [
    "CREATE EXTENSION IF NOT EXISTS pg_prewarm",
    "CREATE EXTENSION IF NOT EXISTS pg_buffercache",
    "CREATE EXTENSION IF NOT EXISTS pg_walinspect",

    "DROP TABLE IF EXISTS t_write_regress CASCADE",

    """CREATE TABLE t_write_regress (a bigint, b text)
       WITH (fillfactor = 10, autovacuum_enabled = false)""",

    """INSERT INTO t_write_regress
SELECT
  -1 * a,
  b
FROM (SELECT r, a, b, generate_series(0, 4 - 1) as p
  FROM ( SELECT row_number() over () as r, a, b
    FROM ( SELECT i as a, md5(i::text) as b
      FROM generate_series(1, 2500000) s(i)
      ORDER BY
        (i + 256 *(random() - 0.5))) foo) bar) baz
ORDER BY
  ((r * 4 + p) + 32 *(random() - 0.5))""",

    "CREATE INDEX idx_write_regress ON t_write_regress(a ASC)",

    "VACUUM ANALYZE t_write_regress",

    "UPDATE t_write_regress SET a=a WHERE random() < 1.0 / 61.000000",

    "CHECKPOINT",
]

STATEMENT_LABELS = {
    "INSERT INTO": "Inserting ~10M rows into t_write_regress...",
    "CREATE INDEX": "Creating index idx_write_regress...",
    "VACUUM": "Running VACUUM ANALYZE...",
    "UPDATE": "Running UPDATE to dirty ~1/61 of rows...",
    "CHECKPOINT": "Running CHECKPOINT...",
}

# --- Benchmark Query ---

BENCHMARK_QUERY = """\
SELECT * FROM (
    SELECT a FROM t_write_regress
    WHERE a BETWEEN -1572936 AND -24361
    ORDER BY a ASC
    OFFSET 1000000000
) AS sub"""


# --- Helper Functions ---

def start_server(pg_bin_dir, pg_name, pg_data_dir, conn_details):
    """Start a PostgreSQL server and wait for it to be ready."""
    pg_ctl_path = os.path.join(pg_bin_dir, "pg_ctl")
    log_file = os.path.join(OUTPUT_DIR, f"{pg_name}.postgres_log")

    # Ensure server is stopped before we start
    result = subprocess.run(
        [pg_ctl_path, "status", "-D", pg_data_dir],
        capture_output=True, check=False
    )
    if result.returncode == 0:
        print(f"{pg_name}: Server is already running. Stopping it...")
        subprocess.run([pg_ctl_path, "stop", "-D", pg_data_dir, "-m", "fast"], check=True)
        time.sleep(2)

    # Start the server
    print(f"Starting {pg_name} server (port {conn_details.get('port', 'default')})...")
    start_options = f"-p {conn_details['port']}" if 'port' in conn_details else ""

    result = subprocess.run(
        [pg_ctl_path, "start",
         "-o", "--autovacuum=off",
         "-D", pg_data_dir,
         "-l", log_file,
         "-o", start_options],
        capture_output=True, text=True
    )

    if result.returncode != 0:
        print(f"Error: Failed to start {pg_name} server")
        print(f"stdout: {result.stdout}")
        print(f"stderr: {result.stderr}")
        try:
            with open(log_file, 'r') as f:
                print(f"Log file contents:\n{f.read()}")
        except FileNotFoundError:
            pass
        sys.exit(1)

    # Wait for the server to be ready
    for attempt in range(15):
        try:
            conn = psycopg.connect(**conn_details, connect_timeout=2)
            conn.close()
            print(f"{pg_name} server started.")
            return
        except (psycopg.OperationalError, psycopg.DatabaseError):
            if attempt == 14:
                print(f"Error: {pg_name} server failed to start after 15 attempts")
                sys.exit(1)
            time.sleep(0.5 if attempt < 5 else 1)


def stop_server(pg_bin_dir, pg_data_dir):
    """Stop a PostgreSQL server."""
    pg_ctl_path = os.path.join(pg_bin_dir, "pg_ctl")
    subprocess.run([pg_ctl_path, "stop", "-D", pg_data_dir, "-m", "fast"], check=False)


def get_git_hash(source_dir):
    """Get current git commit hash."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=source_dir, capture_output=True, text=True, check=True
        )
        return result.stdout.strip()
    except subprocess.CalledProcessError:
        return "unknown"


def get_current_lsn(conn):
    """Get the current WAL LSN."""
    with conn.cursor() as cur:
        cur.execute("SELECT pg_current_wal_lsn()")
        return cur.fetchone()[0]


def get_wal_stats(conn, start_lsn, end_lsn):
    """Get WAL stats between two LSNs using pg_get_wal_stats.

    Returns a dict with the LSN pair and the stats rows, or None if no WAL
    was written (start_lsn == end_lsn).
    """
    result = {
        "start_lsn": start_lsn,
        "end_lsn": end_lsn,
        "rows": [],
    }

    if start_lsn == end_lsn:
        return result

    with conn.cursor() as cur:
        cur.execute(
            'SELECT "resource_manager/record_type", count, record_size, fpi_size, combined_size '
            "FROM pg_get_wal_stats(%s, %s, per_record := true) "
            "WHERE count > 0 "
            "ORDER BY combined_size DESC",
            (start_lsn, end_lsn)
        )
        for row in cur.fetchall():
            result["rows"].append({
                "resource_manager": row[0],
                "count": row[1],
                "record_size": row[2],
                "fpi_size": row[3],
                "combined_size": row[4],
            })

    return result


def print_wal_stats(wal_stats, indent="  "):
    """Print WAL stats in a readable format."""
    print(f"{indent}LSN range: {wal_stats['start_lsn']} .. {wal_stats['end_lsn']}")
    if not wal_stats["rows"]:
        print(f"{indent}(no WAL written)")
        return
    print(f"{indent}{'Resource Manager':<20} {'Count':>8} {'Record':>12} {'FPI':>12} {'Combined':>12}")
    print(f"{indent}{'-'*20} {'-'*8} {'-'*12} {'-'*12} {'-'*12}")
    for row in wal_stats["rows"]:
        print(f"{indent}{row['resource_manager']:<20} {row['count']:>8} "
              f"{row['record_size']:>12} {row['fpi_size']:>12} {row['combined_size']:>12}")


def load_data(conn_details, seed=None):
    """Load write-regression benchmark data.

    Calls SELECT setseed(seed) before loading so that random() calls in the
    INSERT and UPDATE produce identical results across master and patch.

    Returns a list of {statement, wal_stats} dicts for each statement that
    had a WAL stats label (INSERT, CREATE INDEX, VACUUM, UPDATE, CHECKPOINT).
    """
    print("\n" + "=" * 50)
    print("Loading write-regression benchmark data...")
    print("This will take several minutes for 10M rows.")
    print(f"Using random seed: {seed}")
    print("=" * 50 + "\n")

    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    with conn.cursor() as cur:
        cur.execute(f"SELECT setseed({seed})")

    load_wal_stats = []

    for statement in DATA_LOADING_STATEMENTS:
        # Find label for this statement (if any)
        label = None
        for prefix, lbl in STATEMENT_LABELS.items():
            if statement.strip().upper().startswith(prefix):
                label = lbl
                print(label)
                break

        try:
            start_lsn = get_current_lsn(conn)
            with conn.cursor() as cur:
                cur.execute(statement)
            end_lsn = get_current_lsn(conn)

            if label:
                wal = get_wal_stats(conn, start_lsn, end_lsn)
                load_wal_stats.append({"statement": label, "wal_stats": wal})
                print_wal_stats(wal, indent="    ")
        except Exception as e:
            print(f"Error executing: {statement[:80]}...")
            print(f"Error: {e}")
            conn.close()
            sys.exit(1)

    conn.close()
    print("Data loading complete.\n")
    return load_wal_stats


def run_cold_benchmark(pg_bin_dir, pg_name, pg_data_dir, conn_details,
                       is_master, num_runs, prefetch=False):
    """
    Run the write-regression benchmark with cold starts.

    For each run:
      1. Stop server
      2. Clear OS page cache
      3. Start server fresh
      4. Connect, set GUCs, run EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, BUFFERS, WAL)
      5. Record execution time and full explain output
      6. Stop server

    Returns dict with times, all explain outputs, and statistics.
    """
    results = {
        "times": [],
        "explains": [],  # full explain output for every run
        "wal_stats": [],  # WAL stats for every run
    }

    for run in range(num_runs):
        # Ensure server is stopped
        stop_server(pg_bin_dir, pg_data_dir)
        time.sleep(1)

        # Clear OS page cache for truly cold start
        clear_os_cache()
        time.sleep(0.5)

        # Start fresh server
        start_server(pg_bin_dir, pg_name, pg_data_dir, conn_details)

        try:
            conn = psycopg.connect(**conn_details)

            # Set GUCs
            with conn.cursor() as cur:
                cur.execute("SET enable_bitmapscan = off")
                cur.execute("SET enable_seqscan = off")
                cur.execute("SET max_parallel_workers_per_gather = 0")
                cur.execute("SET enable_indexonlyscan = off")
                if not is_master:
                    cur.execute(f"SET enable_indexscan_prefetch = {'on' if prefetch else 'off'}")

            # Capture WAL LSN before query
            start_lsn = get_current_lsn(conn)

            # Run EXPLAIN ANALYZE with BUFFERS
            with conn.cursor() as cur:
                explain_sql = f"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, BUFFERS, WAL) {BENCHMARK_QUERY}"
                cur.execute(explain_sql)
                rows = cur.fetchall()

            # Capture WAL LSN after query and get stats
            end_lsn = get_current_lsn(conn)
            wal = get_wal_stats(conn, start_lsn, end_lsn)
            results["wal_stats"].append(wal)

            conn.close()

            # Extract execution time
            exec_time = extract_execution_time(rows)
            explain_text = "\n".join(row[0] for row in rows)
            results["explains"].append(explain_text)

            if exec_time is not None:
                results["times"].append(exec_time)
                print(f"  Run {run + 1}/{num_runs}: {exec_time:.3f} ms")
            else:
                print(f"  Run {run + 1}/{num_runs}: FAILED to extract time")

            # Print full explain for this run
            print(f"  --- EXPLAIN output (run {run + 1}) ---")
            for line in explain_text.split('\n'):
                print(f"    {line}")

            # Print WAL stats for this run
            print(f"  --- WAL stats (run {run + 1}) ---")
            print_wal_stats(wal, indent="    ")
            print()

        except Exception as e:
            print(f"  Run {run + 1}/{num_runs}: ERROR: {e}")
        finally:
            stop_server(pg_bin_dir, pg_data_dir)
            time.sleep(1)

    # Calculate statistics
    if results["times"]:
        results["avg"] = mean(results["times"])
        results["min"] = min(results["times"])
        results["median"] = median(results["times"])
        results["max"] = max(results["times"])

    return results


# --- Argument Parsing ---

def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Write regression benchmark for PostgreSQL index scan prefetching.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Reproduces a known regression where the patch version is ~2x slower than
master on a cold-start index scan over a table with dirty pages.

Examples:
    %(prog)s                     # Run benchmark, 3 runs per version
    %(prog)s --runs 5            # 5 runs per version
    %(prog)s --no-tmpfs-hugepages  # Don't use tmpfs for binaries
        """
    )
    parser.add_argument("--runs", type=int, default=3,
                        help="Number of cold-start runs per version (default: 3)")
    parser.add_argument("--no-tmpfs-hugepages", action="store_true",
                        help="Disable tmpfs with huge=always for binaries")
    parser.add_argument("--median", action="store_true", dest="use_median",
                        help="Use median instead of min as the representative value")
    parser.add_argument("--prefetch", action="store_true",
                        help="Enable enable_indexscan_prefetch on patch (default: off)")
    parser.add_argument("--seed", type=float, default=0.12345,
                        help="PostgreSQL random seed (SELECT setseed(N)) before data loading, "
                             "so both master and patch get identical data layout (default: 0.12345)")
    return parser.parse_args()


# --- Main ---

def main():
    args = parse_arguments()
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    get_rep = (lambda times: median(times)) if args.use_median else (lambda times: min(times))
    stat_label = "median" if args.use_median else "min"

    # Setup tmpfs for binaries
    tmpfs_mount = None
    master_bin = MASTER_BIN
    patch_bin = PATCH_BIN

    if not args.no_tmpfs_hugepages:
        tmpfs_mount = setup_tmpfs_hugepages()
        master_bin = copy_binaries_to_tmpfs(MASTER_BIN, tmpfs_mount, "master")
        patch_bin = copy_binaries_to_tmpfs(PATCH_BIN, tmpfs_mount, "patch")
        print(f"Using tmpfs binaries:")
        print(f"  master: {master_bin}")
        print(f"  patch:  {patch_bin}\n")

    master_hash = get_git_hash(MASTER_SOURCE_DIR)
    patch_hash = get_git_hash(PATCH_SOURCE_DIR)

    BOLD = "\033[1m"
    RESET = "\033[0m"

    print("=" * 60)
    print("Write Regression Benchmark")
    print("=" * 60)
    print(f"Master git hash: {master_hash}")
    print(f"Patch git hash:  {patch_hash}")
    print(f"Runs per version: {args.runs}")
    print(f"Patch prefetch: {'on' if args.prefetch else 'off'}")
    print(f"Representative stat: {stat_label}")
    print("=" * 60)

    try:
        # --- Data loading phase (always reload — benchmark involves writes) ---
        print("\n--- Loading data on master ---")
        start_server(master_bin, "master", MASTER_DATA_DIR, MASTER_CONN)
        master_load_wal = load_data(MASTER_CONN, seed=args.seed)
        stop_server(master_bin, MASTER_DATA_DIR)
        time.sleep(2)

        print("\n--- Loading data on patch ---")
        start_server(patch_bin, "patch", PATCH_DATA_DIR, PATCH_CONN)
        patch_load_wal = load_data(PATCH_CONN, seed=args.seed)
        stop_server(patch_bin, PATCH_DATA_DIR)
        time.sleep(2)

        # --- Benchmark phase ---
        print(f"\n{'=' * 60}")
        print("Running cold-start benchmark on MASTER")
        print(f"{'=' * 60}")
        master_results = run_cold_benchmark(
            master_bin, "master", MASTER_DATA_DIR, MASTER_CONN,
            is_master=True, num_runs=args.runs,
        )

        print(f"\n{'=' * 60}")
        print("Running cold-start benchmark on PATCH")
        print(f"{'=' * 60}")
        patch_results = run_cold_benchmark(
            patch_bin, "patch", PATCH_DATA_DIR, PATCH_CONN,
            is_master=False, num_runs=args.runs, prefetch=args.prefetch,
        )

        # --- Results summary ---
        master_rep = get_rep(master_results["times"]) if master_results["times"] else None
        patch_rep = get_rep(patch_results["times"]) if patch_results["times"] else None

        print(f"\n{'=' * 60}")
        print("RESULTS SUMMARY")
        print(f"{'=' * 60}")

        if master_rep:
            print(f"  master ({stat_label}): {master_rep:10.3f} ms "
                  f"(avg={master_results['avg']:.3f}, min={master_results['min']:.3f}, "
                  f"max={master_results['max']:.3f})")
        if patch_rep:
            print(f"  patch  ({stat_label}): {patch_rep:10.3f} ms "
                  f"(avg={patch_results['avg']:.3f}, min={patch_results['min']:.3f}, "
                  f"max={patch_results['max']:.3f})")

        ratio = None
        if master_rep and patch_rep:
            ratio = patch_rep / master_rep
            print(f"\n  {BOLD}Ratio (patch/master): {ratio:.3f}x{RESET}")
            if ratio > 1.05:
                print(f"  {BOLD}REGRESSION: patch is {(ratio - 1) * 100:.1f}% slower{RESET}")
            elif ratio < 0.95:
                print(f"  {BOLD}IMPROVEMENT: patch is {(1 - ratio) * 100:.1f}% faster{RESET}")
            else:
                print(f"  NEUTRAL: within 5% of master")

        # --- Print load WAL stats ---
        print(f"\n{'=' * 60}")
        print("DATA LOADING WAL STATS")
        print(f"{'=' * 60}")

        for version_name, load_wal in [("master", master_load_wal), ("patch", patch_load_wal)]:
            print(f"\n  {version_name}:")
            for entry in load_wal:
                print(f"    {entry['statement']}")
                print_wal_stats(entry["wal_stats"], indent="      ")

        # --- Print all EXPLAIN outputs ---
        print(f"\n{'=' * 60}")
        print("ALL EXPLAIN OUTPUTS")
        print(f"{'=' * 60}")

        for version_name, results in [("master", master_results), ("patch", patch_results)]:
            for i, explain in enumerate(results["explains"]):
                time_str = f"{results['times'][i]:.3f} ms" if i < len(results["times"]) else "N/A"
                print(f"\n--- {version_name} run {i + 1} ({time_str}) ---")
                for line in explain.split('\n'):
                    print(f"  {line}")
                if i < len(results["wal_stats"]):
                    print(f"  WAL stats:")
                    print_wal_stats(results["wal_stats"][i], indent="    ")

        # --- Save results to JSON ---
        json_results = {
            "timestamp": datetime.now().isoformat(),
            "master_hash": master_hash,
            "patch_hash": patch_hash,
            "runs": args.runs,
            "stat_label": stat_label,
            "query": BENCHMARK_QUERY.strip(),
            "master_load_wal": master_load_wal,
            "patch_load_wal": patch_load_wal,
            "master": master_results,
            "patch": patch_results,
            "ratio": ratio,
        }

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        json_file = os.path.join(OUTPUT_DIR, f"write_regression_{timestamp}.json")
        with open(json_file, "w") as f:
            json.dump(json_results, f, indent=2)
        print(f"\nResults saved to: {json_file}")

    finally:
        stop_server(master_bin, MASTER_DATA_DIR)
        stop_server(patch_bin, PATCH_DATA_DIR)
        if tmpfs_mount:
            cleanup_tmpfs(tmpfs_mount)


if __name__ == "__main__":
    main()
