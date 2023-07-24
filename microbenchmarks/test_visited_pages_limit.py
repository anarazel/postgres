#!/usr/bin/env python3
"""
Stress test for VISITED_PAGES_LIMIT callback mechanism.

Measures planning latency when many dead tuples accumulate at index extremes.
Uses a queue-like workload pattern.

This tests the callback mechanism that allows get_actual_variable_endpoint()
to abort early when too many heap pages have been visited during selectivity
estimation.
"""

import argparse
import os
import subprocess
import sys
import threading
import time
from typing import List, Optional

import numpy as np
import psycopg

# Import common utilities
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchmark_common import (
    MASTER_BIN, PATCH_BIN, MASTER_DATA_DIR, PATCH_DATA_DIR,
    MASTER_CONN, PATCH_CONN
)

# Test configuration defaults
# To trigger the VISITED_PAGES_LIMIT (100 pages), we need dead tuples spanning
# more than 100 heap pages. With fillfactor=10, each page holds ~10 tuples
# (assuming ~800 bytes per tuple with padding), so we need >1000 dead tuples.
# To be safe, we use much larger numbers.
TABLE_NAME = "queue_test"
INITIAL_ROWS = 1_000_000       # 1M rows
DELETE_PERCENT = 2            # Delete 2% of rows to create dead tuples
EXPLAINS_PER_ROW = 0.02        # 1 explain per 50 rows (0.02 = 1/50)
NUM_WORKERS = 4
FILLFACTOR = 10                # Low fillfactor = more pages = more page visits


