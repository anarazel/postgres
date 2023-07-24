#!/usr/bin/env python3
"""
Fillfactor benchmark for PostgreSQL index scan prefetching.

Demonstrates that lower heap fillfactors (more heap pages) can paradoxically
complete FASTER than high fillfactors under direct I/O, because the prefetch
yield logic destroys I/O concurrency when cache hit ratios are high.

Tests at pgbench scale 100, varying fillfactor (default: 90, 50, 25).

Tables are renamed to pgbench_accounts_ff{N} after loading, so multiple
fillfactors coexist and subsequent runs auto-skip data loading.

Usage:
    ./fillfactor_benchmark.py                 # Run benchmark, 3 runs per config
    ./fillfactor_benchmark.py --runs 5        # 5 runs per config
    ./fillfactor_benchmark.py --fillfactors 100,90,50,25,10
    ./fillfactor_benchmark.py --no-master     # Skip master for reference
    ./fillfactor_benchmark.py --no-direct-io  # Use OS filesystem cache instead
"""

import argparse
import json
import os
import re
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

OUTPUT_DIR = "fillfactor_results"

# --- Per-fillfactor table naming ---
# After pgbench -i, tables are renamed to pgbench_accounts_ff{N} so that
# multiple fillfactors can coexist and data loading can be skipped.

def heap_relation(fillfactor):
    """Return the heap table name for a given fillfactor."""
    return f"pgbench_accounts_ff{fillfactor}"


def index_relation(fillfactor):
    """Return the primary key index name for a given fillfactor."""
    return f"pgbench_accounts_ff{fillfactor}_pkey"


def benchmark_query(fillfactor):
    """Return the EXPLAIN ANALYZE query for a given fillfactor."""
    return (f"EXPLAIN (ANALYZE, BUFFERS) "
            f"SELECT * FROM {heap_relation(fillfactor)} ORDER BY aid")


# --- Helper Functions ---

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


def start_server(pg_bin_dir, pg_name, pg_data_dir, conn_details,
                 shared_buffers="2GB", direct_io=True):
    """Start a PostgreSQL server with io_uring and optional direct I/O."""
    pg_ctl_path = os.path.join(pg_bin_dir, "pg_ctl")
    log_file = os.path.join(OUTPUT_DIR, f"{pg_name}.postgres_log")

    # Ensure server is stopped
    result = subprocess.run(
        [pg_ctl_path, "status", "-D", pg_data_dir],
        capture_output=True, check=False
    )
    if result.returncode == 0:
        print(f"{pg_name}: Server already running. Stopping...")
        subprocess.run([pg_ctl_path, "stop", "-D", pg_data_dir, "-m", "fast"],
                       check=True)
        time.sleep(2)

    print(f"Starting {pg_name} server (port {conn_details.get('port', 'default')})...")
    start_options = f"-p {conn_details['port']}" if 'port' in conn_details else ""

    pg_options = [
        "--autovacuum=off",
        f"-c shared_buffers={shared_buffers}",
        "-c io_method=io_uring",
    ]
    if direct_io:
        pg_options.append("-c debug_io_direct=data")

    cmd = [pg_ctl_path, "start", "-D", pg_data_dir, "-l", log_file]
    for opt in pg_options:
        cmd.extend(["-o", opt])
    if start_options:
        cmd.extend(["-o", start_options])

    result = subprocess.run(cmd, capture_output=True, text=True)
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

    # Wait for ready
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
    subprocess.run([pg_ctl_path, "stop", "-D", pg_data_dir, "-m", "fast"],
                   check=False)


