#!/usr/bin/env python3

"""
Performance profiling script for PostgreSQL using perf and FlameGraph.

This script automates the process of profiling two different versions of
PostgreSQL (e.g., 'master' and a 'patched' version) to generate
differential flame graphs for performance analysis of a specific SQL query.

It performs the following steps:
1.  Checks for necessary command-line tool dependencies (`perf`, `git`).
2.  Ensures the FlameGraph repository is available.
3.  For each PostgreSQL version ('master' and 'patch'):
    a. Starts the PostgreSQL server.
    b. Waits for the server to become available.
    c. Connects to the database and retrieves the backend process PID.
    d. Prewarms database caches.
    e. Starts `perf record` to sample the backend process.
    f. Executes a specified SQL query repeatedly.
    g. Stops `perf` and the PostgreSQL server.
    h. Generates a normalized stack trace file.
4.  Uses the FlameGraph toolkit to:
    a. Fold the stack traces from both profiling runs.
    b. Generate individual flame graphs for both versions.
    c. Generate a differential flame graph comparing the two versions.

Configuration:
The script requires configuration of paths, connection details, and profiling
parameters in the "--- Configuration ---" section of the script. This includes:
- Paths to PostgreSQL binaries and data directories for both versions.
- Connection details.
- The SQL query to be profiled.
- `perf` sampling frequency.
- Path to the FlameGraph repository.

Output:
The script creates an 'output_perf_flamegraph' directory (by default)
containing:
- Log files for each PostgreSQL server instance.
- Raw `perf.data` files (named 'master' and 'patch').
- Collapsed stack files (`.stacks` and `.folded`).
- Individual SVG flame graphs for each version.
- A differential SVG flame graph (`diff.svg`).
"""

import argparse
from collections import OrderedDict
import os
import random
import shutil
import signal
import subprocess
import sys
import time

import psycopg

from benchmark_common import (
    MASTER_BIN, PATCH_BIN,
    MASTER_DATA_DIR, PATCH_DATA_DIR,
    MASTER_CONN, PATCH_CONN,
    BASELINE_CONFIGS, TESTBRANCH_CONFIGS, BUILD_HAS_PREFETCH,
    clear_os_cache, evict_relations, prewarm_relations,
    set_gucs, reset_gucs,
    setup_tmpfs_hugepages, copy_binaries_to_tmpfs, cleanup_tmpfs,
    extract_execution_time,
)

from prefetch_benchmark import BENCHMARK_SUITES

# Build combined query lookup and per-query data functions from
# BENCHMARK_SUITES (the single source of truth shared with
# prefetch_benchmark.py and patch_report.py).
ALL_QUERIES = OrderedDict()
_QUERY_DATA_FNS = {}   # query_id -> (verify_fn, load_fn)

for _suite in BENCHMARK_SUITES:
    _verify = _suite["verify_fn"]
    _load = _suite["load_fn"]
    for _qid, _qdef in _suite["queries"].items():
        ALL_QUERIES[_qid] = _qdef
        _QUERY_DATA_FNS[_qid] = (_verify, _load)

def _get_data_functions(query_id):
    """Return (verify_fn, load_fn) for the given query ID."""
    return _QUERY_DATA_FNS[query_id]

# os.environ["MALLOPT_TOP_PAD_"] = str(64 * 1024 * 1024)
# os.environ["MALLOPT_TOP_PAD"] = str(64 * 1024 * 1024)
# os.environ["M_TOP_PAD"] = str(64 * 1024 * 1024)
# os.environ["M_MMAP_THRESHOLD"] = str(64 * 1024 * 1024)
# os.environ["M_TRIM_THRESHOLD"] = str(64 * 1024 * 1024)
# os.environ["M_MMAP_MAX"] = str(0)
# os.environ["M_ARENA_TEST"] = str(64)

# --- Configuration ---

# Use MASTER_CONN and PATCH_CONN from benchmark_common, aliased for compatibility.
# These are overridden in main() when --baseline is not "master".
MASTER_CONN_DETAILS = MASTER_CONN
PATCH_CONN_DETAILS = PATCH_CONN

# The frequency of 'perf' sampling.
PERF_FREQUENCY=9999

# --- Script variables ---
FLAMEGRAPH_DIR="/home/pg/code/FlameGraph"
OUTPUT_DIR = "output_perf_flamegraph"

# --- Benchmark configurations ---
BENCHMARKS = {
    "nestloop": {
        "sql_query": "select count(*) from pgbench_accounts a join pgbench_branches b on a.bid = b.bid",
        "query_repetitions": 5,
    },
    "simple_select": {
        "sql_query": "select * from pgbench_accounts where aid = %s::int4",
        "query_repetitions": 500_000,
        "max_aid_val": 100_000,
    },
    "bitmap": {
        "sql_query": "SELECT count(abalance) FROM pgbench_accounts WHERE aid BETWEEN %s AND %s",
        "query_repetitions": 500_000,
        "bitmap_range": 2000,
    },
}

def parse_arguments():
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description="Performance profiling script for PostgreSQL using perf and FlameGraph.",
        allow_abbrev=False
    )
    parser.add_argument(
        "--pgbench-scale",
        type=int,
        default=10,
        help="pgbench scale factor for initialization (default: 10)"
    )
    parser.add_argument(
        "--skip-pgbench-init", "--skip",
        action="store_true",
        dest="skip_pgbench_init",
        help="Skip pgbench initialization"
    )
    parser.add_argument(
        "--perf",
        nargs="?",
        const=True,
        default=False,
        metavar="EVENT",
        help="Run perf profiling (disabled by default). Optionally specify event name (e.g., --perf cycles, --perf cache-misses)"
    )
    parser.add_argument(
        "--perfstat",
        action="store_true",
        help="Run perf stat instead of perf record (mutually exclusive with --perf)"
    )
    parser.add_argument(
        "--benchmark",
        choices=["nestloop", "simple_select", "bitmap"],
        default="nestloop",
        help="Benchmark to run (default: nestloop)"
    )
    parser.add_argument(
        "--patch-first",
        action="store_true",
        help="Profile patch version before master (instead of after)"
    )
    parser.add_argument(
        "--highfreq",
        action="store_true",
        help="Use high sampling frequency for perf (49999 Hz instead of 9999 Hz)"
    )
    parser.add_argument(
        "--queries",
        type=int,
        dest="num_queries",
        help="Number of queries to execute (only valid with --benchmark simple_select)"
    )
    parser.add_argument(
        "--bitmap",
        action="store_const",
        const="bitmap",
        dest="benchmark",
        help="Shorthand for --benchmark bitmap (bitmap index scan profiling)"
    )
    parser.add_argument(
        "--bitmap-range",
        type=int,
        default=2000,
        help="Range size for bitmap benchmark (default: 2000 contiguous items)"
    )
    parser.add_argument(
        "--ios",
        action="store_true",
        dest="index_only_scan",
        help="Force index-only scans instead of plain index scans"
    )
    parser.add_argument(
        "--hash",
        action="store_true",
        dest="use_hash_index",
        help="Use a hash index instead of the default B-tree index"
    )
    parser.add_argument(
        "--no-tmpfs-hugepages",
        action="store_true",
        help="Disable tmpfs with huge=always (enabled by default)"
    )
    parser.add_argument(
        "--discard-runs",
        type=int,
        default=3,
        help="Number of initial query runs to discard before starting measurement (default: 3)"
    )
    parser.add_argument(
        "--benchmark-cpu",
        type=int,
        default=14,
        help="CPU core to pin PostgreSQL backend to (default: 14, a performance core)"
    )
    parser.add_argument(
        "--perf-cpu",
        type=int,
        default=15,
        help="CPU core to pin perf process to (default: 15)"
    )
    parser.add_argument(
        "--disable-prefetch",
        action="store_true",
        help="Disable hardware prefetchers on benchmark CPU during test (requires passwordless sudo for wrmsr)"
    )
    parser.add_argument(
        "--query",
        type=str,
        default=None,
        help="Run a prefetch benchmark query (e.g., Q1, A8). Mutually exclusive with --benchmark."
    )
    parser.add_argument(
        "--cached",
        action="store_true",
        help="Run in cached mode (prewarm all relations). Only applicable with --query."
    )
    parser.add_argument(
        "--skip-data-load",
        action="store_true",
        help="Skip prefetch benchmark data verification/loading (use with --query)"
    )
    parser.add_argument(
        "--no-pin",
        action="store_true",
        dest="no_pin",
        help="Disable CPU pinning (no taskset, no chrt, no perf -C; uses perf -p PID instead)"
    )
    prefetch_group = parser.add_mutually_exclusive_group()
    prefetch_group.add_argument(
        "--prefetch-only",
        action="store_true",
        dest="prefetch_only",
        help="Set enable_indexscan_prefetch=on on patch (use with --query)"
    )
    prefetch_group.add_argument(
        "--prefetch-disabled",
        action="store_true",
        dest="prefetch_disabled",
        help="Set enable_indexscan_prefetch=off on patch (use with --query)"
    )
    parser.add_argument(
        "--baseline",
        type=str,
        choices=list(BASELINE_CONFIGS.keys()),
        default="master",
        help="Baseline PostgreSQL build to compare against (default: master)"
    )
    parser.add_argument(
        "--testbranch",
        type=str,
        choices=list(TESTBRANCH_CONFIGS.keys()),
        default="patch",
        help="PostgreSQL build to test (default: patch). Use 'master' to compare master vs rel18."
    )
    return parser.parse_args()