def format_duration(seconds: float) -> str:
    """Format duration as HH:MM:SS.MMMM"""
    hours = int(seconds // 3600)
    minutes = int((seconds % 3600) // 60)
    secs = seconds % 60
    return f"{hours:02d}:{minutes:02d}:{secs:07.4f}"


def start_server(bin_dir: str, data_dir: str, conn_details: dict, name: str) -> None:
    """Start PostgreSQL server."""
    pg_ctl = os.path.join(bin_dir, "pg_ctl")
    log_file = f"/tmp/{name}_visited_pages_test.log"

    # Stop if running
    subprocess.run(
        [pg_ctl, "stop", "-D", data_dir, "-m", "fast"],
        capture_output=True,
        check=False
    )
    time.sleep(1)

    # Start with autovacuum off
    port = conn_details.get("port", 5432)
    cmd = [
        pg_ctl, "start", "-D", data_dir, "-l", log_file,
        "-o", f"-p {port}",
        "-o", "--autovacuum=off"
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Error starting {name} server:")
        print(result.stdout)
        print(result.stderr)
        sys.exit(1)

    # Wait for server to be ready
    for _ in range(30):
        try:
            conn = psycopg.connect(**conn_details)
            conn.close()
            print(f"{name} server started on port {port}")
            return
        except Exception:
            time.sleep(0.5)

    raise RuntimeError(f"Server {name} failed to start")


def stop_server(bin_dir: str, data_dir: str, name: str) -> None:
    """Stop PostgreSQL server."""
    pg_ctl = os.path.join(bin_dir, "pg_ctl")
    subprocess.run(
        [pg_ctl, "stop", "-D", data_dir, "-m", "fast"],
        capture_output=True,
        check=False
    )
    print(f"{name} server stopped")


def setup_queue_table(conn_details: dict, num_rows: int, fillfactor: int,
                      logged: bool = False) -> None:
    """Create queue table and populate with initial data."""
    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    unlogged = "" if logged else "UNLOGGED"

    with conn.cursor() as cur:
        cur.execute(f"DROP TABLE IF EXISTS {TABLE_NAME}")
        # Use low fillfactor so dead tuples are spread across many pages
        # This ensures scanning through dead tuples hits many heap pages
        cur.execute(f"""
            CREATE {unlogged} TABLE {TABLE_NAME} (
                id bigint PRIMARY KEY,
                data text
            ) WITH (autovacuum_enabled=off, fillfactor={fillfactor})
        """)
        # Use larger padding to make tuples bigger = fewer per page
        print("  Loading table...", end="", flush=True)
        load_start = time.perf_counter()
        cur.execute(f"""
            INSERT INTO {TABLE_NAME}
            SELECT i, repeat('x', 500)
            FROM generate_series(1, {num_rows}) i
        """)
        load_duration = time.perf_counter() - load_start
        print(f" done [{format_duration(load_duration)}]")

        # Create additional index for testing (the PK index already exists)
        print("  Creating index...", end="", flush=True)
        index_start = time.perf_counter()
        cur.execute(f"CREATE INDEX {TABLE_NAME}_id_idx ON {TABLE_NAME}(id)")
        index_duration = time.perf_counter() - index_start
        print(f" done [{format_duration(index_duration)}]")

    conn.close()

    conn = psycopg.connect(**conn_details)
    conn.autocommit = True
    with conn.cursor() as cur:
        print("  Running VACUUM ANALYZE...", end="", flush=True)
        vacuum_start = time.perf_counter()
        cur.execute(f"VACUUM ANALYZE {TABLE_NAME}")
        vacuum_duration = time.perf_counter() - vacuum_start
        print(f" done [{format_duration(vacuum_duration)}]")

        # Report table size and visibility map status
        cur.execute(f"""
            SELECT pg_size_pretty(pg_relation_size('{TABLE_NAME}')),
                   pg_relation_size('{TABLE_NAME}') / 8192 as pages
        """)
        size, pages = cur.fetchone()
        print(f"  Table: {num_rows:,} rows, {size}, ~{pages} pages")

    conn.close()


def delete_from_queue(conn_details: dict, low_id: int, high_id: int) -> int:
    """Delete rows from the queue. Returns number of rows deleted."""
    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    with conn.cursor() as cur:
        delete_start = time.perf_counter()
        cur.execute(
            f"DELETE FROM {TABLE_NAME} WHERE id BETWEEN %s AND %s",
            (low_id, high_id)
        )
        delete_duration = time.perf_counter() - delete_start
        deleted = cur.rowcount
        print(f"  Deleted {deleted:,} rows [{format_duration(delete_duration)}]")

    conn.close()
    return deleted


def analyze_index_dead_tuples(conn_details: dict, index_name: str) -> None:
    """
    Analyze index pages using pageinspect to count dead tuples.
    Prints a summary of leaf pages with/without dead items.
    """
    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    with conn.cursor() as cur:
        # Ensure pageinspect is available
        cur.execute("CREATE EXTENSION IF NOT EXISTS pageinspect")

        # Get total number of pages in the index
        cur.execute(f"""
            SELECT pg_relation_size('{index_name}'::regclass) / 8192 AS num_pages
        """)
        total_pages = cur.fetchone()[0]

        print(f"  Analyzing {total_pages:,} index pages...", end="", flush=True)
        analyze_start = time.perf_counter()

        # Get all page stats in one query using generate_series + lateral
        cur.execute(f"""
            SELECT
                stats.type,
                stats.live_items,
                stats.dead_items
            FROM generate_series(1, {total_pages - 1}) AS page_num,
            LATERAL bt_page_stats('{index_name}', page_num) AS stats
        """)
        rows = cur.fetchall()

        # Process results
        leaf_pages_with_dead = []  # list of (live_items, dead_items)
        leaf_pages_no_dead = []    # list of live_items
        non_leaf_pages = 1  # page 0 is meta page, skipped in query

        for page_type, live_items, dead_items in rows:
            if page_type == 'l':  # leaf page
                if dead_items > 0:
                    leaf_pages_with_dead.append((live_items, dead_items))
                else:
                    leaf_pages_no_dead.append(live_items)
            else:
                non_leaf_pages += 1

        analyze_duration = time.perf_counter() - analyze_start
        print(f" done [{format_duration(analyze_duration)}]")

        # Compute statistics
        total_leaf_pages = len(leaf_pages_with_dead) + len(leaf_pages_no_dead)

        print(f"\n  Index Analysis: {index_name}")
        print(f"    Total pages:      {total_pages:,}")
        print(f"    Non-leaf pages:   {non_leaf_pages:,}")
        print(f"    Leaf pages:       {total_leaf_pages:,}")

        if leaf_pages_with_dead:
            dead_counts = [d for _, d in leaf_pages_with_dead]
            live_counts = [l for l, _ in leaf_pages_with_dead]
            pct_with_dead = 100.0 * len(leaf_pages_with_dead) / total_leaf_pages

            print(f"\n    Leaf pages WITH dead items: {len(leaf_pages_with_dead):,} ({pct_with_dead:.1f}%)")
            print(f"      Dead items per page:  min={min(dead_counts)}, avg={np.mean(dead_counts):.1f}, max={max(dead_counts)}")
            print(f"      Live items per page:  min={min(live_counts)}, avg={np.mean(live_counts):.1f}, max={max(live_counts)}")
            total_dead = sum(dead_counts)
            total_live_on_dead_pages = sum(live_counts)
            print(f"      Total dead items:     {total_dead:,}")
            print(f"      Total live items:     {total_live_on_dead_pages:,}")
        else:
            print(f"\n    Leaf pages WITH dead items: 0 (0.0%)")

        if leaf_pages_no_dead:
            pct_no_dead = 100.0 * len(leaf_pages_no_dead) / total_leaf_pages
            total_live_on_clean_pages = sum(leaf_pages_no_dead)
            print(f"\n    Leaf pages without dead items: {len(leaf_pages_no_dead):,} ({pct_no_dead:.1f}%)")
            print(f"      Live items per page:  min={min(leaf_pages_no_dead)}, avg={np.mean(leaf_pages_no_dead):.1f}, max={max(leaf_pages_no_dead)}")
            print(f"      Total live items:     {total_live_on_clean_pages:,}")
        else:
            print(f"\n    Leaf pages without dead items: 0 (0.0%)")

    conn.close()


def synchronized_worker(
    conn_details: dict,
    duration: float,           # how long to run (seconds)
    results: List[float],
    reverse: bool,
    stop_event: threading.Event,
    planning_rate: float,      # plans per second per worker
    delete_rate: float,        # total deletes per second (shared across all workers)
    insert_rate: float,        # total inserts per second (shared across all workers)
    num_workers: int,
    counters: List[int],       # shared [delete_bound, insert_next] - disjoint ranges
    counter_lock: threading.Lock,
    failed_deletes: List[int]  # shared counter for failed deletes
) -> None:
    """
    Worker that runs explains in lockstep with proportional delete/insert.

    Each worker has separate connections for each operation type, ensuring
    each delete and insert runs in its own transaction. Operations are
    synchronized: delete/insert happen proportionally to explain rate.

    IMPORTANT: Delete and insert operate on disjoint ID ranges to avoid races:
    - delete_bound: highest (or lowest in reverse) ID that definitely exists
    - insert_next: next ID to insert, always outside the existing range
    """
    # Separate connections ensure each operation is its own transaction
    plan_conn = psycopg.connect(**conn_details)
    plan_conn.autocommit = True

    delete_conn = psycopg.connect(**conn_details)
    delete_conn.autocommit = True

    insert_conn = psycopg.connect(**conn_details)
    insert_conn.autocommit = True

    # Calculate per-worker rates for delete/insert
    per_worker_delete_rate = delete_rate / num_workers if num_workers > 0 else 0
    per_worker_insert_rate = insert_rate / num_workers if num_workers > 0 else 0

    # Calculate how many plans between each delete/insert for this worker
    # e.g., if planning_rate=500 and per_worker_delete_rate=250, then
    # plans_per_delete=2, meaning every 2 plans we do 1 delete
    plans_per_delete = planning_rate / per_worker_delete_rate if per_worker_delete_rate > 0 else float('inf')
    plans_per_insert = planning_rate / per_worker_insert_rate if per_worker_insert_rate > 0 else float('inf')

    # Accumulators to track when to do delete/insert
    delete_accumulator = 0.0
    insert_accumulator = 0.0

    # Time-based rate limiting for constant planning rate
    plan_interval = 1.0 / planning_rate if planning_rate > 0 else 0

    end_time = time.perf_counter() + duration

    with plan_conn.cursor() as plan_cur, \
         delete_conn.cursor() as delete_cur, \
         insert_conn.cursor() as insert_cur:

        # Force index scan for planning
        plan_cur.execute("SET enable_seqscan = off")
        plan_cur.execute("SET enable_bitmapscan = off")

        # Choose operator based on direction
        if reverse:
            query = f"EXPLAIN SELECT * FROM {TABLE_NAME} WHERE id < 1000000"
        else:
            query = f"EXPLAIN SELECT * FROM {TABLE_NAME} WHERE id > 0"

        while time.perf_counter() < end_time and not stop_event.is_set():
            cycle_start = time.perf_counter()

            # Check if we should do a delete this cycle
            # counters[0] = delete_bound: extremal ID that definitely exists
            # Delete from the end OPPOSITE to where dead tuples are
            delete_accumulator += 1.0
            if delete_accumulator >= plans_per_delete:
                delete_accumulator -= plans_per_delete
                with counter_lock:
                    del_id = counters[0]
                    if reverse:
                        # Delete from low end, move bound up
                        counters[0] += 1
                    else:
                        # Delete from high end, move bound down
                        counters[0] -= 1

                delete_cur.execute(f"DELETE FROM {TABLE_NAME} WHERE id = %s", (del_id,))
                if delete_cur.rowcount != 1:
                    # This should never happen - abort immediately
                    with counter_lock:
                        failed_deletes[0] += 1
                    stop_event.set()
                    break

            # Check if we should do an insert this cycle
            # counters[1] = insert_next: next ID to insert (disjoint from delete range)
            insert_accumulator += 1.0
            if insert_accumulator >= plans_per_insert:
                insert_accumulator -= plans_per_insert
                with counter_lock:
                    ins_id = counters[1]
                    if reverse:
                        # Insert below existing range, move down
                        counters[1] -= 1
                    else:
                        # Insert above existing range, move up
                        counters[1] += 1

                insert_cur.execute(
                    f"INSERT INTO {TABLE_NAME} (id, data) VALUES (%s, %s)",
                    (ins_id, 'x' * 500)
                )

            # Do the explain query and measure latency
            plan_start = time.perf_counter()
            plan_cur.execute(query)
            plan_cur.fetchall()
            latency_ms = (time.perf_counter() - plan_start) * 1000
            results.append(latency_ms)

            # Rate limit to maintain constant planning rate
            if plan_interval > 0:
                elapsed = time.perf_counter() - cycle_start
                sleep_time = plan_interval - elapsed
                if sleep_time > 0:
                    time.sleep(sleep_time)

    plan_conn.close()
    delete_conn.close()
    insert_conn.close()


def run_stress_test(
    conn_details: dict,
    name: str,
    phase: str,
    num_workers: int,
    duration: float,
    reverse: bool,
    planning_rate: float = 0,      # plans per second per worker, 0 = unlimited
    delete_rate: float = 0,        # deletes per second total, 0 = disabled
    insert_rate: float = 0,        # inserts per second total, 0 = disabled
    live_range_init: Optional[List[int]] = None  # [min_live_id, max_live_id]
) -> np.ndarray:
    """Run planning stress test with synchronized delete/insert/explain operations."""
    threads: List[threading.Thread] = []
    stop_event = threading.Event()

    # Per-worker results
    thread_results: List[List[float]] = [[] for _ in range(num_workers)]

    # Initialize counters for disjoint delete/insert ranges
    # counters[0] = delete_bound: extremal ID that definitely exists
    # counters[1] = insert_next: next ID to insert (outside existing range)
    live_range = live_range_init if live_range_init else [1, 1000000]
    if reverse:
        # Reverse: delete from low end, insert below
        # delete_bound = min_live_id, insert_next = min_live_id - 1
        counters = [live_range[0], live_range[0] - 1]
    else:
        # Normal: delete from high end, insert above
        # delete_bound = max_live_id, insert_next = max_live_id + 1
        counters = [live_range[1], live_range[1] + 1]
    counter_lock = threading.Lock()

    # Counter for failed deletes (should always be 0)
    failed_deletes = [0]

    def worker_wrapper(worker_id: int):
        synchronized_worker(
            conn_details,
            duration,
            thread_results[worker_id],
            reverse,
            stop_event,
            planning_rate,
            delete_rate,
            insert_rate,
            num_workers,
            counters,
            counter_lock,
            failed_deletes
        )

    # Start workers
    phase_start = time.perf_counter()
    for i in range(num_workers):
        t = threading.Thread(target=worker_wrapper, args=(i,))
        threads.append(t)
        t.start()

    # Wait for all workers to complete
    for t in threads:
        t.join()
    phase_duration = time.perf_counter() - phase_start

    # Check for failed deletes - this indicates a bug in our tracking logic
    if failed_deletes[0] > 0:
        print(f"\nERROR: {failed_deletes[0]} delete(s) failed to delete exactly 1 row!")
        print("This means we tried to delete a non-existent row, which would mark")
        print("LP_DEAD tuples and invalidate the test. Aborting.")
        sys.exit(1)

    # Combine results
    results: List[float] = []
    for tr in thread_results:
        results.extend(tr)

    arr = np.array(results)

    # Print results
    print(f"\n=== {name}: {phase} [{format_duration(phase_duration)}] ===")
    print(f"Planning Latency (ms):")
    print(f"  Count:  {len(arr)}")
    print(f"  Avg:    {np.mean(arr):.3f}")
    print(f"  p50:    {np.percentile(arr, 50):.3f}")
    print(f"  p95:    {np.percentile(arr, 95):.3f}")
    print(f"  p99:    {np.percentile(arr, 99):.3f}")
    print(f"  Max:    {np.max(arr):.3f}")

    return arr


def run_test_for_version(
    name: str,
    bin_dir: str,
    data_dir: str,
    conn_details: dict,
    args: argparse.Namespace
) -> dict:
    """Run the complete test for one PostgreSQL version."""
    results = {}

    print(f"\n{'='*60}")
    print(f"Testing {name}")
    print(f"{'='*60}")

    start_server(bin_dir, data_dir, conn_details, name)

    try:
        # Setup
        setup_queue_table(conn_details, args.rows, args.fillfactor, args.logged)

        # Phase 1: Baseline (no dead tuples)
        print(f"\nPhase 1: Baseline (no dead tuples)")
        print(f"  Duration: {args.baseline_duration:.1f}s, ~{args.baseline_explains:,} explains")
        results["baseline"] = run_stress_test(
            conn_details, name, "Baseline (no dead tuples)",
            args.workers, args.baseline_duration, args.reverse,
            planning_rate=args.planning_rate
        )

        # Phase 2: Bulk delete to create dead tuples
        if args.reverse:
            # Delete from high end
            low_id = args.rows - args.delete_count + 1
            high_id = args.rows
            print(f"\nPhase 2: Bulk delete (rows {low_id:,}-{high_id:,} from high end)")
        else:
            # Delete from low end
            low_id = 1
            high_id = args.delete_count
            print(f"\nPhase 2: Bulk delete (rows {low_id:,}-{high_id:,} from low end)")

        delete_from_queue(conn_details, low_id, high_id)

        # Track live range for queue workers: [min_live_id, max_live_id]
        if args.reverse:
            live_range = [1, args.rows - args.delete_count]
        else:
            live_range = [args.delete_count + 1, args.rows]

        # Phase 3: Run stress test with dead tuples and background activity
        phase_name = f"Main (with {args.delete_count:,} dead tuples)"
        print(f"\nPhase 3: {phase_name}")
        print(f"  Duration: {args.main_duration:.1f}s, ~{args.main_explains:,} explains")
        results["dead_tuples"] = run_stress_test(
            conn_details, name, phase_name,
            args.workers, args.main_duration, args.reverse,
            planning_rate=args.planning_rate,
            delete_rate=args.delete_rate,
            insert_rate=args.insert_rate,
            live_range_init=live_range
        )

        # Analyze index again to see LP_DEAD marking from scans
        print("\n  Index state after stress test (LP_DEAD items marked by scans):")
        analyze_index_dead_tuples(conn_details, f"{TABLE_NAME}_id_idx")

    finally:
        stop_server(bin_dir, data_dir, name)

    return results


def main():
    parser = argparse.ArgumentParser(
        description="Stress test for VISITED_PAGES_LIMIT callback mechanism"
    )
    parser.add_argument(
        "--preview", action="store_true",
        help="Show test configuration and exit without running"
    )
    parser.add_argument(
        "--master-only", action="store_true",
        help="Only test master branch"
    )
    parser.add_argument(
        "--patch-only", action="store_true",
        help="Only test patch branch"
    )
    parser.add_argument(
        "--workers", type=int, default=NUM_WORKERS,
        help=f"Number of concurrent workers (default: {NUM_WORKERS})"
    )
    parser.add_argument(
        "--rows", type=int, default=INITIAL_ROWS,
        help=f"Initial number of rows (default: {INITIAL_ROWS})"
    )
    parser.add_argument(
        "--explains-per-row", type=float, default=EXPLAINS_PER_ROW,
        help=f"Total explains = rows × this value (default: {EXPLAINS_PER_ROW}, i.e. 1 per 50 rows)"
    )
    parser.add_argument(
        "--delete-percent", type=float, default=DELETE_PERCENT,
        help=f"Percent of rows to bulk-delete before main phase to create dead tuples (default: {DELETE_PERCENT})"
    )
    parser.add_argument(
        "--fillfactor", type=int, default=FILLFACTOR,
        help=f"Table fillfactor (lower = more pages, default: {FILLFACTOR})"
    )
    parser.add_argument(
        "--reverse", action="store_true",
        help="Delete from high end and use < operator (tests max lookup instead of default min)"
    )
    parser.add_argument(
        "--logged", action="store_true",
        help="Use standard logged table instead of UNLOGGED (default: UNLOGGED)"
    )
    parser.add_argument(
        "--planning-rate", type=float, default=500,
        help="Planning ops/sec per worker per million rows (default: 500, 0 = unlimited)"
    )
    parser.add_argument(
        "--delete-rate", type=float, default=1000,
        help="Background deletes/sec per million rows (default: 1000, 0 = disabled)"
    )
    parser.add_argument(
        "--insert-rate", type=float, default=1000,
        help="Background inserts/sec per million rows (default: 1000, 0 = disabled)"
    )

    args = parser.parse_args()

    # Compute delete_count from delete_percent
    args.delete_count = int(args.rows * args.delete_percent / 100)

    # Scale rates by rows/1M (rates are specified as "per million rows")
    scale_factor = args.rows / 1_000_000
    args.planning_rate *= scale_factor
    args.delete_rate *= scale_factor
    args.insert_rate *= scale_factor

    # Compute total explains and duration from explains_per_row
    # Total work scales with table size
    args.total_explains = int(args.rows * args.explains_per_row)
    args.baseline_explains = args.total_explains // 3  # 1/3 for baseline
    args.main_explains = args.total_explains - args.baseline_explains  # 2/3 for main phase

    # Duration is derived: duration = total_explains / (rate × workers)
    if args.planning_rate > 0:
        total_rate = args.planning_rate * args.workers
        args.baseline_duration = args.baseline_explains / total_rate
        args.main_duration = args.main_explains / total_rate
    else:
        # Unlimited rate - use minimal duration
        args.baseline_duration = 1.0
        args.main_duration = 1.0

    # Print workload summary with explanations
    print("=" * 70)
    print("VISITED_PAGES_LIMIT Stress Test")
    print("=" * 70)
    print()
    print("This stress test measures EXPLAIN query latency under a queue-like workload")
    print("where dead tuples accumulate at one end of an index. It tests whether")
    print("get_actual_variable_range() correctly aborts after visiting too many")
    print("heap pages (VISITED_PAGES_LIMIT = 100).")
    print()
    print("Workload:")
    print(f"  Table size:     {args.rows:,} rows")
    print()
    print("Test phases:")
    print(f"  1. Baseline:    {args.baseline_explains:,} EXPLAIN queries over {args.baseline_duration:.1f}s")
    print(f"                  (no dead tuples - establishes normal latency)")
    print(f"  2. Bulk delete: {args.delete_count:,} rows ({args.delete_percent}% of table)")
    print(f"                  Creates dead tuples at index start that planner must scan through")
    print(f"  3. Main:        {args.main_explains:,} EXPLAIN queries over {args.main_duration:.1f}s")
    print(f"                  (measures latency impact of dead tuples)")
    if args.delete_rate > 0 or args.insert_rate > 0:
        print(f"                  + background queue activity: {args.delete_rate:.0f} deletes/sec, {args.insert_rate:.0f} inserts/sec")
    print("=" * 70)

    if args.preview:
        return

    # Determine which versions to test
    test_configs = []
    if not args.patch_only:
        test_configs.append(("master", MASTER_BIN, MASTER_DATA_DIR, MASTER_CONN))
    if not args.master_only:
        test_configs.append(("patch", PATCH_BIN, PATCH_DATA_DIR, PATCH_CONN))

    if not test_configs:
        print("Error: --master-only and --patch-only are mutually exclusive")
        sys.exit(1)

    all_results = {}

    for name, bin_dir, data_dir, conn in test_configs:
        all_results[name] = run_test_for_version(
            name, bin_dir, data_dir, conn, args
        )

    # Print comparison summary if both versions were tested
    if len(all_results) == 2:
        print(f"\n{'='*60}")
        print("COMPARISON SUMMARY")
        print(f"{'='*60}")

        phase_labels = {
            "baseline": "1. Baseline (no dead tuples)",
            "dead_tuples": "3. Main (with bulk-deleted tuples, concurrent inserts/deletes)",
        }

        for phase in ["baseline", "dead_tuples"]:
            if phase in all_results.get("master", {}) and phase in all_results.get("patch", {}):
                master_arr = all_results["master"][phase]
                patch_arr = all_results["patch"][phase]

                print(f"\n{phase_labels[phase]}:")
                print(f"  {'Metric':<10} {'Master':>12} {'Patch':>12} {'Ratio':>10}")
                print(f"  {'-'*10} {'-'*12} {'-'*12} {'-'*10}")

                for metric, func in [
                    ("Avg", np.mean),
                    ("p95", lambda x: np.percentile(x, 95)),
                    ("p99", lambda x: np.percentile(x, 99)),
                ]:
                    m_val = func(master_arr)
                    p_val = func(patch_arr)
                    ratio = p_val / m_val if m_val > 0 else float('inf')
                    print(f"  {metric:<10} {m_val:>12.3f} {p_val:>12.3f} {ratio:>10.3f}x")


if __name__ == "__main__":
    main()