def init_pgbench(pg_bin_dir, conn_details, scale, fillfactor):
    """Initialize pgbench data with the given scale and fillfactor."""
    pgbench_path = os.path.join(pg_bin_dir, "pgbench")
    port = conn_details.get("port", 5432)
    dbname = conn_details["dbname"]

    print(f"  Initializing pgbench: scale={scale}, fillfactor={fillfactor}...")
    cmd = [
        pgbench_path,
        "-i", "-q",
        "-s", str(scale),
        f"--fillfactor={fillfactor}",
        "-p", str(port),
        "-h", conn_details.get("host", "/tmp"),
        "-U", conn_details.get("user", "pg"),
        dbname,
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Error: pgbench init failed")
        print(f"stdout: {result.stdout}")
        print(f"stderr: {result.stderr}")
        sys.exit(1)
    print(f"  pgbench init complete (fillfactor={fillfactor})")


def ensure_extensions(conn):
    """Ensure pg_prewarm and pg_buffercache extensions exist."""
    with conn.cursor() as cur:
        cur.execute("CREATE EXTENSION IF NOT EXISTS pg_prewarm")
        cur.execute("CREATE EXTENSION IF NOT EXISTS pg_buffercache")


def get_relation_size(conn, relation):
    """Get the size of a relation in pages (8kB blocks)."""
    with conn.cursor() as cur:
        cur.execute(f"SELECT pg_relation_size('{relation}') / 8192")
        return cur.fetchone()[0]


def extract_buffer_stats(explain_text):
    """Extract execution buffer statistics from EXPLAIN (BUFFERS) output.

    Parses only the first Buffers: line (execution), ignoring any
    subsequent ones (e.g. Planning: Buffers:).

    Returns dict with shared_hit and shared_read counts.
    """
    stats = {"shared_hit": 0, "shared_read": 0}
    for line in explain_text.split('\n'):
        if 'Planning' in line:
            break  # Stop before planning section
        m = re.search(r'Buffers:.*shared\s+(.*)', line)
        if m:
            buf_str = m.group(1)
            hit_m = re.search(r'hit=(\d+)', buf_str)
            read_m = re.search(r'read=(\d+)', buf_str)
            if hit_m:
                stats["shared_hit"] = int(hit_m.group(1))
            if read_m:
                stats["shared_read"] = int(read_m.group(1))
            break  # Only parse the first Buffers line
    return stats


def run_single_benchmark(conn, fillfactor, is_master, prefetch_setting,
                         direct_io):
    """Run a single benchmark iteration.

    Evicts heap from shared buffers, prewarms index, optionally clears OS
    page cache (when not using direct I/O), then runs the benchmark query.

    Returns (execution_time_ms, explain_text, buffer_stats) tuple.
    """
    heap_rel = heap_relation(fillfactor)
    idx_rel = index_relation(fillfactor)

    # Evict heap, prewarm index (matching the reviewer's test procedure)
    with conn.cursor() as cur:
        cur.execute(
            f"SELECT * FROM pg_buffercache_evict_relation('{heap_rel}')")
    with conn.cursor() as cur:
        cur.execute(f"SELECT pg_prewarm('{idx_rel}')")

    # Clear OS page cache when not using direct I/O
    if not direct_io:
        clear_os_cache()

    # Set GUCs
    with conn.cursor() as cur:
        cur.execute("SET enable_bitmapscan = off")
        cur.execute("SET enable_seqscan = off")
        cur.execute("SET max_parallel_workers_per_gather = 0")
        if not is_master and prefetch_setting is not None:
            cur.execute(
                f"SET enable_indexscan_prefetch = {prefetch_setting}")

    # Run the benchmark query
    query = benchmark_query(fillfactor)
    with conn.cursor() as cur:
        cur.execute(query)
        rows = cur.fetchall()

    explain_text = "\n".join(row[0] for row in rows)
    exec_time = extract_execution_time(rows)
    buffer_stats = extract_buffer_stats(explain_text)

    return exec_time, explain_text, buffer_stats


def table_exists(conn, table_name):
    """Check if a table exists in the database."""
    with conn.cursor() as cur:
        cur.execute(
            "SELECT EXISTS (SELECT 1 FROM pg_class WHERE relname = %s "
            "AND relkind = 'r')",
            (table_name,))
        return cur.fetchone()[0]


def load_fillfactor_data(pg_bin_dir, pg_name, pg_data_dir, conn_details,
                         fillfactor, scale, shared_buffers, direct_io):
    """Load pgbench data for a given fillfactor.

    Starts the server, runs pgbench -i, VACUUM FREEZE ANALYZE, renames
    the table to a fillfactor-specific name, CHECKPOINTs, then stops
    the server so disk state is clean for benchmarking.

    If the fillfactor-specific table already exists, loading is skipped
    and only the relation sizes are returned.

    Returns dict with heap_pages and index_pages.
    """
    heap_rel = heap_relation(fillfactor)
    idx_rel = index_relation(fillfactor)

    print(f"\n  Loading data on {pg_name} (fillfactor={fillfactor})...")
    start_server(pg_bin_dir, pg_name, pg_data_dir, conn_details,
                 shared_buffers, direct_io)

    try:
        conn = psycopg.connect(**conn_details)
        conn.autocommit = True

        ensure_extensions(conn)

        if table_exists(conn, heap_rel):
            print(f"  Table {heap_rel} already exists, skipping init.")
        else:
            init_pgbench(pg_bin_dir, conn_details, scale, fillfactor)

            print("  Running VACUUM FREEZE ANALYZE pgbench_accounts...")
            with conn.cursor() as cur:
                cur.execute("VACUUM FREEZE ANALYZE pgbench_accounts")

            # Rename to fillfactor-specific names
            print(f"  Renaming pgbench_accounts -> {heap_rel}...")
            with conn.cursor() as cur:
                cur.execute(
                    f"ALTER TABLE pgbench_accounts RENAME TO {heap_rel}")
                cur.execute(
                    f"ALTER INDEX pgbench_accounts_pkey "
                    f"RENAME TO {idx_rel}")

            print("  Running CHECKPOINT...")
            with conn.cursor() as cur:
                cur.execute("CHECKPOINT")

        heap_pages = get_relation_size(conn, heap_rel)
        index_pages = get_relation_size(conn, idx_rel)
        print(f"  {heap_rel}: {heap_pages} pages "
              f"({heap_pages * 8 / 1024:.0f} MB)")
        print(f"  {idx_rel}: {index_pages} pages "
              f"({index_pages * 8 / 1024:.0f} MB)")

        conn.close()
    finally:
        stop_server(pg_bin_dir, pg_data_dir)
        time.sleep(2)

    return {"heap_pages": heap_pages, "index_pages": index_pages}


def run_benchmark_for_fillfactor(pg_bin_dir, pg_name, pg_data_dir,
                                  conn_details, fillfactor, is_master,
                                  num_runs, shared_buffers, direct_io,
                                  test_prefetch_disabled=False):
    """Run benchmark queries for one fillfactor on a fresh server.

    Server is started fresh (no leftover shared buffer state from data
    loading), benchmarks are run, then server is stopped.

    Returns dict with per-config benchmark results.
    """
    start_server(pg_bin_dir, pg_name, pg_data_dir, conn_details,
                 shared_buffers, direct_io)

    ff_results = {}

    try:
        conn = psycopg.connect(**conn_details)
        conn.autocommit = True

        if is_master:
            configs = [("master", None)]
        else:
            configs = [("prefetch_on", "on")]
            if test_prefetch_disabled:
                configs.insert(0, ("prefetch_off", "off"))

        for config_name, prefetch_setting in configs:
            print(f"\n  Config: {config_name} ({num_runs} runs)")

            # Warmup run: primes catalog caches so timed runs are consistent
            print(f"    Warmup run (discarded)...")
            run_single_benchmark(conn, fillfactor, is_master,
                                 prefetch_setting, direct_io)

            config_data = {
                "times": [], "explains": [], "buffer_stats": []
            }

            for run in range(num_runs):
                exec_time, explain_text, buffer_stats = \
                    run_single_benchmark(
                        conn, fillfactor, is_master, prefetch_setting,
                        direct_io)

                if exec_time is not None:
                    config_data["times"].append(exec_time)
                    config_data["explains"].append(explain_text)
                    config_data["buffer_stats"].append(buffer_stats)
                    print(f"    Run {run + 1}/{num_runs}: "
                          f"{exec_time:.3f} ms "
                          f"(hit={buffer_stats.get('shared_hit', '?')}, "
                          f"read={buffer_stats.get('shared_read', '?')})")
                else:
                    print(f"    Run {run + 1}/{num_runs}: FAILED")

            if config_data["times"]:
                config_data["avg"] = mean(config_data["times"])
                config_data["min"] = min(config_data["times"])
                config_data["median"] = median(config_data["times"])
                config_data["max"] = max(config_data["times"])
                best_idx = config_data["times"].index(
                    min(config_data["times"]))
                config_data["best_explain"] = \
                    config_data["explains"][best_idx]
                config_data["best_buffer_stats"] = \
                    config_data["buffer_stats"][best_idx]

            ff_results[config_name] = config_data

        conn.close()
    finally:
        stop_server(pg_bin_dir, pg_data_dir)
        time.sleep(2)

    return ff_results


def print_summary(patch_results, master_results, fillfactors, stat_label):
    """Print a summary table of results."""
    BOLD = "\033[1m"
    RESET = "\033[0m"

    get_rep = ((lambda t: median(t)) if stat_label == "median"
               else (lambda t: min(t)))

    # Detect which columns have data
    has_master = master_results is not None
    has_off = any(
        "prefetch_off" in patch_results.get(ff, {})
        for ff in fillfactors)

    print(f"\n{'=' * 80}")
    print(f"RESULTS SUMMARY (representative value: {stat_label})")
    print(f"{'=' * 80}")

    # Header — only show columns that have data
    header = f"{'FF':>4}  {'Heap Pages':>10}  "
    if has_master:
        header += f"{'Master':>10}  "
    if has_off:
        header += f"{'Pfetch OFF':>10}  "
    header += f"{'Pfetch ON':>10}  "
    if has_master:
        header += f"{'ON/Master':>10}"
    if has_off:
        header += f"{'ON/OFF':>8}"
    print(header)
    print("-" * len(header.replace(BOLD, "").replace(RESET, "")))

    for ff in fillfactors:
        pr = patch_results.get(ff, {})
        mr = master_results.get(ff, {}) if master_results else {}

        heap_pages = pr.get("heap_pages", "?")

        off_times = pr.get("prefetch_off", {}).get("times", [])
        on_times = pr.get("prefetch_on", {}).get("times", [])
        m_times = mr.get("master", {}).get("times", []) if mr else []

        off_rep = get_rep(off_times) if off_times else None
        on_rep = get_rep(on_times) if on_times else None
        m_rep = get_rep(m_times) if m_times else None

        line = f"{ff:>4}  {heap_pages:>10}  "
        if has_master:
            line += f"{m_rep:>10.1f}  " if m_rep else f"{'N/A':>10}  "
        if has_off:
            line += f"{off_rep:>10.1f}  " if off_rep else f"{'N/A':>10}  "
        line += f"{on_rep:>10.1f}  " if on_rep else f"{'N/A':>10}  "

        if has_master and m_rep and on_rep:
            ratio = on_rep / m_rep
            line += f"{BOLD}{ratio:>10.3f}x{RESET}"
        elif has_master:
            line += f"{'N/A':>10}"

        if has_off and off_rep and on_rep:
            ratio_on_off = on_rep / off_rep
            line += f"{ratio_on_off:>8.3f}x"
        elif has_off:
            line += f"{'N/A':>8}"

        print(line)

    # Print EXPLAIN from best run for each fillfactor and config
    if has_master:
        print(f"\n{'=' * 80}")
        print("EXPLAIN OUTPUT (best run, master)")
        print(f"{'=' * 80}")
        for ff in fillfactors:
            mr = master_results.get(ff, {})
            m_data = mr.get("master", {})
            explain = m_data.get("best_explain")
            if explain:
                print(f"\n--- Fillfactor {ff} ---")
                for line in explain.split('\n'):
                    print(f"  {line}")

    print(f"\n{'=' * 80}")
    print("EXPLAIN OUTPUT (best run, prefetch=on)")
    print(f"{'=' * 80}")
    for ff in fillfactors:
        pr = patch_results.get(ff, {})
        on_data = pr.get("prefetch_on", {})
        explain = on_data.get("best_explain")
        if explain:
            print(f"\n--- Fillfactor {ff} ---")
            for line in explain.split('\n'):
                print(f"  {line}")


# --- Argument Parsing ---

def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Fillfactor benchmark for PostgreSQL index scan "
                    "prefetching.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Demonstrates how heap fillfactor affects prefetch performance under direct I/O.
Lower fillfactors (more heap pages) can paradoxically be faster because the
yield logic in the prefetch code destroys I/O concurrency when cache hit
ratios are high (fewer pages).

Examples:
    %(prog)s                              # Default: fillfactors 90,50,25
    %(prog)s --fillfactors 100,90,50,25,10
    %(prog)s --scale 200 --runs 5
    %(prog)s --no-master                  # Skip master benchmark
    %(prog)s --no-direct-io               # Use OS filesystem cache
        """
    )
    parser.add_argument("--fillfactors", type=str, default="90,50,25",
                        help="Comma-separated fillfactor values "
                             "(default: 90,50,25)")
    parser.add_argument("--scale", type=int, default=100,
                        help="pgbench scale factor (default: 100)")
    parser.add_argument("--runs", type=int, default=3,
                        help="Number of runs per configuration (default: 3)")
    parser.add_argument("--no-master", action="store_true",
                        dest="no_master",
                        help="Skip testing master (tested by default)")
    parser.add_argument("--no-tmpfs-hugepages", action="store_true",
                        help="Disable tmpfs with huge=always for binaries")
    parser.add_argument("--skip-init", action="store_true",
                        dest="skip_init",
                        help="Skip pgbench initialization entirely "
                             "(tables are auto-reused when they exist)")
    parser.add_argument("--median", action="store_true",
                        dest="use_median",
                        help="Use median instead of min as the "
                             "representative value")
    parser.add_argument("--shared-buffers", type=str, default="2GB",
                        dest="shared_buffers",
                        help="shared_buffers setting (default: 2GB)")
    parser.add_argument("--no-direct-io", action="store_true",
                        dest="no_direct_io",
                        help="Disable debug_io_direct=data; use OS "
                             "filesystem cache (clears OS cache each run)")
    parser.add_argument("--test-prefetch-disabled", action="store_true",
                        dest="test_prefetch_disabled",
                        help="Also test with enable_indexscan_prefetch=off "
                             "on the patch build")
    parser.add_argument("--delay-pgdata", action="store_true",
                        dest="delay_pgdata",
                        help="Use data directories with simulated I/O "
                             "delay (data-delay instead of data)")
    return parser.parse_args()


# --- Main ---

def main():
    args = parse_arguments()
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    fillfactors = [int(x.strip()) for x in args.fillfactors.split(",")]
    stat_label = "median" if args.use_median else "min"
    direct_io = not args.no_direct_io

    master_data_dir = MASTER_DATA_DIR
    patch_data_dir = PATCH_DATA_DIR
    if args.delay_pgdata:
        master_data_dir = "/mnt/nvme/postgresql/master/data-delay"
        patch_data_dir = "/mnt/nvme/postgresql/patch/data-delay"
        print(f"Using delayed I/O data directories:")
        print(f"  Master: {master_data_dir}")
        print(f"  Patch:  {patch_data_dir}\n")

    # Setup tmpfs
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

    patch_hash = get_git_hash(PATCH_SOURCE_DIR)
    test_master = not args.no_master
    master_hash = get_git_hash(MASTER_SOURCE_DIR) if test_master else None

    io_mode = "buffered I/O" if args.no_direct_io else "direct I/O"

    print("=" * 60)
    print("Fillfactor Benchmark")
    print("=" * 60)
    print(f"Fillfactors: {fillfactors}")
    print(f"Scale: {args.scale}")
    print(f"Runs per config: {args.runs}")
    print(f"Shared buffers: {args.shared_buffers}")
    print(f"I/O mode: {io_mode} (io_method=io_uring)")
    print(f"With master: {'yes' if test_master else 'no'}")
    print(f"Patch git hash: {patch_hash}")
    if master_hash:
        print(f"Master git hash: {master_hash}")
    print(f"Representative stat: {stat_label}")
    print("=" * 60)

    master_results = None
    patch_results = {}

    try:
        for ff in fillfactors:
            print(f"\n{'=' * 60}")
            print(f"Fillfactor {ff}")
            print(f"{'=' * 60}")

            # --- Load phase: init data on each branch, then stop ---
            # Tables are renamed to pgbench_accounts_ff{N} so multiple
            # fillfactors can coexist. Loading is auto-skipped when the
            # target table already exists.
            if not args.skip_init:
                if test_master:
                    load_fillfactor_data(
                        master_bin, "master", master_data_dir, MASTER_CONN,
                        ff, args.scale, args.shared_buffers, direct_io)

                sizes = load_fillfactor_data(
                    patch_bin, "patch", patch_data_dir, PATCH_CONN,
                    ff, args.scale, args.shared_buffers, direct_io)

                patch_results[ff] = {
                    "heap_pages": sizes["heap_pages"],
                    "index_pages": sizes["index_pages"],
                }
            else:
                print("  Skipping data loading (--skip-init)")
                patch_results[ff] = {
                    "heap_pages": None, "index_pages": None,
                }

            # --- Benchmark phase: start fresh servers and run ---
            if test_master:
                print(f"\n  --- MASTER benchmark ---")
                if master_results is None:
                    master_results = {}
                master_ff = run_benchmark_for_fillfactor(
                    master_bin, "master", master_data_dir, MASTER_CONN,
                    fillfactor=ff, is_master=True, num_runs=args.runs,
                    shared_buffers=args.shared_buffers, direct_io=direct_io)
                master_results[ff] = {
                    **patch_results[ff],
                    **master_ff,
                }

            print(f"\n  --- PATCH benchmark ---")
            patch_ff = run_benchmark_for_fillfactor(
                patch_bin, "patch", patch_data_dir, PATCH_CONN,
                fillfactor=ff, is_master=False, num_runs=args.runs,
                shared_buffers=args.shared_buffers, direct_io=direct_io,
                test_prefetch_disabled=args.test_prefetch_disabled)
            patch_results[ff].update(patch_ff)

        # Print summary
        print_summary(patch_results, master_results, fillfactors, stat_label)

        # Save results to JSON
        json_results = {
            "timestamp": datetime.now().isoformat(),
            "patch_hash": patch_hash,
            "master_hash": master_hash,
            "fillfactors": fillfactors,
            "scale": args.scale,
            "runs": args.runs,
            "shared_buffers": args.shared_buffers,
            "direct_io": direct_io,
            "stat_label": stat_label,
            "query_template": "EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM pgbench_accounts_ff{N} ORDER BY aid",
            "patch": {
                str(ff): _serialize_ff_results(patch_results[ff])
                for ff in fillfactors
            },
        }
        if master_results:
            json_results["master"] = {
                str(ff): _serialize_ff_results(master_results[ff])
                for ff in fillfactors
            }

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        json_file = os.path.join(OUTPUT_DIR,
                                 f"fillfactor_{timestamp}.json")
        with open(json_file, "w") as f:
            json.dump(json_results, f, indent=2)
        print(f"\nResults saved to: {json_file}")

    finally:
        stop_server(master_bin, master_data_dir)
        stop_server(patch_bin, patch_data_dir)
        if tmpfs_mount:
            cleanup_tmpfs(tmpfs_mount)


def _serialize_ff_results(ff_data):
    """Prepare fillfactor results for JSON serialization."""
    result = {
        "heap_pages": ff_data.get("heap_pages"),
        "index_pages": ff_data.get("index_pages"),
    }
    for config in ["master", "prefetch_off", "prefetch_on"]:
        if config in ff_data:
            cfg = ff_data[config].copy()
            # Keep only best_explain, drop per-run explains list
            cfg.pop("explains", None)
            result[config] = cfg
    return result


if __name__ == "__main__":
    main()