def check_dependencies():
    """Check if required tools are available."""
    for cmd in ["perf", "git"]:
        if not shutil.which(cmd):
            print(f"Error: Command '{cmd}' not found. Please install it.")
            sys.exit(1)
    if not os.path.isdir(FLAMEGRAPH_DIR):
        raise FileNotFoundError("no FlameGraph repository")

def disable_prefetchers(cpu_core):
    """
    Disable hardware prefetchers on the specified CPU core.
    Returns the original MSR value so it can be restored later.

    For AMD Zen CPUs, MSR 0xC0011022 [DC Configuration Register] controls prefetchers:
    - Bit 13 (0x2000): DisHwPf - Disable hardware data prefetcher
    - Bit 15 (0x8000): DisWcPf - Disable prefetcher for Write Combining stores

    We set bit 13 to disable the main hardware prefetcher.

    Requires passwordless sudo for wrmsr/rdmsr.
    """
    MSR_DC_CFG = "0xC0011022"
    DISABLE_HW_PREFETCH_BIT = 0x2000  # Bit 13

    try:
        # Read current value
        result = subprocess.run(
            ["sudo", "rdmsr", "-p", str(cpu_core), MSR_DC_CFG],
            capture_output=True,
            text=True,
            check=False
        )

        if result.returncode != 0:
            print(f"Warning: Could not read prefetcher MSR: {result.stderr}")
            return None

        original_value_str = result.stdout.strip()
        original_value = int(original_value_str, 16)
        print(f"Current DC_CFG MSR value on CPU {cpu_core}: 0x{original_value:x}")

        # Check if hardware prefetcher is currently enabled
        if original_value & DISABLE_HW_PREFETCH_BIT:
            print(f"  -> Hardware prefetcher is already DISABLED (bit 13 is set)")
        else:
            print(f"  -> Hardware prefetcher is currently ENABLED (bit 13 is clear)")

        # Set bit 13 to disable hardware prefetcher
        new_value = original_value | DISABLE_HW_PREFETCH_BIT

        result = subprocess.run(
            ["sudo", "wrmsr", "-p", str(cpu_core), MSR_DC_CFG, f"0x{new_value:x}"],
            capture_output=True,
            text=True,
            check=False
        )

        if result.returncode != 0:
            print(f"Warning: Could not disable prefetchers: {result.stderr}")
            return None

        # Verify the change
        result = subprocess.run(
            ["sudo", "rdmsr", "-p", str(cpu_core), MSR_DC_CFG],
            capture_output=True,
            text=True,
            check=False
        )

        if result.returncode == 0:
            verify_value = int(result.stdout.strip(), 16)
            print(f"Hardware prefetcher disabled on CPU {cpu_core}. New MSR value: 0x{verify_value:x}")
            if verify_value & DISABLE_HW_PREFETCH_BIT:
                print(f"  -> Verified: Bit 13 is now SET (prefetcher disabled)")
            else:
                print(f"  -> Warning: Bit 13 is still CLEAR (prefetcher may still be enabled!)")

        return original_value_str

    except Exception as e:
        print(f"Warning: Failed to disable prefetchers: {e}")
        return None

def restore_prefetchers(cpu_core, original_value):
    """Restore hardware prefetchers to their original state."""
    if original_value is None:
        return

    MSR_HWCR = "0xC0011022"

    try:
        result = subprocess.run(
            ["sudo", "wrmsr", "-p", str(cpu_core), MSR_HWCR, f"0x{original_value}"],
            capture_output=True,
            text=True,
            check=False
        )

        if result.returncode == 0:
            print(f"Restored prefetcher MSR on CPU {cpu_core} to original value: 0x{original_value}")
        else:
            print(f"Warning: Could not restore prefetchers: {result.stderr}")

    except Exception as e:
        print(f"Warning: Failed to restore prefetchers: {e}")

# setup_tmpfs_hugepages, copy_binaries_to_tmpfs, cleanup_tmpfs
# are imported from benchmark_common


def start_server(pg_bin_dir, pg_name, pg_data_dir, conn_details):
    """Start a PostgreSQL server and wait for it to be ready."""
    pg_ctl_path = os.path.join(pg_bin_dir, "pg_ctl")
    log_file = os.path.join(OUTPUT_DIR, f"{pg_name}.postgres_log")

    # Ensure server is stopped before we start
    if subprocess.run([pg_ctl_path, "status", "-D", pg_data_dir], check=False).returncode == 0:
        print(f"{pg_name}: Server is already running. Stopping it...")
        subprocess.run([pg_ctl_path, "stop", "-D", pg_data_dir, "-m", "fast"], check=True)
        time.sleep(3)

    # Start the server
    print(f"Starting {pg_name} PostgreSQL server (port {conn_details.get('port', 'default')})...")
    start_options = f"-p {conn_details['port']}" if 'port' in conn_details else ""

    result = subprocess.run(
        [pg_ctl_path, "start",
         "-o", "--autovacuum=off",
         "-D", pg_data_dir,
         "-l", log_file, "-o",
         start_options],
        capture_output=True,
        text=True
    )

    if result.returncode != 0:
        print(f"Error: Failed to start {pg_name} server")
        print(f"stdout: {result.stdout}")
        print(f"stderr: {result.stderr}")
        print(f"Check log file: {log_file}")
        # Try to read the actual server log if it exists
        try:
            with open(log_file, 'r') as f:
                print(f"Log file contents:\n{f.read()}")
        except FileNotFoundError:
            pass
        sys.exit(1)

    # Wait for the server to be ready
    print(f"Waiting for {pg_name} server to accept connections...")
    for attempt in range(15):
        try:
            conn = psycopg.connect(**conn_details, connect_timeout=2)
            conn.close()
            print(f"{pg_name} server started successfully.")
            return
        except (psycopg.OperationalError, psycopg.DatabaseError) as e:
            if attempt == 14:
                print(f"Error: {pg_name} server failed to start or become available after 15 attempts")
                print(f"Last connection error: {e}")
                sys.exit(1)
            if attempt < 5:
                time.sleep(0.5)
            else:
                time.sleep(1)

def stop_server(pg_bin_dir, pg_data_dir):
    """Stop a PostgreSQL server (gracefully handles if not running)."""
    pg_ctl_path = os.path.join(pg_bin_dir, "pg_ctl")
    print("Stopping PostgreSQL server...")
    # Don't fail if server is not running
    subprocess.run([pg_ctl_path, "stop", "-D", pg_data_dir, "-m", "fast"], check=False)

def init_pgbench(pg_bin_dir, conn_details, scale, use_hash_index=False):
    """Initialize pgbench database with specified scale factor."""
    print(f"Initializing pgbench with scale factor {scale}...")
    pgbench_path = os.path.join(pg_bin_dir, "pgbench")

    subprocess.run(
        [pgbench_path, "-i", "-s", str(scale),
         f"--host={conn_details['host']}",
         f"--port={conn_details['port']}",
         f"--user={conn_details['user']}",
         conn_details["dbname"]],
        check=True
    )
    print(f"pgbench initialization completed.")

    if use_hash_index:
        print("Creating hash indexes on pgbench_accounts.aid and pgbench_branches.bid...")
        try:
            conn = psycopg.connect(**conn_details, prepare_threshold=0)
            with conn.cursor() as cursor:
                # Drop the primary key constraint on pgbench_accounts (which uses the B-tree index)
                cursor.execute("ALTER TABLE pgbench_accounts DROP CONSTRAINT pgbench_accounts_pkey;")
                # Create hash index on pgbench_accounts.aid
                cursor.execute("CREATE INDEX pgbench_accounts_aid_hash ON pgbench_accounts USING hash (aid);")
                # Drop the primary key constraint on pgbench_branches
                cursor.execute("ALTER TABLE pgbench_branches DROP CONSTRAINT pgbench_branches_pkey;")
                # Create hash index on pgbench_branches.bid
                cursor.execute("CREATE INDEX pgbench_branches_bid_hash ON pgbench_branches USING hash (bid);")
            conn.commit()
            conn.close()
            print("Hash indexes created successfully.")
        except Exception as e:
            print(f"Error creating hash indexes: {e}")
            sys.exit(1)

def get_pgbench_row_count(pg_name, conn_details):
    """Get the actual number of rows in pgbench_accounts table."""
    try:
        conn = psycopg.connect(**conn_details, prepare_threshold=0)
        with conn.cursor() as cursor:
            cursor.execute("select count(*) from pgbench_accounts")
            count = cursor.fetchone()[0]
        conn.close()
        print(f"{pg_name}: pgbench_accounts has {count} rows")
        return count
    except Exception as e:
        print(f"Error getting row count from {pg_name}: {e}")
        sys.exit(1)

def verify_hash_index_exists(pg_name, conn_details):
    """Verify that hash indexes exist on pgbench_accounts.aid and pgbench_branches.bid."""
    try:
        conn = psycopg.connect(**conn_details, prepare_threshold=0)
        with conn.cursor() as cursor:
            # Check for hash index on pgbench_accounts.aid
            cursor.execute("""
                SELECT indexname FROM pg_indexes
                WHERE tablename = 'pgbench_accounts'
                  AND indexdef LIKE '%USING hash%'
            """)
            accounts_result = cursor.fetchone()

            # Check for hash index on pgbench_branches.bid
            cursor.execute("""
                SELECT indexname FROM pg_indexes
                WHERE tablename = 'pgbench_branches'
                  AND indexdef LIKE '%USING hash%'
            """)
            branches_result = cursor.fetchone()
        conn.close()

        if accounts_result is None:
            print(f"Error: {pg_name}: No hash index found on pgbench_accounts")
            print(f"When using --hash with --skip, hash indexes must be present.")
            sys.exit(1)

        if branches_result is None:
            print(f"Error: {pg_name}: No hash index found on pgbench_branches")
            print(f"When using --hash with --skip, hash indexes must be present.")
            sys.exit(1)

        print(f"{pg_name}: Verified hash indexes exist: {accounts_result[0]}, {branches_result[0]}")
        return True
    except Exception as e:
        print(f"Error verifying hash indexes on {pg_name}: {e}")
        sys.exit(1)

def verify_btree_index_exists(pg_name, conn_details):
    """Verify that B-tree indexes exist on pgbench_accounts and pgbench_branches when --hash is not used."""
    try:
        conn = psycopg.connect(**conn_details, prepare_threshold=0)
        with conn.cursor() as cursor:
            # Check for B-tree index on pgbench_accounts (or no index spec, which defaults to B-tree)
            cursor.execute("""
                SELECT indexname FROM pg_indexes
                WHERE tablename = 'pgbench_accounts'
                  AND indexdef NOT LIKE '%USING hash%'
            """)
            accounts_result = cursor.fetchone()

            # Check for B-tree index on pgbench_branches
            cursor.execute("""
                SELECT indexname FROM pg_indexes
                WHERE tablename = 'pgbench_branches'
                  AND indexdef NOT LIKE '%USING hash%'
            """)
            branches_result = cursor.fetchone()
        conn.close()

        if accounts_result is None:
            print(f"Error: {pg_name}: No B-tree index found on pgbench_accounts")
            print(f"When not using --hash with --skip, B-tree indexes must be present.")
            sys.exit(1)

        if branches_result is None:
            print(f"Error: {pg_name}: No B-tree index found on pgbench_branches")
            print(f"When not using --hash with --skip, B-tree indexes must be present.")
            sys.exit(1)

        print(f"{pg_name}: Verified B-tree indexes exist: {accounts_result[0]}, {branches_result[0]}")
        return True
    except Exception as e:
        print(f"Error verifying B-tree indexes on {pg_name}: {e}")
        sys.exit(1)

def profile_postgres(pg_bin_dir, pg_name, conn_details, output_file, sql_query, query_repetitions, run_perf, max_aid_val=None, perf_event=None, highfreq=False, index_only_scan=False, run_perfstat=False, discard_runs=0, benchmark_cpu=14, perf_cpu=15, disable_prefetch=False, is_master=False, prefetch_setting=None, no_pin=False, use_bitmap=False, bitmap_range=2000):
    """Profiles a PostgreSQL instance (assumes server is already running)."""
    print(f"--- Testing {pg_name} ---")

    conn = None
    try:
        # Connect to the database to get the backend PID
        conn = psycopg.connect(**conn_details, prepare_threshold=0)

        backend_pid = conn.info.backend_pid
        print(f"Successfully connected. Backend PID is: {backend_pid}")

        # Pin the backend process to a specific core and use RT scheduling to prevent migration.
        if not no_pin:
            try:
                # Pin backend to specified core using taskset (doesn't require root)
                result = subprocess.run(
                    ["taskset", "-cp", str(benchmark_cpu), str(backend_pid)],
                    capture_output=True,
                    text=True,
                    check=False
                )
                if result.returncode == 0:
                    print(f"Pinned backend PID {backend_pid} to CPU {benchmark_cpu}.")
                else:
                    print(f"Warning: Could not set CPU affinity: {result.stderr}")

                # Set RT scheduling (SCHED_FIFO) to truly pin and prevent migrations
                # Use chrt with sudo for RT scheduling (requires passwordless sudo or password entry)
                result = subprocess.run(
                    ["sudo", "chrt", "-f", "-p", "1", str(backend_pid)],
                    capture_output=True,
                    text=True,
                    check=False
                )
                if result.returncode == 0:
                    print(f"Set backend PID {backend_pid} to SCHED_FIFO priority 1 (prevents CPU migration).")

                    # Verify RT scheduling was actually set
                    verify_result = subprocess.run(
                        ["chrt", "-p", str(backend_pid)],
                        capture_output=True,
                        text=True,
                        check=False
                    )
                    if verify_result.returncode == 0:
                        print(f"Verification: {verify_result.stdout.strip()}")
                    else:
                        print(f"Warning: Could not verify RT scheduling")
                else:
                    print(f"Warning: Could not set RT scheduling: {result.stderr}")
                    print(f"         Configure passwordless sudo for 'chrt' or run entire script with sudo.")
            except Exception as e:
                print(f"Warning: Could not set CPU affinity/RT scheduling for backend PID {backend_pid}: {e}")
        else:
            print("CPU pinning disabled (--no-pin)")

        # Disable hardware prefetchers if requested
        original_prefetch_value = None
        if disable_prefetch:
            print(f"\nDisabling hardware prefetchers on CPU {benchmark_cpu}...")
            original_prefetch_value = disable_prefetchers(benchmark_cpu)
            if original_prefetch_value is None:
                print("If you don't have passwordless sudo configured, you can manually:")
                print(f"  1. Read current value: sudo rdmsr -p {benchmark_cpu} 0xC0011022")
                print(f"  2. Set bit 13 to disable: sudo wrmsr -p {benchmark_cpu} 0xC0011022 <original_value | 0x2000>")
                print(f"  3. After benchmark, restore: sudo wrmsr -p {benchmark_cpu} 0xC0011022 <original_value>")

        print("Prewarming...")
        with conn.cursor() as cursor:
            # if pg_name == "patch":
            #     cursor.execute("set enable_indexscan_prefetch=off;")
            if use_bitmap:
                cursor.execute("set enable_bitmapscan=on;")
                cursor.execute("set enable_indexscan=off;")
                cursor.execute("set enable_indexonlyscan=off;")
            else:
                cursor.execute("set enable_bitmapscan=off;")
                if index_only_scan:
                    cursor.execute("set enable_indexonlyscan=on;")
                else:
                    cursor.execute("set enable_indexonlyscan=off;")
            cursor.execute("set enable_hashjoin=off;")
            cursor.execute("set enable_material=off;")
            cursor.execute("set enable_memoize=off;")
            cursor.execute("set enable_mergejoin=off;")
            cursor.execute("set enable_seqscan=off;")

            cursor.execute("set max_parallel_workers_per_gather = 0;")
            # Set prefetch GUC only on patch (not master)
            if not is_master and prefetch_setting is not None:
                cursor.execute(f"set enable_indexscan_prefetch = {prefetch_setting};")
            cursor.execute("create extension if not exists pg_prewarm;")
            cursor.execute("select pg_prewarm('pgbench_accounts');")
            # Prewarm all indexes on pgbench_accounts
            cursor.execute("""
                SELECT indexname FROM pg_indexes
                WHERE tablename = 'pgbench_accounts'
            """)
            for (index_name,) in cursor.fetchall():
                cursor.execute(f"select pg_prewarm('{index_name}');")
            cursor.execute("select pg_prewarm('pgbench_branches');")
            # Prewarm all indexes on pgbench_branches
            cursor.execute("""
                SELECT indexname FROM pg_indexes
                WHERE tablename = 'pgbench_branches'
            """)
            for (index_name,) in cursor.fetchall():
                cursor.execute(f"select pg_prewarm('{index_name}');")
        print("Finished prewarming")

        # Show EXPLAIN plan before starting the benchmark
        print("\n--- Query Plan (EXPLAIN) ---")
        with conn.cursor() as cursor: # type: ignore
            explain_query = f"EXPLAIN {sql_query}"
            if use_bitmap and max_aid_val is not None:
                aid = random.randint(1, max_aid_val - bitmap_range)
                cursor.execute(explain_query, params=[aid, aid + bitmap_range - 1], prepare=False)
            elif max_aid_val is not None:
                cursor.execute(explain_query, params=[random.randint(1, max_aid_val)], prepare=False)
            else:
                cursor.execute(explain_query, prepare=False)
            for row in cursor.fetchall():
                print(row[0])
        print("--- End Query Plan ---\n")

        # Execute the query repeatedly in the same connection
        total_runs = query_repetitions + discard_runs
        if discard_runs > 0:
            print(f"Executing the SQL query {total_runs} times (discarding first {discard_runs} runs)...")
        else:
            print(f"Executing the SQL query {query_repetitions} times...")
        random.seed(42)

        # Execute discard runs before starting measurement
        if discard_runs > 0:
            print(f"Warming up: executing {discard_runs} discard runs...")
            with conn.cursor() as cursor:  # type: ignore
                for i in range(discard_runs):
                    if use_bitmap and max_aid_val is not None:
                        aid = random.randint(1, max_aid_val - bitmap_range)
                        cursor.execute(query=sql_query,
                                       params=[aid, aid + bitmap_range - 1],
                                       prepare=True)
                    elif max_aid_val is not None:
                        cursor.execute(query=sql_query,
                                       params=[random.randint(1, max_aid_val)],
                                       prepare=True)
                    else:
                        cursor.execute(query=sql_query, prepare=True)
            print(f"Warmup complete. Starting measurement...")

        # Start perf profiling just before the measured query loop
        perf_process = None
        perf_command = []
        start_time = time.time()
        if run_perf:
            perf_freq = "49999" if highfreq else str(PERF_FREQUENCY)
            perf_command = [
                "perf", "record",
                "-F", perf_freq,
            ]
            # Only add -e flag if a specific perf event was requested
            if perf_event:
                perf_command.extend(["-e", perf_event])

            if no_pin:
                print(f"Starting perf on PID {backend_pid}...")
                perf_command.extend([
                    "-p", str(backend_pid),
                    "-g",
                    "-o",  OUTPUT_DIR + "/" + pg_name,
                ])
            else:
                print(f"Starting perf on CPU core {benchmark_cpu}...")
                perf_command.extend([
                    "-C", str(benchmark_cpu),  # Monitor CPU core (works with all event types)
                    "-g",
                    "-o",  OUTPUT_DIR + "/" + pg_name,
                ])
            perf_process = subprocess.Popen(perf_command)

            # Pin perf process to a different core to minimize interference.
            if not no_pin:
                try:
                    os.sched_setaffinity(perf_process.pid, {perf_cpu})
                    print(f"Pinned perf PID {perf_process.pid} to CPU {perf_cpu}.")
                except (AttributeError, PermissionError, OSError) as e:
                    print(f"Warning: Could not set CPU affinity for perf PID {perf_process.pid}: {e}")

            # Give perf a moment to initialize before starting the workload
            time.sleep(1)
        elif run_perfstat:
            stat_output_file = OUTPUT_DIR + "/" + pg_name + "_perfstat.txt"
            if no_pin:
                print(f"Starting perf stat on PID {backend_pid}...")
                perf_command = [
                    "perf", "stat",
                    "-ddd",                     # Very detailed statistics
                    "-p", str(backend_pid),     # Monitor backend PID
                    "-o", stat_output_file,     # Output to file
                ]
            else:
                print(f"Starting perf stat on CPU core {benchmark_cpu}...")
                perf_command = [
                    "perf", "stat",
                    "-ddd",                     # Very detailed statistics
                    "-C", str(benchmark_cpu),   # Monitor CPU core where backend is RT-pinned
                    "-o", stat_output_file,     # Output to file
                ]
            perf_process = subprocess.Popen(perf_command)

            # Pin perf stat process to different core to minimize interference
            if not no_pin:
                try:
                    os.sched_setaffinity(perf_process.pid, {perf_cpu})
                    print(f"Pinned perf stat PID {perf_process.pid} to CPU {perf_cpu}.")
                except (AttributeError, PermissionError, OSError) as e:
                    print(f"Warning: Could not set CPU affinity for perf stat PID {perf_process.pid}: {e}")

            # Give perf stat a moment to initialize
            time.sleep(1)

        with conn.cursor() as cursor: # type: ignore
            # Show per-query timing for small query counts (nestloop benchmark)
            show_per_query_timing = query_repetitions <= 10

            for i in range(query_repetitions):
                query_start = time.time()
                if use_bitmap and max_aid_val is not None:
                    aid = random.randint(1, max_aid_val - bitmap_range)
                    cursor.execute(query=sql_query,
                                   params=[aid, aid + bitmap_range - 1],
                                   prepare=True)
                elif max_aid_val is not None:
                    cursor.execute(query=sql_query,
                                   params=[random.randint(1, max_aid_val)],
                                   prepare=True)
                else:
                    cursor.execute(query=sql_query, prepare=True)

                if show_per_query_timing:
                    query_time = time.time() - query_start
                    print(f"  Query {i+1}/{query_repetitions}: {query_time:.3f} seconds")

        end_time = time.time()
        total_time = end_time - start_time
        print(f"Query loop finished in \033[1m{total_time:.3f} seconds\033[0m")

        if perf_process:
            # Stop the perf process gracefully by sending SIGINT (like Ctrl+C)
            if run_perf:
                print("Stopping perf...")
            else:
                print("Stopping perf stat...")
            perf_process.send_signal(signal.SIGINT)

            # Wait for perf to terminate
            perf_process.wait()

            if run_perf:
                # Generate the stack trace file, normalizing the binary path
                print(f"Generating and normalizing stack trace file: {output_file}")

                # The sed expression will replace the full, version-specific path
                # with the generic name 'postgres', allowing difffolded.pl to match symbols.
                pg_executable_path = os.path.join(pg_bin_dir, "postgres")
                sed_expression = f"s|{pg_executable_path}|postgres|g"

                perf_script_process = subprocess.Popen(["perf", "script",
                                                        "-i",  OUTPUT_DIR + "/" + pg_name,
                                                        ], stdout=subprocess.PIPE)

                with open(output_file, "w") as f:
                    sed_process = subprocess.run(
                        ["sed", sed_expression],
                        stdin=perf_script_process.stdout,
                        stdout=f,
                        check=True
                    )
                perf_script_process.stdout.close()
                perf_script_process.wait()
            elif run_perfstat:
                # Don't print here - will be displayed side-by-side in main()
                pass

    finally:
        # Restore hardware prefetchers if they were disabled
        if disable_prefetch and original_prefetch_value is not None:
            print(f"\nRestoring hardware prefetchers on CPU {benchmark_cpu}...")
            restore_prefetchers(benchmark_cpu, original_prefetch_value)

        # Ensure the connection is closed
        if conn:
            conn.close()
        print("----------------------------------------")

    # Return string of perf command for flamegraph --subtitle arg
    return ' '.join(perf_command), total_time

def display_perfstat_comparison(master_stat_file, patch_stat_file):
    """Display perf stat results for master and patch side by side with ratio column."""
    import re

    print("\n" + "="*120)
    print("PERF STAT COMPARISON: MASTER vs PATCH")
    print("="*120 + "\n")

    try:
        with open(master_stat_file, "r") as f_master:
            master_lines = f_master.readlines()
    except FileNotFoundError:
        print(f"Error: Could not find master perf stat file: {master_stat_file}")
        return

    try:
        with open(patch_stat_file, "r") as f_patch:
            patch_lines = f_patch.readlines()
    except FileNotFoundError:
        print(f"Error: Could not find patch perf stat file: {patch_stat_file}")
        return

    # Parse metric values from lines
    # Format: "     26,138,231,122      instructions                     #    3.00  insn per cycle"
    # We want to extract the number and the metric name
    metric_pattern = re.compile(r'^\s*([\d,]+)\s+([a-zA-Z0-9-]+)')
    time_pattern = re.compile(r'^\s*([\d.]+)\s+seconds time elapsed')

    # Build dictionaries of metric_name -> value
    master_metrics = {}
    patch_metrics = {}

    for line in master_lines:
        # Try metric pattern
        match = metric_pattern.match(line)
        if match:
            value_str = match.group(1).replace(',', '')
            metric_name = match.group(2)
            master_metrics[metric_name] = float(value_str)
        else:
            # Try time elapsed pattern
            match = time_pattern.match(line)
            if match:
                master_metrics['seconds-time-elapsed'] = float(match.group(1))

    for line in patch_lines:
        # Try metric pattern
        match = metric_pattern.match(line)
        if match:
            value_str = match.group(1).replace(',', '')
            metric_name = match.group(2)
            patch_metrics[metric_name] = float(value_str)
        else:
            # Try time elapsed pattern
            match = time_pattern.match(line)
            if match:
                patch_metrics['seconds-time-elapsed'] = float(match.group(1))

    # Print side by side with fixed column width plus ratio column
    col_width = 58
    ratio_width = 12
    print(f"{'MASTER':<{col_width}} | {'PATCH':<{col_width}} | {'RATIO':<{ratio_width}}")
    print("-" * col_width + "-+-" + "-" * col_width + "-+-" + "-" * ratio_width)

    max_lines = max(len(master_lines), len(patch_lines))
    for i in range(max_lines):
        master_line = master_lines[i].rstrip() if i < len(master_lines) else ""
        patch_line = patch_lines[i].rstrip() if i < len(patch_lines) else ""

        # Calculate ratio if this line contains a metric
        ratio_str = ""

        # Check if master line has a metric
        match = metric_pattern.match(master_line)
        if match:
            metric_name = match.group(2)
            if metric_name in master_metrics and metric_name in patch_metrics:
                master_val = master_metrics[metric_name]
                patch_val = patch_metrics[metric_name]
                if master_val != 0:
                    ratio = patch_val / master_val
                    ratio_str = f"{ratio:.3f}x"
        else:
            # Check for time elapsed
            match = time_pattern.match(master_line)
            if match:
                metric_name = 'seconds-time-elapsed'
                if metric_name in master_metrics and metric_name in patch_metrics:
                    master_val = master_metrics[metric_name]
                    patch_val = patch_metrics[metric_name]
                    if master_val != 0:
                        ratio = patch_val / master_val
                        ratio_str = f"{ratio:.3f}x"

        # Truncate lines that are too long
        if len(master_line) > col_width:
            master_line = master_line[:col_width-3] + "..."
        if len(patch_line) > col_width:
            patch_line = patch_line[:col_width-3] + "..."

        print(f"{master_line:<{col_width}} | {patch_line:<{col_width}} | {ratio_str:<{ratio_width}}")

    print("\n" + "="*120 + "\n")


def prepare_prefetch_cache(conn, query_def, cached_mode):
    """Prepare cache state for a prefetch benchmark query."""
    if cached_mode:
        # Cached mode: prewarm everything (indexes + tables)
        print("Prewarming indexes and tables (cached mode)...")
        prewarm_relations(conn, query_def.get("prewarm_indexes", []))
        prewarm_relations(conn, query_def.get("prewarm_tables", []), include_vm=True)
    else:
        # Uncached mode: evict heap, prewarm only indexes, clear OS cache
        print("Evicting relations and clearing OS cache (uncached mode)...")
        evict_relations(conn, query_def.get("evict", []))
        prewarm_relations(conn, query_def.get("prewarm_indexes", []))
        prewarm_relations(conn, query_def.get("prewarm_tables", []), vm_only=True)
        clear_os_cache()


def run_prefetch_query_profiling(args, master_bin, patch_bin, tmpfs_mount, perf_event, run_perf, prefetch_setting):
    """Profile a prefetch benchmark query and generate flame graphs."""
    query_id = args.query.upper()
    query_def = ALL_QUERIES[query_id]
    sql_query = query_def["sql"].strip()
    cached_mode = args.cached
    verify_fn, load_fn = _get_data_functions(query_id)

    print(f"\n{'='*60}")
    print(f"Profiling prefetch query: {query_id} - {query_def['name']}")
    print(f"Mode: {'cached' if cached_mode else 'uncached'}")
    print(f"{'='*60}\n")

    # Create output directory
    if os.path.exists(OUTPUT_DIR):
        print(f"Removing previous output directory: {OUTPUT_DIR}")
        shutil.rmtree(OUTPUT_DIR)
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    stacks_file_master = os.path.join(OUTPUT_DIR, "master.stacks")
    stacks_file_patch = os.path.join(OUTPUT_DIR, "patch.stacks")
    folded_file_master = os.path.join(OUTPUT_DIR, "master.folded")
    folded_file_patch = os.path.join(OUTPUT_DIR, "patch.folded")
    svg_file_diff = os.path.join(OUTPUT_DIR, "diff.svg")
    svg_file_master = os.path.join(OUTPUT_DIR, "master_flamegraph.svg")
    svg_file_patch = os.path.join(OUTPUT_DIR, "patch_flamegraph.svg")

    try:
        # Verify/load data on both servers if needed
        if not args.skip_data_load:
            print("\n--- Verifying benchmark data on master ---")
            start_server(master_bin, "master", MASTER_DATA_DIR, MASTER_CONN_DETAILS)
            if not verify_fn(MASTER_CONN_DETAILS):
                print("Loading data on master (this will take several minutes)...")
                load_fn(MASTER_CONN_DETAILS)
            stop_server(master_bin, MASTER_DATA_DIR)
            time.sleep(2)

            print("\n--- Verifying benchmark data on patch ---")
            start_server(patch_bin, "patch", PATCH_DATA_DIR, PATCH_CONN_DETAILS)
            if not verify_fn(PATCH_CONN_DETAILS):
                print("Loading data on patch (this will take several minutes)...")
                load_fn(PATCH_CONN_DETAILS)
            stop_server(patch_bin, PATCH_DATA_DIR)
            time.sleep(2)

        # Profile master
        print("\n--- Profiling master version ---")
        start_server(master_bin, "master", MASTER_DATA_DIR, MASTER_CONN_DETAILS)
        perf_command_master, total_time_master = profile_prefetch_query(
            master_bin, "master", MASTER_CONN_DETAILS,
            stacks_file_master, sql_query, query_def, cached_mode,
            is_master=True, prefetch_setting=prefetch_setting,
            run_perf=run_perf, perf_event=perf_event,
            highfreq=args.highfreq, run_perfstat=args.perfstat,
            benchmark_cpu=args.benchmark_cpu, perf_cpu=args.perf_cpu,
            disable_prefetch=args.disable_prefetch,
            no_pin=args.no_pin,
        )
        stop_server(master_bin, MASTER_DATA_DIR)
        time.sleep(2)

        # Profile patch
        print("\n--- Profiling patch version ---")
        start_server(patch_bin, "patch", PATCH_DATA_DIR, PATCH_CONN_DETAILS)
        perf_command_patch, total_time_patch = profile_prefetch_query(
            patch_bin, "patch", PATCH_CONN_DETAILS,
            stacks_file_patch, sql_query, query_def, cached_mode,
            is_master=test_is_master, prefetch_setting=prefetch_setting,
            run_perf=run_perf, perf_event=perf_event,
            highfreq=args.highfreq, run_perfstat=args.perfstat,
            benchmark_cpu=args.benchmark_cpu, perf_cpu=args.perf_cpu,
            disable_prefetch=args.disable_prefetch,
            no_pin=args.no_pin,
        )
        stop_server(patch_bin, PATCH_DATA_DIR)

    finally:
        # Always stop both servers
        print("\n--- Stopping PostgreSQL servers ---")
        stop_server(master_bin, MASTER_DATA_DIR)
        stop_server(patch_bin, PATCH_DATA_DIR)

        # Cleanup tmpfs if it was created
        if tmpfs_mount:
            cleanup_tmpfs(tmpfs_mount)

    print(f"\nPatch min: {total_time_patch:.2f} ms, Master min: {total_time_master:.2f} ms")
    print(f"Patch query took \033[1m{total_time_patch/total_time_master:.3f}x\033[0m as long as master")

    if not run_perf and not args.perfstat:
        print("Perf profiling was disabled. Exiting.")
        return
    elif args.perfstat:
        # Show perf stat output comparison
        master_stat_file = os.path.join(OUTPUT_DIR, "master_perfstat.txt")
        patch_stat_file = os.path.join(OUTPUT_DIR, "patch_perfstat.txt")
        print("\n--- Perf Stat Output Comparison ---")
        display_perfstat_comparison(master_stat_file, patch_stat_file)
        return

    # Generate flame graphs
    print("\n--- Generating flame graphs ---")
    generate_flamegraphs(
        stacks_file_master, stacks_file_patch,
        folded_file_master, folded_file_patch,
        svg_file_master, svg_file_patch, svg_file_diff,
        sql_query, perf_command_master, perf_command_patch
    )

    # Display all generated flame graphs
    print("\n--- Displaying flame graphs with imgcat ---")
    for svg_file in [svg_file_master, svg_file_patch, svg_file_diff]:
        if os.path.exists(svg_file):
            print(f"Displaying {os.path.basename(svg_file)}...")
            try:
                subprocess.run(["imgcat", svg_file], check=False)
            except FileNotFoundError:
                print("(imgcat not available for inline display)")
                break


def profile_prefetch_query(pg_bin_dir, pg_name, conn_details, output_file, sql_query,
                           query_def, cached_mode, is_master, prefetch_setting=None,
                           run_perf=False, perf_event=None,
                           highfreq=False, run_perfstat=False,
                           benchmark_cpu=14, perf_cpu=15, disable_prefetch=False,
                           no_pin=False):
    """Profile a prefetch benchmark query with appropriate cache preparation."""
    print(f"--- Testing {pg_name} ---")

    conn = None
    try:
        # Connect to the database
        conn = psycopg.connect(**conn_details, prepare_threshold=0)
        backend_pid = conn.info.backend_pid
        print(f"Successfully connected. Backend PID is: {backend_pid}")

        # Pin the backend process
        if not no_pin:
            try:
                result = subprocess.run(
                    ["taskset", "-cp", str(benchmark_cpu), str(backend_pid)],
                    capture_output=True, text=True, check=False
                )
                if result.returncode == 0:
                    print(f"Pinned backend PID {backend_pid} to CPU {benchmark_cpu}.")

                result = subprocess.run(
                    ["sudo", "chrt", "-f", "-p", "1", str(backend_pid)],
                    capture_output=True, text=True, check=False
                )
                if result.returncode == 0:
                    print(f"Set backend PID {backend_pid} to SCHED_FIFO.")
            except Exception as e:
                print(f"Warning: Could not set CPU affinity: {e}")
        else:
            print("CPU pinning disabled (--no-pin)")

        # Disable hardware prefetchers if requested
        original_prefetch_value = None
        if disable_prefetch:
            print(f"\nDisabling hardware prefetchers on CPU {benchmark_cpu}...")
            original_prefetch_value = disable_prefetchers(benchmark_cpu)

        # Prepare cache state
        with conn.cursor() as cursor:
            cursor.execute("CREATE EXTENSION IF NOT EXISTS pg_prewarm;")
            cursor.execute("CREATE EXTENSION IF NOT EXISTS pg_buffercache;")

        prepare_prefetch_cache(conn, query_def, cached_mode)

        # Set GUCs for the query
        gucs = query_def.get("gucs", {})
        set_gucs(conn, gucs, is_master=is_master, prefetch_setting=prefetch_setting)

        # Prepare the query once to avoid planning overhead in timing.
        # force_generic_plan ensures EXECUTE reuses the cached plan
        # without per-execution plan cache overhead.
        stmt_name = "_bench_stmt"
        with conn.cursor() as cursor:
            cursor.execute("SET plan_cache_mode = force_generic_plan")
            cursor.execute(f"PREPARE {stmt_name} AS {sql_query}")

        # Handle warmup queries (A3 special case)
        if query_def.get("warmup_query") and not cached_mode:
            print("Running warmup queries...")
            with conn.cursor() as cursor:
                cursor.execute(f"EXECUTE {stmt_name}")
                cursor.execute(f"EXECUTE {stmt_name}")

        # Show EXPLAIN plan
        print("\n--- Query Plan (EXPLAIN ANALYZE) ---")
        with conn.cursor() as cursor:
            cursor.execute(f"EXPLAIN (ANALYZE, COSTS OFF, TIMING ON) EXECUTE {stmt_name}")
            for row in cursor.fetchall():
                print(row[0])
        print("--- End Query Plan ---\n")

        # Start perf profiling
        perf_process = None
        perf_command = []
        start_time = time.time()

        if run_perf:
            perf_freq = "49999" if highfreq else str(PERF_FREQUENCY)
            perf_command = ["perf", "record", "-F", perf_freq]
            if perf_event:
                perf_command.extend(["-e", perf_event])
            if no_pin:
                print(f"Starting perf on PID {backend_pid}...")
                perf_command.extend(["-p", str(backend_pid), "-g", "-o", OUTPUT_DIR + "/" + pg_name])
            else:
                print(f"Starting perf on CPU core {benchmark_cpu}...")
                perf_command.extend(["-C", str(benchmark_cpu), "-g", "-o", OUTPUT_DIR + "/" + pg_name])
            perf_process = subprocess.Popen(perf_command)

            if not no_pin:
                try:
                    os.sched_setaffinity(perf_process.pid, {perf_cpu})
                    print(f"Pinned perf PID {perf_process.pid} to CPU {perf_cpu}.")
                except (AttributeError, PermissionError, OSError) as e:
                    print(f"Warning: Could not set CPU affinity for perf: {e}")

            time.sleep(1)  # Give perf time to initialize

        elif run_perfstat:
            stat_output_file = OUTPUT_DIR + "/" + pg_name + "_perfstat.txt"
            if no_pin:
                print(f"Starting perf stat on PID {backend_pid}...")
                perf_command = ["perf", "stat", "-ddd", "-p", str(backend_pid), "-o", stat_output_file]
            else:
                print(f"Starting perf stat on CPU core {benchmark_cpu}...")
                perf_command = ["perf", "stat", "-ddd", "-C", str(benchmark_cpu), "-o", stat_output_file]
            perf_process = subprocess.Popen(perf_command)
            time.sleep(1)

        # Execute the query (run multiple times for better profiling data)
        query_repetitions = 5  # Run a few times for flame graph data
        print(f"Executing query {query_repetitions} times for profiling...")

        explain_sql = f"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SERIALIZE) EXECUTE {stmt_name}"
        execution_times = []

        for i in range(query_repetitions):
            # In uncached mode, prepare cache before each run
            # In cached mode, data stays in buffer cache so no need to re-prewarm
            if not cached_mode:
                prepare_prefetch_cache(conn, query_def, cached_mode)

            with conn.cursor() as cursor:
                cursor.execute(explain_sql)
                result = cursor.fetchall()
                exec_time = extract_execution_time(result)
                if exec_time is not None:
                    execution_times.append(exec_time)
                    print(f"  Run {i+1}: {exec_time:.2f} ms")
                else:
                    print(f"  Run {i+1}: (could not extract time)")

        total_time = time.time() - start_time
        if execution_times:
            avg_time = sum(execution_times) / len(execution_times)
            min_time = min(execution_times)
            print(f"  Average: {avg_time:.2f} ms, Min: {min_time:.2f} ms")

        # Stop perf
        if perf_process:
            print("Stopping perf...")
            perf_process.send_signal(signal.SIGINT)
            perf_process.wait(timeout=10)

            # Generate stacks file
            if run_perf:
                print("Generating stack trace file...")
                perf_data_file = OUTPUT_DIR + "/" + pg_name
                perf_script_cmd = ["perf", "script", "-i", perf_data_file]
                with open(output_file, "w") as f:
                    subprocess.run(perf_script_cmd, stdout=f, check=True)

        # Restore hardware prefetchers if they were disabled
        if original_prefetch_value is not None:
            restore_prefetchers(benchmark_cpu, original_prefetch_value)

        # Deallocate prepared statement
        with conn.cursor() as cursor:
            cursor.execute(f"DEALLOCATE {stmt_name}")

        # Reset GUCs
        if gucs:
            reset_gucs(conn, gucs)

        # Return min execution time (in ms) for accurate comparison
        query_time_ms = min_time if execution_times else total_time * 1000
        return perf_command, query_time_ms

    finally:
        if conn:
            conn.close()


def generate_flamegraphs(stacks_file_master, stacks_file_patch,
                         folded_file_master, folded_file_patch,
                         svg_file_master, svg_file_patch, svg_file_diff,
                         sql_query, perf_command_master, perf_command_patch):
    """Generate individual and differential flame graphs."""
    STACKCOLLAPSE = os.path.join(FLAMEGRAPH_DIR, "stackcollapse-perf.pl")
    FLAMEGRAPH = os.path.join(FLAMEGRAPH_DIR, "flamegraph.pl")
    DIFFFOLDED = os.path.join(FLAMEGRAPH_DIR, "difffolded.pl")

    # Fold master stacks
    print("Folding master stacks...")
    with open(folded_file_master, "w") as f:
        subprocess.run([STACKCOLLAPSE, stacks_file_master], stdout=f, check=True)

    # Fold patch stacks
    print("Folding patch stacks...")
    with open(folded_file_patch, "w") as f:
        subprocess.run([STACKCOLLAPSE, stacks_file_patch], stdout=f, check=True)

    # Generate master flame graph
    print("Generating master flame graph...")
    with open(svg_file_master, "w") as f:
        perf_cmd_str = " ".join(perf_command_master) if perf_command_master else "perf record"
        subprocess.run([
            FLAMEGRAPH,
            "--title", f'master, "{sql_query[:50]}..."',
            "--subtitle", perf_cmd_str,
            folded_file_master
        ], stdout=f, check=True)

    # Generate patch flame graph
    print("Generating patch flame graph...")
    with open(svg_file_patch, "w") as f:
        perf_cmd_str = " ".join(perf_command_patch) if perf_command_patch else "perf record"
        subprocess.run([
            FLAMEGRAPH,
            "--title", f'patch, "{sql_query[:50]}..."',
            "--subtitle", perf_cmd_str,
            folded_file_patch
        ], stdout=f, check=True)

    # Generate differential flame graph
    print("Generating differential flame graph...")
    diff_proc = subprocess.run(
        [DIFFFOLDED, folded_file_master, folded_file_patch],
        capture_output=True, check=True
    )
    with open(svg_file_diff, "w") as f:
        subprocess.run([
            FLAMEGRAPH,
            "--title", f'master versus patch, "{sql_query[:50]}..."',
            "--negate"
        ], input=diff_proc.stdout, stdout=f, check=True)

    print(f"\nFlame graphs generated:")
    print(f"  Master: {svg_file_master}")
    print(f"  Patch: {svg_file_patch}")
    print(f"  Diff: {svg_file_diff}")


def main():
    """Main execution flow."""
    args = parse_arguments()

    # Validate that --perf and --perfstat are mutually exclusive
    if args.perf and args.perfstat:
        print("Error: --perf and --perfstat cannot be used together")
        sys.exit(1)

    # Extract perf event from args.perf if it's a string, otherwise set to None
    perf_event = args.perf if isinstance(args.perf, str) else None
    run_perf = bool(args.perf)  # True if --perf was specified (with or without event)

    # Check perf_event_paranoid setting when using perf
    if run_perf or args.perfstat:
        try:
            with open("/proc/sys/kernel/perf_event_paranoid", "r") as f:
                paranoid = int(f.read().strip())
                if paranoid != -1:
                    print(f"Error: /proc/sys/kernel/perf_event_paranoid is {paranoid}, must be -1")
                    print("Fix with: sudo sysctl kernel.perf_event_paranoid=-1")
                    sys.exit(1)
        except (FileNotFoundError, ValueError) as e:
            print(f"Warning: Could not check perf_event_paranoid: {e}")

    # Validate that --queries is only used with parameterized benchmarks
    if args.num_queries is not None and args.benchmark not in ("simple_select", "bitmap"):
        print("Error: --queries can only be used with --benchmark simple_select or bitmap")
        sys.exit(1)

    # Validate that --hash and --ios are not used together
    if args.use_hash_index and args.index_only_scan:
        print("Error: --hash and --ios cannot be used together (hash indexes lack support for index-only scans)")
        sys.exit(1)

    # Validate --query and --cached arguments
    if args.query and args.benchmark != "nestloop":  # nestloop is the default
        print("Error: --query and --benchmark are mutually exclusive")
        sys.exit(1)

    if args.cached and not args.query:
        print("Error: --cached requires --query")
        sys.exit(1)


    if args.query:
        query_id = args.query.upper()
        if query_id not in ALL_QUERIES:
            available = ", ".join(ALL_QUERIES.keys())
            print(f"Error: Unknown query '{args.query}'. Available: {available}")
            sys.exit(1)

    # Pin the script itself to a core to avoid it interfering with the benchmark.
    if not args.no_pin:
        try:
            pid = os.getpid()
            os.sched_setaffinity(pid, {0})
            print(f"Pinned this script (PID {pid}) to CPU 0.")
        except (AttributeError, PermissionError, OSError) as e:
            print(f"Warning: Could not set CPU affinity for this script: {e}")

    check_dependencies()

    # Resolve baseline and testbranch configuration
    global MASTER_CONN_DETAILS, MASTER_DATA_DIR, PATCH_CONN_DETAILS, PATCH_DATA_DIR
    baseline_name = args.baseline
    baseline_bin_orig, baseline_data_dir, _baseline_source_dir, baseline_conn = BASELINE_CONFIGS[baseline_name]
    MASTER_CONN_DETAILS = baseline_conn
    MASTER_DATA_DIR = baseline_data_dir

    testbranch_name = args.testbranch
    test_bin_orig, test_data_dir, _test_source_dir, test_conn = TESTBRANCH_CONFIGS[testbranch_name]
    PATCH_CONN_DETAILS = test_conn
    PATCH_DATA_DIR = test_data_dir

    # Setup tmpfs with hugepages if requested
    tmpfs_mount = None
    original_master_bin = baseline_bin_orig
    original_patch_bin = test_bin_orig
    master_bin = baseline_bin_orig
    patch_bin = test_bin_orig

    if not args.no_tmpfs_hugepages:
        tmpfs_mount = setup_tmpfs_hugepages()
        master_bin = copy_binaries_to_tmpfs(baseline_bin_orig, tmpfs_mount, "baseline")
        patch_bin = copy_binaries_to_tmpfs(test_bin_orig, tmpfs_mount, "testbranch")
        print(f"\nUsing tmpfs binaries:")
        print(f"  baseline ({baseline_name}): {master_bin}")
        print(f"  testbranch ({testbranch_name}): {patch_bin}\n")
    else:
        print("\nSkipping tmpfs hugepages setup (disabled with --no-tmpfs-hugepages)")
        master_bin = baseline_bin_orig
        patch_bin = test_bin_orig

    # Determine prefetch setting for patch/test version
    test_has_prefetch = BUILD_HAS_PREFETCH[testbranch_name]
    test_is_master = not test_has_prefetch
    if test_has_prefetch:
        if args.prefetch_only:
            prefetch_setting = "on"
        elif args.prefetch_disabled:
            prefetch_setting = "off"
        else:
            prefetch_setting = None
    else:
        # Testbranch has no prefetch GUC — don't try to set it
        prefetch_setting = None

    # Handle prefetch benchmark query mode
    if args.query:
        run_prefetch_query_profiling(
            args, master_bin, patch_bin, tmpfs_mount,
            perf_event, run_perf, prefetch_setting
        )
        return

    # Get benchmark configuration
    benchmark = BENCHMARKS[args.benchmark]
    sql_query = benchmark["sql_query"]
    query_repetitions = benchmark["query_repetitions"]
    max_aid_val = benchmark.get("max_aid_val")
    use_bitmap = args.benchmark == "bitmap"
    bitmap_range = args.bitmap_range if use_bitmap else benchmark.get("bitmap_range", 2000)

    # Override query_repetitions if --queries was provided
    if args.num_queries is not None:
        query_repetitions = args.num_queries

    # Modify SQL query for index-only scan if --ios is used
    if args.index_only_scan:
        if args.benchmark == "simple_select":
            # Change "select *" to "select aid" to enable index-only scans
            sql_query = sql_query.replace("select *", "select aid")

    if os.path.exists(OUTPUT_DIR):
        print(f"Removing previous output directory: {OUTPUT_DIR}")
        shutil.rmtree(OUTPUT_DIR)

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    print(f"--- Profiling \"{sql_query}\" ---")
    print(f"Using benchmark: {args.benchmark}")

    stacks_file_master = os.path.join(OUTPUT_DIR, "master.stacks")
    stacks_file_patch = os.path.join(OUTPUT_DIR, "patch.stacks")
    folded_file_master = os.path.join(OUTPUT_DIR, "master.folded")
    folded_file_patch = os.path.join(OUTPUT_DIR, "patch.folded")
    svg_file_diff = os.path.join(OUTPUT_DIR, "diff.svg")
    svg_file_master = os.path.join(OUTPUT_DIR, "master_flamegraph.svg")
    svg_file_patch = os.path.join(OUTPUT_DIR, "patch_flamegraph.svg")

    try:
        # Initialize pgbench if requested (must do one at a time due to shared memory constraints)
        if not args.skip_pgbench_init:
            print("--- Initializing pgbench for master ---")
            start_server(master_bin, "master", MASTER_DATA_DIR, MASTER_CONN_DETAILS)
            init_pgbench(master_bin, MASTER_CONN_DETAILS, args.pgbench_scale, args.use_hash_index)
            stop_server(master_bin, MASTER_DATA_DIR)
            time.sleep(2)

            print("--- Initializing pgbench for patch ---")
            start_server(patch_bin, "patch", PATCH_DATA_DIR, PATCH_CONN_DETAILS)
            init_pgbench(patch_bin, PATCH_CONN_DETAILS, args.pgbench_scale, args.use_hash_index)
            stop_server(patch_bin, PATCH_DATA_DIR)
            time.sleep(2)
        else:
            print("Skipping pgbench initialization.")
            # Verify that existing indexes match the requested type
            if args.use_hash_index:
                print("\n--- Verifying existing hash indexes ---")
                start_server(master_bin, "master", MASTER_DATA_DIR, MASTER_CONN_DETAILS)
                verify_hash_index_exists("master", MASTER_CONN_DETAILS)
                stop_server(master_bin, MASTER_DATA_DIR)
                time.sleep(1)

                start_server(patch_bin, "patch", PATCH_DATA_DIR, PATCH_CONN_DETAILS)
                verify_hash_index_exists("patch", PATCH_CONN_DETAILS)
                stop_server(patch_bin, PATCH_DATA_DIR)
                time.sleep(1)
            else:
                print("\n--- Verifying existing B-tree indexes ---")
                start_server(master_bin, "master", MASTER_DATA_DIR, MASTER_CONN_DETAILS)
                verify_btree_index_exists("master", MASTER_CONN_DETAILS)
                stop_server(master_bin, MASTER_DATA_DIR)
                time.sleep(1)

                start_server(patch_bin, "patch", PATCH_DATA_DIR, PATCH_CONN_DETAILS)
                verify_btree_index_exists("patch", PATCH_CONN_DETAILS)
                stop_server(patch_bin, PATCH_DATA_DIR)
                time.sleep(1)

        # Get row counts from both servers and verify they match
        print("\n--- Verifying pgbench_accounts row counts ---")
        start_server(master_bin, "master", MASTER_DATA_DIR, MASTER_CONN_DETAILS)
        master_row_count = get_pgbench_row_count("master", MASTER_CONN_DETAILS)
        stop_server(master_bin, MASTER_DATA_DIR)
        time.sleep(1)

        start_server(patch_bin, "patch", PATCH_DATA_DIR, PATCH_CONN_DETAILS)
        patch_row_count = get_pgbench_row_count("patch", PATCH_CONN_DETAILS)
        stop_server(patch_bin, PATCH_DATA_DIR)
        time.sleep(1)

        if master_row_count != patch_row_count:
            print(f"Error: Row count mismatch! master={master_row_count}, patch={patch_row_count}")
            sys.exit(1)

        # Use the actual row count for parameterized benchmarks
        if args.benchmark == "simple_select":
            max_aid_val = master_row_count
            print(f"Using max_aid_val={max_aid_val} for simple_select benchmark")
        elif args.benchmark == "bitmap":
            max_aid_val = master_row_count
            print(f"Using max_aid_val={max_aid_val}, bitmap_range={bitmap_range} for bitmap benchmark")

        # Profile in the order specified by args.patch_first
        if args.patch_first:
            # Profile patch first
            print("--- Profiling patch version ---")
            start_server(patch_bin, "patch", PATCH_DATA_DIR, PATCH_CONN_DETAILS)
            perf_command_patch, total_time_patch = profile_postgres(
                patch_bin, "patch", PATCH_CONN_DETAILS,
                stacks_file_patch, sql_query, query_repetitions, run_perf, max_aid_val, perf_event, args.highfreq, args.index_only_scan, args.perfstat, args.discard_runs, args.benchmark_cpu, args.perf_cpu, args.disable_prefetch, is_master=test_is_master, prefetch_setting=prefetch_setting, no_pin=args.no_pin, use_bitmap=use_bitmap, bitmap_range=bitmap_range)
            stop_server(patch_bin, PATCH_DATA_DIR)
            time.sleep(2)

            # Then profile master
            print("--- Profiling master version ---")
            start_server(master_bin, "master", MASTER_DATA_DIR, MASTER_CONN_DETAILS)
            perf_command_master, total_time_master = profile_postgres(
                master_bin, "master", MASTER_CONN_DETAILS,
                stacks_file_master, sql_query, query_repetitions, run_perf, max_aid_val, perf_event, args.highfreq, args.index_only_scan, args.perfstat, args.discard_runs, args.benchmark_cpu, args.perf_cpu, args.disable_prefetch, is_master=True, prefetch_setting=prefetch_setting, no_pin=args.no_pin, use_bitmap=use_bitmap, bitmap_range=bitmap_range)
            stop_server(master_bin, MASTER_DATA_DIR)
        else:
            # Profile master first (default)
            print("--- Profiling master version ---")
            start_server(master_bin, "master", MASTER_DATA_DIR, MASTER_CONN_DETAILS)
            perf_command_master, total_time_master = profile_postgres(
                master_bin, "master", MASTER_CONN_DETAILS,
                stacks_file_master, sql_query, query_repetitions, run_perf, max_aid_val, perf_event, args.highfreq, args.index_only_scan, args.perfstat, args.discard_runs, args.benchmark_cpu, args.perf_cpu, args.disable_prefetch, is_master=True, prefetch_setting=prefetch_setting, no_pin=args.no_pin, use_bitmap=use_bitmap, bitmap_range=bitmap_range)
            stop_server(master_bin, MASTER_DATA_DIR)
            time.sleep(2)

            # Then profile patch
            print("--- Profiling patch version ---")
            start_server(patch_bin, "patch", PATCH_DATA_DIR, PATCH_CONN_DETAILS)
            perf_command_patch, total_time_patch = profile_postgres(
                patch_bin, "patch", PATCH_CONN_DETAILS,
                stacks_file_patch, sql_query, query_repetitions, run_perf, max_aid_val, perf_event, args.highfreq, args.index_only_scan, args.perfstat, args.discard_runs, args.benchmark_cpu, args.perf_cpu, args.disable_prefetch, is_master=test_is_master, prefetch_setting=prefetch_setting, no_pin=args.no_pin, use_bitmap=use_bitmap, bitmap_range=bitmap_range)
            stop_server(patch_bin, PATCH_DATA_DIR)

    finally:
        # Always stop both servers
        print("--- Stopping PostgreSQL servers ---")
        stop_server(master_bin, MASTER_DATA_DIR)
        stop_server(patch_bin, PATCH_DATA_DIR)

        # Cleanup tmpfs if it was created
        if tmpfs_mount:
            cleanup_tmpfs(tmpfs_mount)

    print(f"Patch query loop took \033[1m{total_time_patch/total_time_master:.3f}x\033[0m as long as master")

    if not run_perf and not args.perfstat:
        print("Perf profiling was disabled. Exiting.")
        return
    elif args.perfstat:
        # Display side-by-side comparison of perf stat results
        master_stat_file = os.path.join(OUTPUT_DIR, "master_perfstat.txt")
        patch_stat_file = os.path.join(OUTPUT_DIR, "patch_perfstat.txt")
        display_perfstat_comparison(master_stat_file, patch_stat_file)
        print("perf stat mode complete. Skipping flamegraph generation.")
        return

    # --- Generate Flame Graphs ---
    print("--- Generating Flame Graphs ---")

    # Fold stack traces
    print("Folding stack traces...")
    with open(folded_file_master, "w") as f1, open(folded_file_patch, "w") as f2:
        subprocess.run(
            [os.path.join(FLAMEGRAPH_DIR, "stackcollapse-perf.pl"), stacks_file_master],
            stdout=f1, check=True
        )
        subprocess.run(
            [os.path.join(FLAMEGRAPH_DIR, "stackcollapse-perf.pl"), stacks_file_patch],
            stdout=f2, check=True
        )

    print("Creating individual flame graph for master...")
    with open(svg_file_master, "w") as f_svg:
        subprocess.run(
                [
                    os.path.join(FLAMEGRAPH_DIR, "flamegraph.pl"),
                    "--title", "master, \"" + sql_query + "\"",
                    "--subtitle", perf_command_master,
                    folded_file_master,
                    ],
                stdout=f_svg,
                check=True,
                )
    print(f"master flame graph created: {svg_file_master}")

    print("Creating individual flame graph for patch...")
    with open(svg_file_patch, "w") as f_svg:
        subprocess.run(
                [
                    os.path.join(FLAMEGRAPH_DIR, "flamegraph.pl"),
                    "--title", "patch, \"" + sql_query + "\"",
                    "--subtitle", perf_command_patch,
                    folded_file_patch,
                    ],
                stdout=f_svg,
                check=True,
                )
    print(f"patch flame graph created: {svg_file_patch}")

    # Create the differential SVG
    print("Creating the differential flame graph...")
    difffolded_cmd = [os.path.join(FLAMEGRAPH_DIR, "difffolded.pl"), folded_file_master, folded_file_patch]

    flamegraph_cmd = [
        os.path.join(FLAMEGRAPH_DIR, "flamegraph.pl"),
        "--title", "master versus patch, \"" + sql_query + "\"",
        "--subtitle", perf_command_master + ", " + perf_command_patch,
    ]

    p1 = subprocess.Popen(difffolded_cmd, stdout=subprocess.PIPE)
    with open(svg_file_diff, "w") as f_svg:
        subprocess.run(flamegraph_cmd, stdin=p1.stdout, stdout=f_svg, check=True)
    p1.stdout.close()

    print(f"Differential flame graph created: {svg_file_diff}")

    # Print perf recipe for comparing a specific function
    print("\n--- Perf Recipe for Function Comparison (master vs patch) ---")
    print("To compare a specific function between master and patch profiles, use:")
    print()
    print(f"  perf diff {OUTPUT_DIR}/master {OUTPUT_DIR}/patch")
    print()
    print("Replace 'master' and 'patch' with the perf.data file paths if using different locations.")
    print("This will show the differences in:")
    print("  - Sampling count")
    print("  - Instructions")
    print("  - Cache misses")
    print("  - Other performance metrics")
    print()
    print("To view a specific function in detail from master:")
    print(f"  perf report -i {OUTPUT_DIR}/master")
    print()
    print("To view a specific function in detail from patch:")
    print(f"  perf report -i {OUTPUT_DIR}/patch")
    print()

    # Display all generated SVG files using imgcat
    print("\n--- Displaying flame graphs with imgcat ---")
    svg_files = [svg_file_master, svg_file_patch, svg_file_diff]
    for svg_file in svg_files:
        if os.path.exists(svg_file):
            print(f"Displaying {os.path.basename(svg_file)}...")
            subprocess.run(["imgcat", svg_file], check=False)
        else:
            print(f"Warning: SVG file not found: {svg_file}")

    print("Done.")

if __name__ == "__main__":
    main()
