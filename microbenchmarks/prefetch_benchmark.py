#!/usr/bin/env python3
"""
Prefetch benchmark script for PostgreSQL index scan prefetching.

Compares query performance across three configurations:
1. master (no prefetching, GUC not available)
2. patch with enable_indexscan_prefetch=off
3. patch with enable_indexscan_prefetch=on

Usage:
    ./prefetch_benchmark.py                    # Run all queries, uncached, 3 runs
    ./prefetch_benchmark.py --cached           # Run with data prewarmed
    ./prefetch_benchmark.py --queries Q1,Q2    # Run specific queries
    ./prefetch_benchmark.py --runs 5           # 5 runs per query
    ./prefetch_benchmark.py --skip-load        # Skip data loading verification
    ./prefetch_benchmark.py --no-serialize     # Exclude serialization overhead from timing
"""

import argparse
import json
import math
import os
import random
import re
import subprocess
import sys
import time
from collections import OrderedDict
from datetime import datetime, timedelta
from statistics import mean, median

import psycopg

from benchmark_common import (
    QUERIES,
    BASELINE_CONFIGS, TESTBRANCH_CONFIGS, BUILD_HAS_PREFETCH,
    DATA_LOADING_SQL, EXPECTED_TABLES,
    verify_data, load_data,
    clear_os_cache, evict_relations, prewarm_relations,
    set_gucs, reset_gucs,
    setup_tmpfs_hugepages, copy_binaries_to_tmpfs, cleanup_tmpfs,
)

# Output directory for results
OUTPUT_DIR = "prefetch_results"

# Global flag for using median vs min (set from args)
USE_MEDIAN = False


def get_representative(times):
    """Return the representative value (median or min) from a list of times."""
    if not times:
        return None
    return median(times) if USE_MEDIAN else min(times)


def get_stat_label():
    """Return the label for the representative statistic."""
    return "median" if USE_MEDIAN else "min"

# CPU pinning settings
BENCHMARK_CPU = 14

# --- Stress-test query generation probabilities ---
# These control the likelihood of various query features in randomly generated queries.
# Tune these to focus on patterns most likely to expose regressions.

STRESS_PROB_LATERAL_JOIN = 0.15        # Use LATERAL subquery (top-N per group)
STRESS_PROB_ANTI_JOIN = 0.10           # Use NOT EXISTS anti-join
STRESS_PROB_SEMI_JOIN = 0.15           # Use EXISTS semi-join
STRESS_PROB_CORRELATED_SUBQUERY = 0.10 # Correlated subquery in SELECT clause
STRESS_PROB_SELF_JOIN = 0.10           # Self-join on orders (exercises merge join mark/restore)
STRESS_PROB_FILTER_QUAL = 0.25         # Add filter qual that can't use index
STRESS_PROB_ORDER_BY = 0.70            # Include ORDER BY clause
STRESS_PROB_LIMIT = 0.50               # Include LIMIT clause (when ORDER BY present)
STRESS_PROB_AGGREGATE = 0.20           # Use count(*) or sum() aggregate
STRESS_PROB_INDEX_ONLY = 0.15          # Query only columns in index (index-only scan)
STRESS_PROB_MULTI_TABLE_JOIN = 0.30    # JOIN to dimension tables
STRESS_PROB_BACKWARDS_SCAN = 0.10      # Use DESC ordering (backwards index scan)
STRESS_PROB_IN_LIST = 0.15             # Use IN (...) instead of BETWEEN
STRESS_PROB_HIGH_SELECTIVITY = 0.30    # Very selective (few rows)
STRESS_PROB_LOW_SELECTIVITY = 0.20     # Low selectivity (many rows)
STRESS_PROB_SINGLE_DATE = 0.20         # Use exact date equality (more zero-row inner scans)
STRESS_PROB_FORCE_MERGE = 0.20         # Force MergeJoin on joins (disable hashjoin+nestloop)
STRESS_PROB_SORT_NON_INDEXED = 0.15    # ORDER BY non-indexed column (forces Sort above scan)

# Stress-test configuration
STRESS_QUERIES_PER_BATCH = 10          # Number of queries to generate per iteration
STRESS_REGRESSION_THRESHOLD = 1.05     # 5% slower = regression
STRESS_MIN_QUERY_MS = 1.5             # Discard queries slower than this (too noisy)

# QUERIES is imported from benchmark_common


# --- Readstream Test Queries ---
# Based on tomas-weird-issue-readstream.sql

READSTREAM_QUERIES = OrderedDict([
    ("RS1", {
        "name": "Readstream, readahead regression issue present (forward scan)",
        "sql": """
            SELECT * FROM t_readstream
            WHERE a BETWEEN 16150 AND 4540437
            ORDER BY a ASC
        """,
        "evict": ["t_readstream"],
        "prewarm_indexes": ["idx_readstream"],
        "prewarm_tables": ["t_readstream"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
    ("RS2", {
        "name": "Readstream, readahead regression issue not present (backward scan)",
        "sql": """
            SELECT * FROM t_readstream
            WHERE a BETWEEN 16150 AND 4540437
            ORDER BY a DESC
        """,
        "evict": ["t_readstream"],
        "prewarm_indexes": ["idx_readstream"],
        "prewarm_tables": ["t_readstream"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
])

# --- Tupdistance regression queries ---
# Isolated from readstream suite so each suite loads only its own table.

TUPDISTANCE_QUERIES = OrderedDict([
    ("TD1", {
        "name": "Tupdistance regression (backward scan), depends on 'Don't wait for already in-progress IO' patch",
        "sql": """
            SELECT * FROM t_tupdistance_new_regress
            WHERE a BETWEEN 9401 AND 2271544
            ORDER BY a DESC
        """,
        "evict": ["t_tupdistance_new_regress"],
        "prewarm_indexes": ["t_tupdistance_new_regress_idx"],
        "prewarm_tables": ["t_tupdistance_new_regress"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
    ("TD2", {
        "name": "Tupdistance regression (forward scan)",
        "sql": """
            SELECT * FROM t_tupdistance_new_regress
            WHERE a BETWEEN 9401 AND 2271544
            ORDER BY a ASC
        """,
        "evict": ["t_tupdistance_new_regress"],
        "prewarm_indexes": ["t_tupdistance_new_regress_idx"],
        "prewarm_tables": ["t_tupdistance_new_regress"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
])


# --- Random Backwards Test Queries ---
# Based on random_backwards_weird.sql

# --- Thomas Munro mid-January regression test queries ---
# Cold-run regression with fillfactor=90, semi-random tuple layout.
# MU1 (forward) relies on read stream pausing to avoid wasteful prefetch.
# MU2 (backward) does not depend on pausing and should be fine without it.

MUNRO_QUERIES = OrderedDict([
    ("MU1", {
        "name": "Thomas Munro mid-January regression, forward index scan (affected by readahead regression issue)",
        "sql": """
            SELECT a FROM t_munro
            WHERE a BETWEEN 19440 AND 2068015
            ORDER BY a ASC
        """,
        "evict": ["t_munro"],
        "prewarm_indexes": ["idx_munro"],
        "prewarm_tables": ["t_munro"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
            "enable_indexonlyscan": "off",
        },
    }),
    ("MU2", {
        "name": "Thomas Munro mid-January regression, backward index scan (not affected by readahead regression issue)",
        "sql": """
            SELECT a FROM t_munro
            WHERE a BETWEEN 19440 AND 2068015
            ORDER BY a DESC
        """,
        "evict": ["t_munro"],
        "prewarm_indexes": ["idx_munro"],
        "prewarm_tables": ["t_munro"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
            "enable_indexonlyscan": "off",
        },
    }),
])


# Cold-run regression with io_method=worker.
# DESC index on semi-random data with fillfactor=90.
WORKER_REGRESS_QUERIES = OrderedDict([
    ("WR1", {
        "name": "Worker regression, backward index scan",
        "sql": """
            SELECT * FROM (SELECT a FROM worker_regress
            WHERE a BETWEEN -4054398 AND -5823
            ORDER BY a DESC offset 1000000000)
        """,
        "evict": ["worker_regress"],
        "prewarm_indexes": ["idx_worker_regress"],
        "prewarm_tables": ["worker_regress"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
            "enable_indexonlyscan": "off",
        },
    }),
    ("WR2", {
        "name": "Worker regression, forward index scan",
        "sql": """
            SELECT * FROM (SELECT a FROM worker_regress
            WHERE a BETWEEN -4054398 AND -5823
            ORDER BY a ASC offset 1000000000)
        """,
        "evict": ["worker_regress"],
        "prewarm_indexes": ["idx_worker_regress"],
        "prewarm_tables": ["worker_regress"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
            "enable_indexonlyscan": "off",
        },
    }),
])


# --- Bookmark index-only-scan regression test queries ---
# Index-only scan on DESC index with semi-random layout, fillfactor=90.
# Uses OFFSET 1000000000 trick to force full scan without returning rows.
# RP1 = backward scan (matches index order, DESC on DESC index).
# RP2 = forward scan (opposite of index order).

REPRO_QUERIES = OrderedDict([
    ("RP1", {
        "name": "Bookmark IOS regression, backward index-only scan (DESC on DESC index)",
        "sql": """
            SELECT * FROM (SELECT a FROM bookm_ios
            WHERE a BETWEEN 12811 AND 1024954
            ORDER BY a DESC OFFSET 1000000000)
        """,
        "evict": ["bookm_ios"],
        "prewarm_indexes": ["idx_bookm_ios"],
        "prewarm_tables": ["bookm_ios"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
    ("RP2", {
        "name": "Bookmark IOS regression, forward index-only scan (ASC on DESC index)",
        "sql": """
            SELECT * FROM (SELECT a FROM bookm_ios
            WHERE a BETWEEN 12811 AND 1024954
            ORDER BY a ASC OFFSET 1000000000)
        """,
        "evict": ["bookm_ios"],
        "prewarm_indexes": ["idx_bookm_ios"],
        "prewarm_tables": ["bookm_ios"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
])


# --- UUID Test Queries ---
# UUIDv4 primary key table: physical/logical correlation ≈ 0.
# Since gen_random_uuid() produces uniformly random values, the B-tree
# index order is entirely uncorrelated with heap tuple order.  Every
# index scan degenerates into random I/O, making this an ideal workload
# for measuring index prefetching benefit.

UUID_QUERIES = OrderedDict([
    ("UU1", {
        "name": "UUID range scan, ~10% of table (random I/O, forward)",
        "sql": """
            SELECT * FROM t_uuid
            WHERE id >= '10000000-0000-0000-0000-000000000000'::uuid
              AND id <  '28000000-0000-0000-0000-000000000000'::uuid
            ORDER BY id ASC
        """,
        "evict": ["t_uuid"],
        "prewarm_indexes": ["t_uuid_pkey"],
        "prewarm_tables": ["t_uuid"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
    ("UU2", {
        "name": "UUID range scan, ~10% of table (random I/O, backward)",
        "sql": """
            SELECT * FROM t_uuid
            WHERE id >= '10000000-0000-0000-0000-000000000000'::uuid
              AND id <  '28000000-0000-0000-0000-000000000000'::uuid
            ORDER BY id DESC
        """,
        "evict": ["t_uuid"],
        "prewarm_indexes": ["t_uuid_pkey"],
        "prewarm_tables": ["t_uuid"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
    ("UU3", {
        "name": "UUID index-only scan, ~10% of table (no heap fetches if all-visible)",
        "sql": """
            SELECT id FROM t_uuid
            WHERE id >= '10000000-0000-0000-0000-000000000000'::uuid
              AND id <  '28000000-0000-0000-0000-000000000000'::uuid
            ORDER BY id ASC
        """,
        "evict": ["t_uuid"],
        "prewarm_indexes": ["t_uuid_pkey"],
        "prewarm_tables": ["t_uuid"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
    ("UU4", {
        "name": "UUID range scan with filter qual (payload column not in index)",
        "sql": """
            SELECT * FROM t_uuid
            WHERE id >= '10000000-0000-0000-0000-000000000000'::uuid
              AND id <  '28000000-0000-0000-0000-000000000000'::uuid
              AND val > 500
            ORDER BY id ASC
        """,
        "evict": ["t_uuid"],
        "prewarm_indexes": ["t_uuid_pkey"],
        "prewarm_tables": ["t_uuid"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
])


RANDOM_BACKWARDS_QUERIES = OrderedDict([
    ("RB1", {
        "name": "Sequential table forward scan",
        "sql": """
            SELECT * FROM t
            WHERE a BETWEEN 16336 AND 49103
            ORDER BY a
        """,
        "evict": ["t"],
        "prewarm_indexes": ["t_pk"],
        "prewarm_tables": ["t"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
    ("RB2", {
        "name": "Sequential table backward scan (somewhat depends on 'Don't wait for already in-progress IO' patch)",
        "sql": """
            SELECT * FROM t
            WHERE a BETWEEN 16336 AND 49103
            ORDER BY a DESC
        """,
        "evict": ["t"],
        "prewarm_indexes": ["t_pk"],
        "prewarm_tables": ["t"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
    ("RB3", {
        "name": "Randomized table forward scan",
        "sql": """
            SELECT * FROM t_randomized
            WHERE a BETWEEN 16336 AND 49103
            ORDER BY a
        """,
        "evict": ["t_randomized"],
        "prewarm_indexes": ["t_randomized_pk"],
        "prewarm_tables": ["t_randomized"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
    ("RB4", {
        "name": "Randomized table backward scan",
        "sql": """
            SELECT * FROM t_randomized
            WHERE a BETWEEN 16336 AND 49103
            ORDER BY a DESC
        """,
        "evict": ["t_randomized"],
        "prewarm_indexes": ["t_randomized_pk"],
        "prewarm_tables": ["t_randomized"],
        "gucs": {
            "enable_bitmapscan": "off",
            "enable_seqscan": "off",
        },
    }),
])


# DATA_LOADING_SQL is imported from benchmark_common


# --- Readstream Test Data Loading SQL ---
# Based on tomas-weird-issue-readstream.sql

READSTREAM_DATA_SQL = """
-- Create extensions
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

DROP TABLE IF EXISTS t_readstream CASCADE;

-- t_readstream (5M rows x 2 = 10M rows)
CREATE TABLE t_readstream (a bigint, b text) WITH (fillfactor = 20);
SELECT setseed(0.1234567890123456);
INSERT INTO t_readstream
SELECT
  1 * a,
  b
FROM (
  SELECT r, a, b, generate_series(0, 2 - 1) AS p
  FROM (
    SELECT
      row_number() OVER () AS r,
      a,
      b
    FROM (
      SELECT
        i AS a,
        md5(i::text) AS b
      FROM
        generate_series(1, 5000000) s(i)
      ORDER BY
        (i + 16 * (random() - 0.5))) foo) bar) baz
ORDER BY
  ((r * 2 + p) + 8 * (random() - 0.5));
CREATE INDEX idx_readstream ON t_readstream(a ASC) WITH (deduplicate_items=false);
VACUUM (ANALYZE, FREEZE) t_readstream;

CHECKPOINT;
"""


TUPDISTANCE_DATA_SQL = """
-- Create extensions
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

DROP TABLE IF EXISTS t_tupdistance_new_regress CASCADE;

-- t_tupdistance_new_regress (2.5M rows x 4 = 10M rows)
CREATE TABLE t_tupdistance_new_regress (a bigint, b text) WITH (fillfactor = 20);
SELECT setseed(0.2345678901234567);
INSERT INTO t_tupdistance_new_regress
SELECT 1 * a, b
FROM (
  SELECT
    r,
    a,
    b,
    generate_series(0, 4 - 1) AS p
  FROM (
    SELECT
      row_number() OVER () AS r,
      a,
      b
    FROM (
      SELECT
        i AS a,
        md5(i::text) AS b
      FROM
        generate_series(1, 2500000) s(i)
      ORDER BY
        (i + 0 * (random() - 0.5))) foo) bar) baz
ORDER BY
  ((r * 4 + p) + 8 * (random() - 0.5));
CREATE INDEX t_tupdistance_new_regress_idx ON t_tupdistance_new_regress(a DESC) WITH (deduplicate_items = false);
VACUUM ANALYZE t_tupdistance_new_regress;

CHECKPOINT;
"""



# --- Random Backwards Test Data Loading SQL ---
# Based on random_backwards_weird.sql

RANDOM_BACKWARDS_DATA_SQL = """
-- Create extensions
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

-- Drop existing tables
DROP TABLE IF EXISTS t CASCADE;
DROP TABLE IF EXISTS t_randomized CASCADE;

SET synchronize_seqscans = off;

-- Table 1: t (sequential layout, 312500 x 32 = 10M rows)
CREATE TABLE t (a bigint, b text) WITH (fillfactor = 20);
SELECT setseed(0.3456789012345678);
INSERT INTO t
SELECT a, b
FROM (SELECT
    r,
    a,
    b,
    generate_series(0, 32 - 1) AS p
  FROM (
    SELECT
      row_number() OVER () AS r,
      a,
      b
    FROM (
      SELECT
        i AS a,
        md5(i::text) AS b
      FROM
        generate_series(1, 312500) s(i)
      ORDER BY
        (i + 1 * (random() - 0.5))) foo) bar) baz
ORDER BY ((r * 32 + p) + 8 * (random() - 0.5));
CREATE INDEX t_pk ON t(a ASC) WITH (deduplicate_items=off);

-- Table 2: t_randomized (clustered by hash for random physical layout)
CREATE TABLE t_randomized (a bigint, b text) WITH (fillfactor = 20);
SELECT setseed(0.4567890123456789);
INSERT INTO t_randomized
SELECT a, b
FROM (SELECT
    r,
    a,
    b,
    generate_series(0, 32 - 1) AS p
  FROM (
    SELECT
      row_number() OVER () AS r,
      a,
      b
    FROM (
      SELECT
        i AS a,
        md5(i::text) AS b
      FROM
        generate_series(1, 312500) s(i)
      ORDER BY
        (i + 1 * (random() - 0.5))) foo) bar) baz
ORDER BY ((r * 32 + p) + 8 * (random() - 0.5));
CREATE INDEX t_randomized_pk ON t_randomized(a ASC) WITH (deduplicate_items=off);
CREATE INDEX randomizer ON t_randomized (hashint8(a));
CLUSTER t_randomized USING randomizer;

VACUUM FREEZE;
ANALYZE;
CHECKPOINT;
"""


# --- Thomas Munro mid-January regression data loading SQL ---

MUNRO_DATA_SQL = """
-- Create extensions
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

-- Drop existing table
DROP TABLE IF EXISTS t_munro CASCADE;

-- Table: t_munro (2.5M rows x 4 = 10M rows, fillfactor=90)
CREATE TABLE t_munro (a bigint, b text) WITH (fillfactor = 90, autovacuum_enabled = false);
SELECT setseed(0.6789012345678901);
INSERT INTO t_munro
SELECT 1 * a, b
FROM (
  SELECT r, a, b, generate_series(0, 4 - 1) AS p
  FROM (
    SELECT
      row_number() OVER () AS r,
      a,
      b
    FROM (
      SELECT
        i AS a,
        md5(i::text) AS b
      FROM
        generate_series(1, 2500000) s(i)
      ORDER BY
        (i + 16384 * (random() - 0.5))) foo) bar) baz
ORDER BY ((r * 4 + p) + 32 * (random() - 0.5));
CREATE INDEX idx_munro ON t_munro(a ASC) WITH (deduplicate_items=false);
VACUUM ANALYZE t_munro;

CHECKPOINT;
"""


# --- Worker regression data loading SQL ---

WORKER_REGRESS_DATA_SQL = """
-- Create extensions
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

-- Drop existing table
DROP TABLE IF EXISTS worker_regress CASCADE;

-- Table: worker_regress (5M rows, fillfactor=90)
SELECT setseed(.00003612716763005780);
CREATE TABLE worker_regress (a bigint, b text) WITH (fillfactor = 90, autovacuum_enabled = false);
INSERT INTO worker_regress
SELECT -1 * a, b
FROM (
  SELECT r, a, b, generate_series(0, 2 - 1) AS p
  FROM (
    SELECT
      row_number() OVER () AS r,
      a,
      b
    FROM (
      SELECT
        i AS a,
        md5(i::text) AS b
      FROM
        generate_series(1, 5000000) s(i)
      ORDER BY
        (i + 1024 * (random() - 0.5))) foo) bar) baz
ORDER BY ((r * 2 + p) + 2 * (random() - 0.5));
CREATE INDEX idx_worker_regress ON worker_regress(a DESC) WITH (deduplicate_items=false);
VACUUM ANALYZE worker_regress;

CHECKPOINT;
"""


# --- Bookmark index-only-scan regression data loading SQL ---

REPRO_DATA_SQL = """
-- Create extensions
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

-- Drop existing table
DROP TABLE IF EXISTS bookm_ios CASCADE;

-- Table: bookm_ios (1.25M rows x 8 = 10M rows, fillfactor=90)
CREATE UNLOGGED TABLE bookm_ios (a bigint, b text) WITH (fillfactor = 90, autovacuum_enabled = false);
SELECT setseed(0.00008086035417);
INSERT INTO bookm_ios
SELECT 1 * a, b
FROM (
  SELECT r, a, b, generate_series(0, 8 - 1) AS p
  FROM (
    SELECT
      row_number() OVER () AS r,
      a,
      b
    FROM (
      SELECT
        i AS a,
        md5(i::text) AS b
      FROM
        generate_series(1, 1250000) s(i)
      ORDER BY
        (i + 1 * (random() - 0.5))) foo) bar) baz
ORDER BY ((r * 8 + p) + 2 * (random() - 0.5));
CREATE INDEX idx_bookm_ios ON bookm_ios(a DESC);
VACUUM ANALYZE bookm_ios;

-- Dirty ~0.05% of rows to force heap fetches on index-only scans
SELECT setseed(0.00008086035417);
UPDATE bookm_ios SET a = a WHERE random() < 1.0 / 2017.0;

CHECKPOINT;
"""


# --- UUID benchmark data loading SQL ---

UUID_DATA_SQL = """
-- Create extensions
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

DROP TABLE IF EXISTS t_uuid CASCADE;

-- t_uuid: 5M rows with deterministic UUID primary key.
-- md5(i::text)::uuid produces deterministic values whose sort order
-- is uncorrelated with the sequential insertion order, so
-- pg_stats.correlation ≈ 0.  This maximises random I/O during
-- index scans, which is the ideal scenario for prefetching.
-- Column "val" is a deterministic integer payload so filter quals
-- can be tested, and "payload" adds width to force heap fetches.
CREATE TABLE t_uuid (
    id uuid NOT NULL,
    val integer NOT NULL,
    payload text NOT NULL
);
SELECT setseed(0.5678901234567890);
INSERT INTO t_uuid (id, val, payload)
SELECT md5(i::text)::uuid,
       (random() * 1000)::integer,
       md5((i * 3)::text)
FROM generate_series(1, 5000000) s(i);
ALTER TABLE t_uuid ADD CONSTRAINT t_uuid_pkey PRIMARY KEY (id);
VACUUM (ANALYZE, FREEZE) t_uuid;

CHECKPOINT;
"""


def parse_arguments():
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description="Prefetch benchmark for PostgreSQL index scan prefetching.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    %(prog)s                         # Run all queries, uncached, 3 runs
    %(prog)s --cached                # Run with data prewarmed
    %(prog)s --queries Q1,Q2,A1      # Run specific queries
    %(prog)s --runs 5                # 5 runs per query
    %(prog)s --skip-load             # Skip data loading verification
        """
    )
    parser.add_argument(
        "--cached",
        action="store_true",
        help="Run in cached mode (prewarm all relations instead of just indexes)"
    )
    parser.add_argument(
        "--queries",
        type=str,
        default=None,
        help="Comma-separated list of queries to run (e.g., Q1,Q2,A1). Default: all"
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=3,
        help="Number of runs per query (default: 3)"
    )
    parser.add_argument(
        "--skip-load",
        action="store_true",
        dest="skip_load",
        help="Skip data loading verification (assume tables exist with correct data)"
    )
    parser.add_argument(
        "--benchmark-cpu",
        type=int,
        default=BENCHMARK_CPU,
        help=f"CPU core to pin PostgreSQL backend to (default: {BENCHMARK_CPU})"
    )
    parser.add_argument(
        "--pin",
        action="store_true",
        dest="pin",
        help="Enable CPU pinning and SCHED_FIFO for the backend process (off by default, can starve io_uring)"
    )
    parser.add_argument(
        "--topn",
        type=int,
        default=20,
        help="Number of top improvements/regressions to show (default: 20)"
    )
    prefetch_group = parser.add_mutually_exclusive_group()
    prefetch_group.add_argument(
        "--prefetch-only",
        action="store_true",
        dest="prefetch_only",
        help="Only test patch with prefetching enabled (skip prefetch=off)"
    )
    prefetch_group.add_argument(
        "--prefetch-disabled",
        action="store_true",
        dest="prefetch_disabled",
        help="Only test patch with prefetching disabled (skip prefetch=on)"
    )
    parser.add_argument(
        "--stress-test",
        action="store_true",
        dest="stress_test",
        help="Run stress test mode: randomly generate queries to find regressions"
    )
    parser.add_argument(
        "--min-query-ms",
        type=float,
        default=STRESS_MIN_QUERY_MS,
        dest="min_query_ms",
        help=f"Minimum query duration in ms for stress test (default: {STRESS_MIN_QUERY_MS}). "
             "Queries slower than this on master are discarded as too noisy."
    )
    parser.add_argument(
        "--no-tmpfs-hugepages",
        action="store_true",
        help="Disable tmpfs with huge=always (enabled by default)"
    )
    # Suite-selection flags (auto-generated from BENCHMARK_SUITES registry)
    for suite in BENCHMARK_SUITES:
        if suite["cli_flag"] is not None:
            parser.add_argument(
                suite["cli_flag"],
                action="store_true",
                dest=suite["cli_dest"],
                help=suite["help"],
            )
    parser.add_argument(
        "--list-modes",
        action="store_true",
        dest="list_modes",
        help="Output benchmark suite metadata as JSON (used by orchestration scripts)"
    )
    parser.add_argument(
        "--effective_io_concurrency",
        type=int,
        default=100,
        help="Value for effective_io_concurrency PostgreSQL setting (default: 100)"
    )
    parser.add_argument(
        "--io_combine_limit",
        type=int,
        default=16,
        help="Value for io_combine_limit in 8kB units (default: 16 = 128KB)"
    )
    parser.add_argument(
        "--io_max_combine_limit",
        type=int,
        default=128,
        help="Value for io_max_combine_limit in 8kB units (default: 128 = 1MB)"
    )
    parser.add_argument(
        "--io_max_concurrency",
        type=int,
        default=64,
        help="Value for io_max_concurrency PostgreSQL setting (default: 64)"
    )
    parser.add_argument(
        "--io_workers",
        type=int,
        default=3,
        help="Value for io_workers PostgreSQL setting (default: 3)"
    )
    parser.add_argument(
        "--io_method",
        type=str,
        default="io_uring",
        help="Value for io_method PostgreSQL setting (default: io_uring)"
    )
    parser.add_argument(
        "--delay-pgdata",
        action="store_true",
        dest="delay_pgdata",
        help="Use data directories with simulated I/O delay (data-delay instead of data)"
    )
    parser.add_argument(
        "--old-master-results",
        action="store_true",
        dest="old_master_results",
        help="Use baseline results from most recent benchmark run instead of re-running baseline"
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
    parser.add_argument(
        "--exclude-ms",
        type=float,
        default=None,
        dest="exclude_ms",
        help="Exclude queries whose master runtime (min/median) is below this threshold in ms from patch runs and rankings"
    )
    parser.add_argument(
        "--median",
        action="store_true",
        dest="use_median",
        help="Use median instead of min as the representative value in reports and comparisons"
    )
    parser.add_argument(
        "--terse",
        action="store_true",
        help="Suppress per-run times and per-query detail; print only header, verification, and summary"
    )
    parser.add_argument(
        "--no-serialize",
        action="store_true",
        dest="no_serialize",
        help="Disable SERIALIZE in EXPLAIN ANALYZE options (serialization is included by default)"
    )
    parser.add_argument(
        "--direct-io",
        action="store_true",
        dest="direct_io",
        help="Start Postgres with debug_io_direct=data and skip clear_cache.sh (O_DIRECT bypasses OS page cache)"
    )
    return parser.parse_args()


# --- Stress-test query generation ---

def random_date_range():
    """Generate a random date range within 2023."""
    # Start date: random day in 2023
    start_day = random.randint(1, 330)  # Leave room for range
    start_date = datetime(2023, 1, 1) + timedelta(days=start_day)

    # Determine range size based on selectivity
    if random.random() < STRESS_PROB_HIGH_SELECTIVITY:
        # Narrow range: 1-3 days (high selectivity)
        days = random.randint(1, 3)
    elif random.random() < STRESS_PROB_LOW_SELECTIVITY:
        # Wide range: 30-90 days (low selectivity)
        days = random.randint(30, 90)
    else:
        # Medium range: 5-20 days
        days = random.randint(5, 20)

    end_date = start_date + timedelta(days=days)
    return start_date.strftime('%Y-%m-%d'), end_date.strftime('%Y-%m-%d')


def random_customer_range():
    """Generate a random customer_id range."""
    if random.random() < STRESS_PROB_HIGH_SELECTIVITY:
        # Single customer or very small range
        start = random.randint(1, 100000)
        count = random.randint(1, 5)
    elif random.random() < STRESS_PROB_LOW_SELECTIVITY:
        # Large range: 5000-20000 customers
        start = random.randint(1, 80000)
        count = random.randint(5000, 20000)
    else:
        # Medium range: 100-1000 customers
        start = random.randint(1, 99000)
        count = random.randint(100, 1000)

    end = min(start + count, 100000)
    return start, end


def random_product_range():
    """Generate a random product_id range."""
    if random.random() < STRESS_PROB_HIGH_SELECTIVITY:
        start = random.randint(1, 10000)
        count = random.randint(1, 10)
    elif random.random() < STRESS_PROB_LOW_SELECTIVITY:
        start = random.randint(1, 5000)
        count = random.randint(2000, 5000)
    else:
        start = random.randint(1, 9000)
        count = random.randint(50, 500)

    end = min(start + count, 10000)
    return start, end


def random_in_list(start, end, max_items=10):
    """Generate a random IN list from a range."""
    count = min(random.randint(3, max_items), end - start + 1)
    values = random.sample(range(start, end + 1), count)
    return ', '.join(str(v) for v in sorted(values))


def random_filter_qual(table_alias=""):
    """Generate a random filter qual that can't use the index."""
    prefix = f"{table_alias}." if table_alias else ""
    qual_type = random.choice(['amount_gt', 'amount_between', 'region_in'])

    if qual_type == 'amount_gt':
        threshold = random.randint(100, 900)
        return f"{prefix}amount > {threshold}"
    elif qual_type == 'amount_between':
        low = random.randint(0, 500)
        high = low + random.randint(100, 400)
        return f"{prefix}amount BETWEEN {low} AND {high}"
    else:  # region_in
        regions = random.sample(range(1, 21), random.randint(2, 5))
        return f"{prefix}region_id IN ({', '.join(str(r) for r in regions)})"


def random_limit():
    """Generate a random LIMIT value."""
    if random.random() < 0.3:
        return random.randint(1, 10)  # Very small
    elif random.random() < 0.5:
        return random.randint(100, 1000)  # Medium
    else:
        return random.randint(5000, 50000)  # Large


def generate_random_query(query_num):
    """
    Generate a random query targeting the prefetch benchmark tables.
    Returns a query definition dict compatible with the QUERIES format.
    """
    # Decide query type based on probabilities
    # These are mutually exclusive special patterns
    use_lateral = random.random() < STRESS_PROB_LATERAL_JOIN
    use_anti_join = not use_lateral and random.random() < STRESS_PROB_ANTI_JOIN
    use_semi_join = not use_lateral and not use_anti_join and random.random() < STRESS_PROB_SEMI_JOIN
    use_correlated = not use_lateral and not use_anti_join and not use_semi_join and random.random() < STRESS_PROB_CORRELATED_SUBQUERY
    use_self_join = not use_lateral and not use_anti_join and not use_semi_join and not use_correlated and random.random() < STRESS_PROB_SELF_JOIN
    use_aggregate = not use_lateral and not use_self_join and random.random() < STRESS_PROB_AGGREGATE

    # Independent features
    use_filter_qual = random.random() < STRESS_PROB_FILTER_QUAL
    use_order_by = random.random() < STRESS_PROB_ORDER_BY
    use_limit = use_order_by and random.random() < STRESS_PROB_LIMIT
    use_backwards = use_order_by and random.random() < STRESS_PROB_BACKWARDS_SCAN
    use_in_list = random.random() < STRESS_PROB_IN_LIST
    use_index_only = random.random() < STRESS_PROB_INDEX_ONLY
    use_multi_join = random.random() < STRESS_PROB_MULTI_TABLE_JOIN
    use_single_date = random.random() < STRESS_PROB_SINGLE_DATE
    use_force_merge = random.random() < STRESS_PROB_FORCE_MERGE
    use_sort_non_indexed = use_order_by and random.random() < STRESS_PROB_SORT_NON_INDEXED

    # Generate date range (used in most queries)
    date_start, date_end = random_date_range()
    cust_start, cust_end = random_customer_range()
    prod_start, prod_end = random_product_range()

    # Build date condition: single-date equality or BETWEEN range.
    # Single-date creates more zero-row inner scans (only ~0.7 orders/customer/day).
    if use_single_date:
        date_cond = f"order_date = '{date_start}'"
        date_cond_aliased = lambda alias: f"{alias}.order_date = '{date_start}'"
    else:
        date_cond = f"order_date BETWEEN '{date_start}' AND '{date_end}'"
        date_cond_aliased = lambda alias: f"{alias}.order_date BETWEEN '{date_start}' AND '{date_end}'"

    # Build query based on type
    evict = ["prefetch_orders"]
    prewarm_indexes = []
    prewarm_tables = ["prefetch_orders"]
    query_features = []
    gucs = {}

    if use_lateral:
        # LATERAL join: top-N per customer
        query_features.append("LATERAL")
        limit_val = random.randint(3, 10)
        order_dir = "DESC" if use_backwards else ""

        if use_in_list and (cust_end - cust_start) <= 20:
            cust_cond = f"c.customer_id IN ({random_in_list(cust_start, cust_end)})"
        else:
            cust_cond = f"c.customer_id BETWEEN {cust_start} AND {cust_end}"

        inner_filter = ""
        if use_filter_qual:
            # Inside LATERAL subquery, prefetch_orders has no alias, so no prefix needed
            # But avoid region_id which would be ambiguous - use amount only
            qual_type = random.choice(['amount_gt', 'amount_between'])
            if qual_type == 'amount_gt':
                threshold = random.randint(100, 900)
                inner_filter = f"AND amount > {threshold}"
            else:
                low = random.randint(0, 500)
                high = low + random.randint(100, 400)
                inner_filter = f"AND amount BETWEEN {low} AND {high}"
            query_features.append("filter")

        sql = f"""
            SELECT c.customer_id, o.order_id, o.order_date, o.amount
            FROM prefetch_customers c,
            LATERAL (
                SELECT order_id, order_date, amount
                FROM prefetch_orders
                WHERE customer_id = c.customer_id
                  AND {date_cond}
                  {inner_filter}
                ORDER BY order_date {order_dir}
                LIMIT {limit_val}
            ) o
            WHERE {cust_cond}
        """
        evict.append("prefetch_customers")
        prewarm_indexes = ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"]
        prewarm_tables.append("prefetch_customers")

    elif use_anti_join:
        # NOT EXISTS anti-join
        query_features.append("anti-join")

        if use_in_list and (cust_end - cust_start) <= 20:
            cust_cond = f"o.customer_id IN ({random_in_list(cust_start, cust_end)})"
        else:
            cust_cond = f"o.customer_id BETWEEN {cust_start} AND {cust_end}"

        region_id = random.randint(1, 20)

        select_cols = "o.customer_id, o.order_date" if use_index_only else "o.order_id, o.customer_id, o.amount"
        if use_index_only:
            query_features.append("index-only")

        filter_clause = ""
        if use_filter_qual and not use_index_only:
            filter_clause = f"AND {random_filter_qual('o')}"
            query_features.append("filter")

        sql = f"""
            SELECT {select_cols}
            FROM prefetch_orders o
            WHERE {date_cond_aliased('o')}
              AND {cust_cond}
              AND NOT EXISTS (
                  SELECT 1 FROM prefetch_customers c
                  WHERE c.customer_id = o.customer_id
                    AND c.region_id = {region_id}
              )
              {filter_clause}
        """
        evict.append("prefetch_customers")
        prewarm_indexes = ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"]
        prewarm_tables.append("prefetch_customers")

        if use_order_by:
            order_dir = "DESC" if use_backwards else ""
            order_col = "o.amount" if use_sort_non_indexed else "o.order_date"
            sql = sql.rstrip() + f"\n            ORDER BY {order_col} {order_dir}"
            if use_limit:
                sql += f"\n            LIMIT {random_limit()}"

    elif use_semi_join:
        # EXISTS semi-join
        query_features.append("semi-join")

        region_id = random.randint(1, 20)

        select_cols = "o.customer_id, o.order_date" if use_index_only else "o.order_id, o.customer_id, o.amount"
        if use_index_only:
            query_features.append("index-only")

        filter_clause = ""
        if use_filter_qual and not use_index_only:
            filter_clause = f"AND {random_filter_qual('o')}"
            query_features.append("filter")

        sql = f"""
            SELECT {select_cols}
            FROM prefetch_orders o
            WHERE {date_cond_aliased('o')}
              AND EXISTS (
                  SELECT 1 FROM prefetch_customers c
                  WHERE c.customer_id = o.customer_id
                    AND c.region_id = {region_id}
              )
              {filter_clause}
        """
        evict.append("prefetch_customers")
        prewarm_indexes = ["prefetch_orders_date_idx", "prefetch_customers_pkey"]
        prewarm_tables.append("prefetch_customers")

        if use_order_by:
            order_dir = "DESC" if use_backwards else ""
            order_col = "o.amount" if use_sort_non_indexed else "o.order_date"
            sql = sql.rstrip() + f"\n            ORDER BY {order_col} {order_dir}"
            if use_limit:
                sql += f"\n            LIMIT {random_limit()}"

    elif use_correlated:
        # Correlated subquery in SELECT
        query_features.append("correlated")

        if use_in_list and (cust_end - cust_start) <= 20:
            cust_cond = f"o.customer_id IN ({random_in_list(cust_start, cust_end)})"
        else:
            cust_cond = f"o.customer_id BETWEEN {cust_start} AND {cust_end}"

        filter_clause = ""
        if use_filter_qual:
            filter_clause = f"AND {random_filter_qual('o')}"
            query_features.append("filter")

        sql = f"""
            SELECT o.order_id, o.customer_id, o.amount,
                   (SELECT c.customer_name FROM prefetch_customers c
                    WHERE c.customer_id = o.customer_id) as cust_name
            FROM prefetch_orders o
            WHERE {date_cond_aliased('o')}
              AND {cust_cond}
              {filter_clause}
        """
        evict.append("prefetch_customers")
        prewarm_indexes = ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"]
        prewarm_tables.append("prefetch_customers")

        if use_order_by:
            order_dir = "DESC" if use_backwards else ""
            order_col = "o.amount" if use_sort_non_indexed else "o.order_date"
            sql = sql.rstrip() + f"\n            ORDER BY {order_col} {order_dir}"
            if use_limit:
                sql += f"\n            LIMIT {random_limit()}"

    elif use_aggregate:
        # Aggregate query
        query_features.append("aggregate")
        agg_type = random.choice(['count', 'sum', 'both'])

        if agg_type == 'count':
            select_clause = "order_date, count(*) as cnt"
        elif agg_type == 'sum':
            select_clause = "order_date, sum(amount) as total"
        else:
            select_clause = "order_date, count(*) as cnt, sum(amount) as total"

        filter_clause = ""
        if use_filter_qual:
            # No table alias in aggregate query, so no prefix needed
            # Avoid region_id ambiguity by only using amount
            qual_type = random.choice(['amount_gt', 'amount_between'])
            if qual_type == 'amount_gt':
                threshold = random.randint(100, 900)
                filter_clause = f"AND amount > {threshold}"
            else:
                low = random.randint(0, 500)
                high = low + random.randint(100, 400)
                filter_clause = f"AND amount BETWEEN {low} AND {high}"
            query_features.append("filter")

        sql = f"""
            SELECT {select_clause}
            FROM prefetch_orders
            WHERE {date_cond}
              {filter_clause}
            GROUP BY order_date
        """
        prewarm_indexes = ["prefetch_orders_date_idx"]

        if use_order_by:
            order_dir = "DESC" if use_backwards else ""
            # Aggregate GROUP BY order_date: sort-non-indexed not applicable
            sql = sql.rstrip() + f"\n            ORDER BY order_date {order_dir}"

    elif use_multi_join:
        # Multi-table JOIN
        query_features.append("JOIN")
        join_to = random.choice(['customers', 'products', 'both'])

        if use_in_list and (cust_end - cust_start) <= 20:
            cust_cond = f"o.customer_id IN ({random_in_list(cust_start, cust_end)})"
        else:
            cust_cond = f"o.customer_id BETWEEN {cust_start} AND {cust_end}"

        filter_clause = ""
        if use_filter_qual:
            filter_clause = f"AND {random_filter_qual('o')}"
            query_features.append("filter")

        if join_to == 'customers':
            sql = f"""
                SELECT o.order_id, c.customer_name, o.amount, o.order_date
                FROM prefetch_orders o
                JOIN prefetch_customers c ON c.customer_id = o.customer_id
                WHERE {cust_cond}
                  AND {date_cond_aliased('o')}
                  {filter_clause}
            """
            evict.append("prefetch_customers")
            prewarm_indexes = ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"]
            prewarm_tables.append("prefetch_customers")
        elif join_to == 'products':
            sql = f"""
                SELECT o.order_id, p.product_name, o.amount, o.order_date
                FROM prefetch_orders o
                JOIN prefetch_products p ON p.product_id = o.product_id
                WHERE o.product_id BETWEEN {prod_start} AND {prod_end}
                  AND {date_cond_aliased('o')}
                  {filter_clause}
            """
            evict.append("prefetch_products")
            prewarm_indexes = ["prefetch_orders_prod_idx", "prefetch_products_pkey"]
            prewarm_tables.append("prefetch_products")
        else:  # both
            query_features.append("multi-JOIN")
            sql = f"""
                SELECT o.order_id, c.customer_name, p.product_name, o.amount
                FROM prefetch_orders o
                JOIN prefetch_customers c ON c.customer_id = o.customer_id
                JOIN prefetch_products p ON p.product_id = o.product_id
                WHERE {date_cond_aliased('o')}
                  AND {cust_cond}
                  {filter_clause}
            """
            evict.extend(["prefetch_customers", "prefetch_products"])
            prewarm_indexes = ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey", "prefetch_products_pkey"]
            prewarm_tables.extend(["prefetch_customers", "prefetch_products"])

        if use_order_by:
            order_dir = "DESC" if use_backwards else ""
            order_col = "o.amount" if use_sort_non_indexed else "o.order_date"
            sql = sql.rstrip() + f"\n                ORDER BY {order_col} {order_dir}"
            if use_limit:
                sql += f"\n                LIMIT {random_limit()}"

        # Force merge join on multi-table JOINs
        if use_force_merge:
            gucs["enable_hashjoin"] = "off"
            gucs["enable_nestloop"] = "off"
            query_features.append("merge-forced")

    elif use_self_join:
        # Self-join on orders: both sides use same index, can trigger
        # MergeJoin mark/restore when outer has duplicate join keys.
        query_features.append("self-join")

        # Generate two separate date ranges for the two sides
        date_start2, date_end2 = random_date_range()

        if use_in_list and (cust_end - cust_start) <= 20:
            cust_cond = f"o1.customer_id IN ({random_in_list(cust_start, cust_end)})"
        else:
            cust_cond = f"o1.customer_id BETWEEN {cust_start} AND {cust_end}"

        date_cond1 = (f"o1.order_date = '{date_start}'"
                      if use_single_date else
                      f"o1.order_date BETWEEN '{date_start}' AND '{date_end}'")
        date_cond2 = (f"o2.order_date = '{date_start2}'"
                      if use_single_date else
                      f"o2.order_date BETWEEN '{date_start2}' AND '{date_end2}'")
        if use_single_date:
            query_features.append("single-date")

        sql = f"""
            SELECT count(*)
            FROM prefetch_orders o1
            JOIN prefetch_orders o2 ON o1.customer_id = o2.customer_id
            WHERE {cust_cond}
              AND {date_cond1}
              AND {date_cond2}
        """
        prewarm_indexes = ["prefetch_orders_cust_date_idx"]

        # Always force merge join on self-joins — that's the point
        gucs["enable_hashjoin"] = "off"
        gucs["enable_nestloop"] = "off"
        query_features.append("merge-forced")

    else:
        # Simple range scan on prefetch_orders
        query_features.append("range-scan")

        # Choose which index to target
        index_choice = random.choice(['date', 'cust_date', 'product'])

        if use_index_only:
            query_features.append("index-only")

        filter_clause = ""
        if use_filter_qual and not use_index_only:
            # No table alias in simple range scan, avoid region_id ambiguity
            qual_type = random.choice(['amount_gt', 'amount_between'])
            if qual_type == 'amount_gt':
                threshold = random.randint(100, 900)
                filter_clause = f"AND amount > {threshold}"
            else:
                low = random.randint(0, 500)
                high = low + random.randint(100, 400)
                filter_clause = f"AND amount BETWEEN {low} AND {high}"
            query_features.append("filter")

        if index_choice == 'date':
            select_cols = "order_date" if use_index_only else "order_id, customer_id, amount"
            sql = f"""
                SELECT {select_cols}
                FROM prefetch_orders
                WHERE {date_cond}
                  {filter_clause}
            """
            prewarm_indexes = ["prefetch_orders_date_idx"]

        elif index_choice == 'cust_date':
            if use_in_list and (cust_end - cust_start) <= 20:
                cust_cond = f"customer_id IN ({random_in_list(cust_start, cust_end)})"
            else:
                cust_cond = f"customer_id BETWEEN {cust_start} AND {cust_end}"

            select_cols = "customer_id, order_date" if use_index_only else "order_id, customer_id, amount"
            sql = f"""
                SELECT {select_cols}
                FROM prefetch_orders
                WHERE {cust_cond}
                  AND {date_cond}
                  {filter_clause}
            """
            prewarm_indexes = ["prefetch_orders_cust_date_idx"]

        else:  # product
            if use_in_list and (prod_end - prod_start) <= 20:
                prod_cond = f"product_id IN ({random_in_list(prod_start, prod_end)})"
            else:
                prod_cond = f"product_id BETWEEN {prod_start} AND {prod_end}"

            select_cols = "product_id" if use_index_only else "order_id, product_id, amount"
            sql = f"""
                SELECT {select_cols}
                FROM prefetch_orders
                WHERE {prod_cond}
                  {filter_clause}
            """
            prewarm_indexes = ["prefetch_orders_prod_idx"]

        if use_order_by:
            if use_sort_non_indexed:
                order_col = "amount"
            else:
                order_col = "order_date" if index_choice in ['date', 'cust_date'] else "product_id"
            order_dir = "DESC" if use_backwards else ""
            sql = sql.rstrip() + f"\n                ORDER BY {order_col} {order_dir}"
            if use_limit:
                sql += f"\n                LIMIT {random_limit()}"

    # Add feature tags for independent modifiers
    if use_backwards:
        query_features.append("backwards")
    if use_single_date and not use_self_join:  # self-join already tags it
        query_features.append("single-date")
    if use_sort_non_indexed:
        query_features.append("sort-non-indexed")

    # Build name from features
    name = f"Stress #{query_num}: {', '.join(query_features)}"

    result = {
        "name": name,
        "sql": sql,
        "evict": list(set(evict)),  # Remove duplicates
        "prewarm_indexes": prewarm_indexes,
        "prewarm_tables": list(set(prewarm_tables)),
    }
    if gucs:
        result["gucs"] = gucs
    return result


def get_git_hash(source_dir):
    """Get the current git commit hash for a source directory."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=source_dir,
            capture_output=True,
            text=True,
            check=True
        )
        return result.stdout.strip()
    except subprocess.CalledProcessError:
        return "unknown"


def get_pg_version(conn_details):
    """Get PostgreSQL version string."""
    try:
        conn = psycopg.connect(**conn_details, connect_timeout=5)
        with conn.cursor() as cur:
            cur.execute("SELECT version()")
            version = cur.fetchone()[0]
        conn.close()
        return version
    except Exception as e:
        return f"error: {e}"


# setup_tmpfs_hugepages, copy_binaries_to_tmpfs, cleanup_tmpfs are imported from benchmark_common


def print_io_settings(args):
    """Print the PostgreSQL I/O settings that will be used."""
    print(f"\nPostgreSQL I/O settings:")
    print(f"  effective_io_concurrency = {args.effective_io_concurrency}")
    print(f"  io_combine_limit = {args.io_combine_limit} (8kB units)")
    print(f"  io_max_combine_limit = {args.io_max_combine_limit} (8kB units)")
    print(f"  io_max_concurrency = {args.io_max_concurrency}")
    print(f"  io_workers = {args.io_workers}")
    print(f"  io_method = {args.io_method}")
    if args.direct_io:
        print(f"  debug_io_direct = data")


def start_server(pg_bin_dir, pg_name, pg_data_dir, conn_details, args):
    """Start a PostgreSQL server and wait for it to be ready."""
    pg_ctl_path = os.path.join(pg_bin_dir, "pg_ctl")
    log_file = os.path.join(OUTPUT_DIR, f"{pg_name}.postgres_log")

    # Ensure server is stopped before we start
    result = subprocess.run(
        [pg_ctl_path, "status", "-D", pg_data_dir],
        capture_output=True,
        check=False
    )
    if result.returncode == 0:
        print(f"{pg_name}: Server is already running. Stopping it...")
        subprocess.run([pg_ctl_path, "stop", "-D", pg_data_dir, "-m", "fast"], check=True)
        time.sleep(2)

    # Start the server
    print(f"Starting {pg_name} PostgreSQL server (port {conn_details.get('port', 'default')})...")
    start_options = f"-p {conn_details['port']}" if 'port' in conn_details else ""

    # Build PostgreSQL configuration options
    pg_options = [
        "--autovacuum=off",
        "-c wal_level=minimal",
        "-c max_wal_senders=0",
        f"-c effective_io_concurrency={args.effective_io_concurrency}",
        f"-c io_combine_limit={args.io_combine_limit}",
        f"-c io_max_combine_limit={args.io_max_combine_limit}",
        f"-c io_max_concurrency={args.io_max_concurrency}",
        f"-c io_workers={args.io_workers}",
        f"-c io_method={args.io_method}",
    ]
    if args.direct_io:
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
        sys.exit(1)

    # Wait for the server to be ready
    for attempt in range(15):
        try:
            conn = psycopg.connect(**conn_details, connect_timeout=2)
            conn.close()
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


# verify_data and load_data are imported from benchmark_common


def _verify_tables(conn_details, tables, suite_name, check_all_visible=True):
    """Verify that tables exist, have data, and (optionally) are fully all-visible."""
    try:
        conn = psycopg.connect(**conn_details)
        conn.autocommit = True
        if check_all_visible:
            with conn.cursor() as cur:
                cur.execute("CREATE EXTENSION IF NOT EXISTS pg_visibility")
        for table in tables:
            with conn.cursor() as cur:
                cur.execute("""
                    SELECT EXISTS (
                        SELECT 1 FROM pg_class WHERE relname = %s AND relkind = 'r'
                    )
                """, (table,))
                if not cur.fetchone()[0]:
                    print(f"Table {table} does not exist")
                    conn.close()
                    return False
                cur.execute(f"SELECT EXISTS (SELECT 1 FROM {table} LIMIT 1)")
                if not cur.fetchone()[0]:
                    print(f"Table {table} exists but has 0 rows")
                    conn.close()
                    return False
                if check_all_visible:
                    cur.execute("""
                        SELECT c.relpages,
                               (SELECT all_visible
                                FROM pg_visibility_map_summary(%s::regclass))
                        FROM pg_class c
                        WHERE c.relname = %s AND c.relkind = 'r'
                    """, (table, table))
                    row = cur.fetchone()
                    if row:
                        relpages, all_visible = row
                        if relpages != all_visible:
                            conn.close()
                            sys.exit(f"ERROR: Table {table}: only {all_visible}/{relpages} "
                                     f"pages are all-visible (expected all). "
                                     f"Run VACUUM on the table or reload the data.")
        conn.close()
        if check_all_visible:
            print(f"All tables exist, have data, and are all-visible ✓")
        else:
            print(f"All tables exist and have data ✓")
        return True
    except Exception as e:
        print(f"Error verifying {suite_name} data: {e}")
        return False


def _ensure_all_visible_after_load(conn_details, tables):
    """After loading, verify all pages are all-visible; VACUUM FREEZE if not.

    Disconnects, waits one second, reconnects, then checks the visibility map
    for each table.  If any page is not all-visible, runs an unqualified
    VACUUM FREEZE and rechecks.
    """
    time.sleep(1)
    conn = psycopg.connect(**conn_details)
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute("CREATE EXTENSION IF NOT EXISTS pg_visibility")

    needs_vacuum = False
    for table in tables:
        with conn.cursor() as cur:
            cur.execute("""
                SELECT c.relpages,
                       (SELECT all_visible
                        FROM pg_visibility_map_summary(%s::regclass))
                FROM pg_class c
                WHERE c.relname = %s AND c.relkind = 'r'
            """, (table, table))
            row = cur.fetchone()
            if row:
                relpages, all_visible = row
                if relpages != all_visible:
                    print(f"Table {table}: {all_visible}/{relpages} pages "
                          f"all-visible after load")
                    needs_vacuum = True

    if needs_vacuum:
        print("Running VACUUM FREEZE to set all pages all-visible...")
        with conn.cursor() as cur:
            cur.execute("VACUUM FREEZE")
        print("VACUUM FREEZE complete.")

    conn.close()


def _load_sql(conn_details, data_sql, suite_name, row_description):
    """Execute a data-loading SQL script with progress messages."""
    print("\n" + "=" * 50)
    print(f"Loading {suite_name} benchmark data...")
    print(f"This will take several minutes for {row_description}.")
    print("=" * 50 + "\n")

    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    statements = []
    current_stmt = []
    for line in data_sql.split('\n'):
        stripped = line.strip()
        if stripped.startswith('--') or not stripped:
            continue
        current_stmt.append(line)
        if stripped.endswith(';'):
            statements.append('\n'.join(current_stmt))
            current_stmt = []

    for statement in statements:
        statement = statement.strip()
        if not statement:
            continue
        try:
            if 'INSERT INTO' in statement:
                tbl_match = re.search(r'INSERT INTO (\S+)', statement)
                tbl_name = tbl_match.group(1) if tbl_match else "table"
                print(f"Loading {tbl_name}...")
            elif 'CREATE INDEX' in statement:
                idx_match = re.search(r'CREATE INDEX (\S+)', statement)
                idx_name = idx_match.group(1) if idx_match else "index"
                print(f"Creating index {idx_name}...")

            with conn.cursor() as cur:
                cur.execute(statement)

        except Exception as e:
            print(f"Error executing: {statement[:80]}...")
            print(f"Error: {e}")
            conn.close()
            sys.exit(1)

    conn.close()
    print(f"{suite_name} data loading complete.")


def verify_readstream_data(conn_details):
    return _verify_tables(conn_details, ['t_readstream'], "readstream")

def load_readstream_data(conn_details):
    _load_sql(conn_details, READSTREAM_DATA_SQL, "readstream", "10M rows")

def verify_tupdistance_data(conn_details):
    return _verify_tables(conn_details, ['t_tupdistance_new_regress'], "tupdistance")

def load_tupdistance_data(conn_details):
    _load_sql(conn_details, TUPDISTANCE_DATA_SQL, "tupdistance", "10M rows")



def verify_random_backwards_data(conn_details):
    return _verify_tables(conn_details, ['t', 't_randomized'], "random_backwards")


def load_random_backwards_data(conn_details):
    """Load random backwards benchmark data into the database."""
    print("\n" + "=" * 50)
    print("Loading random backwards benchmark data...")
    print("This will take several minutes for 20M rows.")
    print("=" * 50 + "\n")

    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    # Parse SQL into individual statements
    statements = []
    current_stmt = []
    for line in RANDOM_BACKWARDS_DATA_SQL.split('\n'):
        stripped = line.strip()
        if stripped.startswith('--') or not stripped:
            continue
        current_stmt.append(line)
        if stripped.endswith(';'):
            statements.append('\n'.join(current_stmt))
            current_stmt = []

    for statement in statements:
        statement = statement.strip()
        if not statement:
            continue
        try:
            # Print progress for long operations
            if 'INSERT INTO t_randomized' in statement:
                print("Loading t_randomized (10M rows)...")
            elif 'INSERT INTO t' in statement and 'INSERT INTO t_randomized' not in statement:
                print("Loading t (10M rows)...")
            elif 'CREATE INDEX' in statement:
                idx_match = re.search(r'CREATE INDEX (\S+)', statement)
                idx_name = idx_match.group(1) if idx_match else "index"
                print(f"Creating index {idx_name}...")
            elif 'CLUSTER' in statement:
                print("Clustering t_randomized by hash (randomizing physical order)...")

            with conn.cursor() as cur:
                cur.execute(statement)

        except Exception as e:
            print(f"Error executing: {statement[:80]}...")
            print(f"Error: {e}")
            conn.close()
            sys.exit(1)

    conn.close()
    print("Random backwards data loading complete.")


def verify_munro_data(conn_details):
    return _verify_tables(conn_details, ['t_munro'], "munro")


def load_munro_data(conn_details):
    """Load Thomas Munro regression benchmark data into the database."""
    print("\n" + "=" * 50)
    print("Loading Thomas Munro regression benchmark data...")
    print("This will take several minutes for 10M rows.")
    print("=" * 50 + "\n")

    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    # Parse SQL into individual statements
    statements = []
    current_stmt = []
    for line in MUNRO_DATA_SQL.split('\n'):
        stripped = line.strip()
        if stripped.startswith('--') or not stripped:
            continue
        current_stmt.append(line)
        if stripped.endswith(';'):
            statements.append('\n'.join(current_stmt))
            current_stmt = []

    for statement in statements:
        statement = statement.strip()
        if not statement:
            continue
        try:
            # Print progress for long operations
            if 'INSERT INTO t_munro' in statement:
                print("Loading t_munro (10M rows)...")
            elif 'CREATE INDEX' in statement:
                idx_match = re.search(r'CREATE INDEX (\S+)', statement)
                idx_name = idx_match.group(1) if idx_match else "index"
                print(f"Creating index {idx_name}...")

            with conn.cursor() as cur:
                cur.execute(statement)

        except Exception as e:
            print(f"Error executing: {statement[:80]}...")
            print(f"Error: {e}")
            conn.close()
            sys.exit(1)

    conn.close()
    print("Thomas Munro regression data loading complete.")


def verify_worker_regress_data(conn_details):
    return _verify_tables(conn_details, ['worker_regress'], "worker_regress")


def load_worker_regress_data(conn_details):
    """Load worker regression benchmark data into the database."""
    print("\n" + "=" * 50)
    print("Loading worker regression benchmark data...")
    print("This will take several minutes for 5M rows.")
    print("=" * 50 + "\n")

    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    # Parse SQL into individual statements
    statements = []
    current_stmt = []
    for line in WORKER_REGRESS_DATA_SQL.split('\n'):
        stripped = line.strip()
        if stripped.startswith('--') or not stripped:
            continue
        current_stmt.append(line)
        if stripped.endswith(';'):
            statements.append('\n'.join(current_stmt))
            current_stmt = []

    for statement in statements:
        statement = statement.strip()
        if not statement:
            continue
        try:
            # Print progress for long operations
            if 'INSERT INTO worker_regress' in statement:
                print("Loading worker_regress (5M rows)...")
            elif 'CREATE INDEX' in statement:
                idx_match = re.search(r'CREATE INDEX (\S+)', statement)
                idx_name = idx_match.group(1) if idx_match else "index"
                print(f"Creating index {idx_name}...")

            with conn.cursor() as cur:
                cur.execute(statement)

        except Exception as e:
            print(f"Error executing: {statement[:80]}...")
            print(f"Error: {e}")
            conn.close()
            sys.exit(1)

    conn.close()
    print("Worker regression data loading complete.")


def verify_repro_data(conn_details):
    # bookm intentionally dirties ~0.05% of rows after VACUUM to force heap
    # fetches on index-only scans, so not all pages will be all-visible.
    return _verify_tables(conn_details, ['bookm_ios'], "repro",
                          check_all_visible=False)


def load_repro_data(conn_details):
    """Load bookmark index-only-scan regression benchmark data into the database."""
    print("\n" + "=" * 50)
    print("Loading bookmark IOS regression benchmark data...")
    print("This will take several minutes for 10M rows.")
    print("=" * 50 + "\n")

    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    # Parse SQL into individual statements
    statements = []
    current_stmt = []
    for line in REPRO_DATA_SQL.split('\n'):
        stripped = line.strip()
        if stripped.startswith('--') or not stripped:
            continue
        current_stmt.append(line)
        if stripped.endswith(';'):
            statements.append('\n'.join(current_stmt))
            current_stmt = []

    for statement in statements:
        statement = statement.strip()
        if not statement:
            continue
        try:
            # Print progress for long operations
            if 'INSERT INTO bookm_ios' in statement:
                print("Loading bookm_ios (10M rows)...")
            elif 'UPDATE bookm_ios' in statement:
                print("Dirtying ~0.05% of rows for heap fetches...")
            elif 'CREATE INDEX' in statement:
                idx_match = re.search(r'CREATE INDEX (\S+)', statement)
                idx_name = idx_match.group(1) if idx_match else "index"
                print(f"Creating index {idx_name}...")

            with conn.cursor() as cur:
                cur.execute(statement)

        except Exception as e:
            print(f"Error executing: {statement[:80]}...")
            print(f"Error: {e}")
            conn.close()
            sys.exit(1)

    conn.close()
    print("Bookmark IOS regression data loading complete.")


def verify_uuid_data(conn_details):
    return _verify_tables(conn_details, ['t_uuid'], "uuid")

def load_uuid_data(conn_details):
    _load_sql(conn_details, UUID_DATA_SQL, "uuid", "5M rows")


# ── Benchmark suite registry ────────────────────────────────────────────
#
# Single source of truth for all benchmark suites.  run_all_benchmarks.sh and
# patch_report.py discover this list via --list-modes, so adding a new suite
# here automatically makes it available everywhere.
#
# To add a new suite:
#   1. Define its QUERIES dict, DATA_SQL, verify_*_data(), and load_*_data()
#   2. Append one entry to BENCHMARK_SUITES below
#   That's it -- no other files need updating.

BENCHMARK_SUITES = [
    {
        "mode_prefix": "benchmark",
        "cli_flag": None,
        "cli_dest": None,
        "help": None,
        "title": "Prefetch Benchmark",
        "queries": QUERIES,
        "verify_fn": verify_data,
        "load_fn": load_data,
        "tables": EXPECTED_TABLES + ['prefetch_sequential', 'prefetch_sparse'],
        "uncached_runs": 3,
        "cached_runs": 40,
        "sync_stats": True,
    },
    {
        "mode_prefix": "readstream",
        "cli_flag": "--readstream-tests",
        "cli_dest": "readstream_tests",
        "help": "Run readstream benchmark tests (loads data if needed)",
        "title": "Readstream Benchmark Tests",
        "queries": READSTREAM_QUERIES,
        "verify_fn": verify_readstream_data,
        "load_fn": load_readstream_data,
        "tables": ['t_readstream'],
        "uncached_runs": 3,
        "cached_runs": 40,
    },
    {
        "mode_prefix": "tupdistance",
        "cli_flag": "--tupdistance",
        "cli_dest": "tupdistance_tests",
        "help": "Run tupdistance regression benchmark (loads data if needed)",
        "title": "Tupdistance Regression Benchmark",
        "queries": TUPDISTANCE_QUERIES,
        "verify_fn": verify_tupdistance_data,
        "load_fn": load_tupdistance_data,
        "tables": ['t_tupdistance_new_regress'],
        "uncached_runs": 3,
        "cached_runs": 40,
    },
    {
        "mode_prefix": "random_backwards",
        "cli_flag": "--random-backwards-tests",
        "cli_dest": "random_backwards_tests",
        "help": "Run random backwards benchmark tests (loads data if needed)",
        "title": "Random Backwards Benchmark Tests",
        "queries": RANDOM_BACKWARDS_QUERIES,
        "verify_fn": verify_random_backwards_data,
        "load_fn": load_random_backwards_data,
        "tables": ['t', 't_randomized'],
        "uncached_runs": 3,
        "cached_runs": 40,
    },
    {
        "mode_prefix": "munro",
        "cli_flag": "--munro",
        "cli_dest": "munro_tests",
        "help": "Run Thomas Munro mid-January regression benchmark (cold-run regression test)",
        "title": "Thomas Munro mid-January Regression Benchmark",
        "queries": MUNRO_QUERIES,
        "verify_fn": verify_munro_data,
        "load_fn": load_munro_data,
        "tables": ['t_munro'],
        "uncached_runs": 3,
        "cached_runs": 40,
    },
    {
        "mode_prefix": "worker_regress",
        "cli_flag": "--worker-regress-feb",
        "cli_dest": "worker_regress_feb_tests",
        "help": "Run worker regression benchmark (io_method=worker regression test)",
        "title": "Worker Regression Benchmark",
        "queries": WORKER_REGRESS_QUERIES,
        "verify_fn": verify_worker_regress_data,
        "load_fn": load_worker_regress_data,
        "tables": ['worker_regress'],
        "uncached_runs": 3,
        "cached_runs": 40,
    },
    {
        "mode_prefix": "uuid",
        "cli_flag": "--uuid",
        "cli_dest": "uuid_tests",
        "help": "Run UUID benchmark (UUIDv4 PK, correlation ≈ 0, ideal for prefetching)",
        "title": "UUID Benchmark (correlation ≈ 0)",
        "queries": UUID_QUERIES,
        "verify_fn": verify_uuid_data,
        "load_fn": load_uuid_data,
        "tables": ['t_uuid'],
        "uncached_runs": 3,
        "cached_runs": 40,
    },
    {
        "mode_prefix": "repro",
        "cli_flag": "--bookm",
        "cli_dest": "bookm_tests",
        "help": "Run bookmark IOS regression benchmark (index-only scan on DESC index, 10M rows)",
        "title": "Bookmark Index-Only-Scan Regression Benchmark",
        "queries": REPRO_QUERIES,
        "verify_fn": verify_repro_data,
        "load_fn": load_repro_data,
        "tables": ['bookm_ios'],
        "expect_all_visible": False,
        "uncached_runs": 3,
        "cached_runs": 40,
    },
]


# Relations to sync statistics for
STATS_RELATIONS = [
    # Tables
    'prefetch_orders', 'prefetch_customers', 'prefetch_products',
    'prefetch_sequential', 'prefetch_sparse',
    # Indexes
    'prefetch_orders_cust_date_idx', 'prefetch_orders_date_idx',
    'prefetch_orders_prod_idx', 'prefetch_orders_id_idx',
    'prefetch_sequential_idx', 'prefetch_sparse_cat_idx',
    'prefetch_customers_pkey', 'prefetch_products_pkey',
]


def extract_statistics(conn_details):
    """
    Extract optimizer statistics from a PostgreSQL database.
    Returns a dictionary containing relation and attribute stats.
    """
    conn = psycopg.connect(**conn_details)
    stats = {
        'relation_stats': [],
        'attribute_stats': []
    }

    # Extract relation-level stats from pg_class
    with conn.cursor() as cur:
        for relname in STATS_RELATIONS:
            cur.execute("""
                SELECT c.relname, n.nspname,
                       c.relpages, c.reltuples,
                       c.relallvisible, c.relallfrozen
                FROM pg_class c
                JOIN pg_namespace n ON n.oid = c.relnamespace
                WHERE c.relname = %s AND n.nspname = 'public'
            """, (relname,))
            row = cur.fetchone()
            if row:
                stats['relation_stats'].append({
                    'relname': row[0],
                    'schemaname': row[1],
                    'relpages': row[2],
                    'reltuples': row[3],
                    'relallvisible': row[4],
                    'relallfrozen': row[5],
                })

    # Extract attribute-level stats from pg_stats
    with conn.cursor() as cur:
        for relname in STATS_RELATIONS:
            cur.execute("""
                SELECT schemaname, tablename, attname, inherited,
                       null_frac, avg_width, n_distinct,
                       most_common_vals::text, most_common_freqs,
                       histogram_bounds::text, correlation,
                       most_common_elems::text, most_common_elem_freqs,
                       elem_count_histogram
                FROM pg_stats
                WHERE tablename = %s AND schemaname = 'public'
            """, (relname,))
            for row in cur.fetchall():
                stats['attribute_stats'].append({
                    'schemaname': row[0],
                    'tablename': row[1],
                    'attname': row[2],
                    'inherited': row[3],
                    'null_frac': row[4],
                    'avg_width': row[5],
                    'n_distinct': row[6],
                    'most_common_vals': row[7],
                    'most_common_freqs': list(row[8]) if row[8] else None,
                    'histogram_bounds': row[9],
                    'correlation': row[10],
                    'most_common_elems': row[11],
                    'most_common_elem_freqs': list(row[12]) if row[12] else None,
                    'elem_count_histogram': list(row[13]) if row[13] else None,
                })

    conn.close()
    print(f"Extracted stats for {len(stats['relation_stats'])} relations, "
          f"{len(stats['attribute_stats'])} attributes")
    return stats


def _sql_literal(value, type_cast=None):
    """Convert a Python value to a SQL literal string."""
    if value is None:
        return 'NULL' + (f'::{type_cast}' if type_cast else '')
    elif isinstance(value, bool):
        return ('true' if value else 'false') + (f'::{type_cast}' if type_cast else '')
    elif isinstance(value, (int, float)):
        return str(value) + (f'::{type_cast}' if type_cast else '')
    elif isinstance(value, list):
        # Format as PostgreSQL array literal
        elements = ', '.join(str(v) for v in value)
        return f"ARRAY[{elements}]" + (f'::{type_cast}' if type_cast else '')
    else:
        # String - escape single quotes
        escaped = str(value).replace("'", "''")
        return f"'{escaped}'" + (f'::{type_cast}' if type_cast else '')


def restore_statistics(conn_details, stats):
    """
    Restore optimizer statistics to a PostgreSQL database.
    Uses pg_restore_relation_stats and pg_restore_attribute_stats.
    """
    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    restored_rels = 0
    restored_attrs = 0

    # Restore relation-level stats
    with conn.cursor() as cur:
        for rs in stats['relation_stats']:
            try:
                sql = f"""
                    SELECT pg_restore_relation_stats(
                        'schemaname', {_sql_literal(rs['schemaname'])},
                        'relname', {_sql_literal(rs['relname'])},
                        'relpages', {_sql_literal(rs['relpages'], 'integer')},
                        'reltuples', {_sql_literal(rs['reltuples'], 'real')},
                        'relallvisible', {_sql_literal(rs['relallvisible'], 'integer')},
                        'relallfrozen', {_sql_literal(rs['relallfrozen'], 'integer')}
                    )
                """
                cur.execute(sql)
                restored_rels += 1
            except Exception as e:
                print(f"Warning: Failed to restore relation stats for {rs['relname']}: {e}")

    # Restore attribute-level stats
    with conn.cursor() as cur:
        for ats in stats['attribute_stats']:
            try:
                sql = f"""
                    SELECT pg_restore_attribute_stats(
                        'schemaname', {_sql_literal(ats['schemaname'])},
                        'relname', {_sql_literal(ats['tablename'])},
                        'attname', {_sql_literal(ats['attname'])},
                        'inherited', {_sql_literal(ats['inherited'], 'boolean')},
                        'null_frac', {_sql_literal(ats['null_frac'], 'real')},
                        'avg_width', {_sql_literal(ats['avg_width'], 'integer')},
                        'n_distinct', {_sql_literal(ats['n_distinct'], 'real')},
                        'most_common_vals', {_sql_literal(ats['most_common_vals'], 'text')},
                        'most_common_freqs', {_sql_literal(ats['most_common_freqs'], 'real[]')},
                        'histogram_bounds', {_sql_literal(ats['histogram_bounds'], 'text')},
                        'correlation', {_sql_literal(ats['correlation'], 'real')},
                        'most_common_elems', {_sql_literal(ats['most_common_elems'], 'text')},
                        'most_common_elem_freqs', {_sql_literal(ats['most_common_elem_freqs'], 'real[]')},
                        'elem_count_histogram', {_sql_literal(ats['elem_count_histogram'], 'real[]')}
                    )
                """
                cur.execute(sql)
                restored_attrs += 1
            except Exception as e:
                print(f"Warning: Failed to restore attribute stats for "
                      f"{ats['tablename']}.{ats['attname']}: {e}")

    conn.close()
    print(f"Restored stats for {restored_rels} relations, {restored_attrs} attributes")


# clear_os_cache, evict_relations, prewarm_relations, set_gucs, reset_gucs
# are imported from benchmark_common


def extract_execution_time(explain_output):
    """Extract execution time from EXPLAIN ANALYZE output."""
    for line in explain_output:
        match = re.search(r'Execution Time: ([\d.]+) ms', line[0])
        if match:
            return float(match.group(1))
    return None


def pin_backend(pid, cpu, enabled=False):
    """Pin a backend process to a specific CPU."""
    if not enabled:
        return
    try:
        result = subprocess.run(
            ["taskset", "-cp", str(cpu), str(pid)],
            capture_output=True,
            text=True,
            check=False
        )
        if result.returncode == 0:
            print(f"Pinned backend PID {pid} to CPU {cpu}")

        # Try RT scheduling
        result = subprocess.run(
            ["sudo", "chrt", "-f", "-p", "1", str(pid)],
            capture_output=True,
            text=True,
            check=False
        )
        if result.returncode == 0:
            print(f"Set backend PID {pid} to SCHED_FIFO")
    except Exception as e:
        print(f"Warning: Could not pin backend: {e}")


PREPARED_STMT_NAME = "_bench_stmt"


def prepare_query(conn, sql, gucs=None, is_master=False, prefetch_setting=None):
    """Prepare a query so that subsequent runs avoid planning overhead.

    Planning-relevant GUCs must be active before PREPARE, since a
    parameter-less prepared statement is planned once at PREPARE time.
    We also force generic plan mode so EXECUTE reuses the cached plan
    without per-execution plan cache overhead.
    """
    set_gucs(conn, gucs or {}, is_master=is_master, prefetch_setting=prefetch_setting)
    with conn.cursor() as cur:
        cur.execute("SET plan_cache_mode = force_generic_plan")
        cur.execute(f"PREPARE {PREPARED_STMT_NAME} AS {sql.strip()}")


def deallocate_query(conn):
    """Deallocate the prepared benchmark statement."""
    with conn.cursor() as cur:
        cur.execute(f"DEALLOCATE {PREPARED_STMT_NAME}")


def run_query(conn, query_def, cached_mode, is_master, prefetch_setting, benchmark_cpu, serialize=True, direct_io=False, skip_prewarm=False):
    """
    Run a single query with proper cache preparation.
    The query must already be prepared via prepare_query().
    Returns (execution_time_ms, explain_output_str) tuple.

    skip_prewarm: If True, skip cache preparation entirely.  Useful in
    cached mode where prewarming only needs to happen once before the
    first run of a query, not on every subsequent run.
    """
    # Cache preparation
    if skip_prewarm:
        pass
    elif cached_mode:
        # Prewarm everything
        prewarm_relations(conn, query_def.get("prewarm_indexes", []))
        prewarm_relations(conn, query_def.get("prewarm_tables", []), include_vm=True)
    else:
        # Uncached: evict heap, prewarm indexes + heap VM, clear OS cache
        evict_relations(conn, query_def.get("evict", []))
        prewarm_relations(conn, query_def.get("prewarm_indexes", []))
        prewarm_relations(conn, query_def.get("prewarm_tables", []), vm_only=True)
        if not direct_io:
            clear_os_cache()

    # Set GUCs
    gucs = query_def.get("gucs", {})
    set_gucs(conn, gucs, is_master=is_master, prefetch_setting=prefetch_setting)

    # Special handling for warmup queries (A3)
    if query_def.get("warmup_query") and not cached_mode:
        with conn.cursor() as cur:
            cur.execute(f"EXECUTE {PREPARED_STMT_NAME}")
            cur.execute(f"EXECUTE {PREPARED_STMT_NAME}")

    # Execute with EXPLAIN ANALYZE using force_custom_plan to avoid
    # plan cache artifacts while still skipping full re-planning.
    serialize_opt = ", SERIALIZE" if serialize else ""
    explain_sql = f"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF{serialize_opt}) EXECUTE {PREPARED_STMT_NAME}"

    with conn.cursor() as cur:
        cur.execute(explain_sql)
        result = cur.fetchall()

    # Reset query-specific GUCs
    if gucs:
        reset_gucs(conn, gucs)

    # Format EXPLAIN output as string
    explain_output = "\n".join(row[0] for row in result)

    # Extract execution time
    exec_time = extract_execution_time(result)
    if exec_time is None:
        print("Warning: Could not extract execution time from EXPLAIN output")
        return None, explain_output

    return exec_time, explain_output


def load_most_recent_master_results(mode_name, needed_queries=None):
    """Load master results from the most recent benchmark JSON file for a given mode.

    Searches result files from newest to oldest.  When *needed_queries* is
    given, skips files that don't contain master results for at least one of
    those queries; this avoids picking up a tiny single-query run when a
    larger earlier run has the data we need.

    Args:
        mode_name: The benchmark mode to match (e.g. "benchmark_cached",
                   "readstream_uncached").  Files are matched by their
                   "{mode_name}_" prefix.
        needed_queries: Optional list of query IDs that the caller wants
                        master results for.  If provided, the first file
                        (newest) that contains at least one of them is used.

    Returns:
        tuple: (old_results dict, json_file path) or (None, None) if not found
    """
    if not os.path.exists(OUTPUT_DIR):
        return None, None

    # Find JSON files matching this mode's naming convention
    prefix = f"{mode_name}_"
    json_files = []
    for f in os.listdir(OUTPUT_DIR):
        if f.startswith(prefix) and f.endswith(".json"):
            json_files.append(os.path.join(OUTPUT_DIR, f))

    if not json_files:
        return None, None

    # Sort by modification time (most recent first)
    json_files.sort(key=os.path.getmtime, reverse=True)

    for json_file in json_files[:500]:
        try:
            with open(json_file, "r") as f:
                old_results = json.load(f)
        except (json.JSONDecodeError, IOError) as e:
            print(f"Warning: Could not load {json_file}: {e}")
            continue

        # If caller needs specific queries, check that this file has at least one
        if needed_queries:
            old_queries = old_results.get("queries", {})
            has_any = any(
                qid in old_queries and old_queries[qid].get("master", {}).get("min") is not None
                for qid in needed_queries
            )
            if not has_any:
                continue

        return old_results, json_file

    return None, None



def run_generic_benchmark(args, queries_dict, mode_name, title,
                          verify_func=None, load_func=None,
                          sync_stats=False, tables=None,
                          expect_all_visible=True):
    """Run a benchmark suite.

    This is the unified benchmark runner used by all benchmark modes
    (see BENCHMARK_SUITES registry). It handles tmpfs setup,
    data verification/loading, master runs, patch runs, results summary,
    file saving, and rankings.

    Args:
        args: Parsed command-line arguments.
        queries_dict: OrderedDict of query definitions (e.g. QUERIES, READSTREAM_QUERIES).
        mode_name: Short mode identifier for result files (e.g. "benchmark", "readstream").
        title: Display title for the benchmark (e.g. "Prefetch Benchmark").
        verify_func: Function(conn_details) -> bool to verify data exists.
                     For the default benchmark with sync_stats=True, this takes
                     (conn_details, skip_load) instead.
        load_func: Function(conn_details) to load data.
        sync_stats: If True, extract optimizer stats from master and restore to patch
                    (only needed for the default benchmark where tables are shared).
        tables: List of table names belonging to this suite (used to verify
                all-visible status after loading).
        expect_all_visible: If True (default), verify all pages are all-visible
                            after loading and VACUUM FREEZE if not.
    """
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    # Resolve baseline and testbranch configuration
    baseline_name = getattr(args, "baseline", "master")
    baseline_label = baseline_name.upper()
    baseline_bin_orig, baseline_data_dir, baseline_source_dir, baseline_conn = BASELINE_CONFIGS[baseline_name]
    testbranch_name = getattr(args, "testbranch", "patch")
    test_bin_orig, test_data_dir, test_source_dir, test_conn = TESTBRANCH_CONFIGS[testbranch_name]
    test_has_prefetch = BUILD_HAS_PREFETCH[testbranch_name]

    # Setup tmpfs with hugepages if requested
    tmpfs_mount = None
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

    # Parse query selection
    if args.queries:
        selected_queries = [q.strip().upper() for q in args.queries.split(",")]
        for q in selected_queries:
            if q not in queries_dict:
                print(f"Error: Unknown query '{q}'. Available: {', '.join(queries_dict.keys())}")
                sys.exit(1)
    else:
        selected_queries = list(queries_dict.keys())

    # Track queries excluded by --exclude-ms (populated after master runs)
    excluded_query_ids = set()

    # Handle --old-master-results: load previous results and filter queries
    old_master_data = None
    old_master_file = None
    old_results = None
    if args.old_master_results:
        old_results, old_master_file = load_most_recent_master_results(mode_name, selected_queries)
        if old_results is None:
            print("No previous master results found, will run master fresh")
            args.old_master_results = False
        else:
            # Extract master data from old results
            old_master_data = {}
            old_queries = old_results.get("queries", {})
            for qid in list(selected_queries):
                if qid in old_queries and old_queries[qid].get("master", {}).get("min") is not None:
                    old_master_data[qid] = old_queries[qid]["master"]
                else:
                    print(f"Warning: Query {qid} has no master results in previous run, skipping")
                    selected_queries.remove(qid)

            if not selected_queries:
                print("No queries with existing master results, will run master fresh")
                args.old_master_results = False
                old_results = None
                old_master_data = None
                old_master_file = None
                # Restore the original query selection (don't expand to all queries)
                if args.queries:
                    selected_queries = [q.strip().upper() for q in args.queries.split(",")]
                else:
                    selected_queries = list(queries_dict.keys())
            else:
                print(f"\nUsing master results from: {old_master_file}")
                print(f"  Original master hash: {old_results.get('master_hash', 'unknown')}")
                print(f"  Queries with results: {', '.join(selected_queries)}")

    print(f"\n{'=' * 60}")
    print(title)
    print(f"{'=' * 60}")
    mode_word = ("\033[32m💵  cached\033[0m" if args.cached
                 else "\033[34m💾  uncached\033[0m")
    print(f"Mode: {mode_word}")
    print(f"Queries: {', '.join(selected_queries)}")
    print(f"Runs per query: {args.runs}")
    print(f"{'=' * 60}\n")

    # Get git hashes
    master_hash = get_git_hash(baseline_source_dir)
    patch_hash = get_git_hash(test_source_dir)
    if not args.terse:
        print(f"Baseline ({baseline_name}) git hash: {master_hash}")
        print(f"Testbranch ({testbranch_name}) git hash: {patch_hash}")
        print_io_settings(args)

    # Verify/load data on each server (one at a time due to memory constraints)
    master_stats = None
    if args.skip_load:
        master_version = None
        patch_version = None
    elif verify_func and load_func:
        # sync_stats mode (default benchmark): verify_data takes (conn, skip_load)
        # Other modes: verify functions take only conn_details

        if args.old_master_results and sync_stats and old_results:
            # Load saved statistics from old results instead of starting master
            master_stats = old_results.get("master_stats")
            if master_stats:
                print(f"\nUsing saved master statistics from previous results")
            else:
                print("Warning: Old results have no saved master_stats, starting master to extract them")

        if not (args.old_master_results and master_stats):
            # Need to start master to verify data and/or extract statistics
            print(f"\n--- Verifying data on baseline ({baseline_name}) ---")
            start_server(master_bin, baseline_name, baseline_data_dir, baseline_conn, args)
            if sync_stats:
                if not verify_func(baseline_conn, args.skip_load):
                    print(f"Loading data on {baseline_name}...")
                    load_func(baseline_conn)
                    if expect_all_visible and tables:
                        _ensure_all_visible_after_load(baseline_conn, tables)
                master_stats = extract_statistics(baseline_conn)
            else:
                if not verify_func(baseline_conn):
                    print(f"Loading data on {baseline_name}...")
                    load_func(baseline_conn)
                    if expect_all_visible and tables:
                        _ensure_all_visible_after_load(baseline_conn, tables)
            master_version = get_pg_version(baseline_conn)
            stop_server(master_bin, baseline_data_dir)
            time.sleep(2)
        else:
            master_version = old_results.get("master_version")

        print(f"\n--- Verifying data on testbranch ({testbranch_name}) ---")
        start_server(patch_bin, testbranch_name, test_data_dir, test_conn, args)
        if sync_stats:
            if not verify_func(test_conn, args.skip_load):
                print(f"Loading data on {testbranch_name}...")
                load_func(test_conn)
                if expect_all_visible and tables:
                    _ensure_all_visible_after_load(test_conn, tables)
            restore_statistics(test_conn, master_stats)
        else:
            if not verify_func(test_conn):
                print(f"Loading data on {testbranch_name}...")
                load_func(test_conn)
                if expect_all_visible and tables:
                    _ensure_all_visible_after_load(test_conn, tables)
        patch_version = get_pg_version(test_conn)
        stop_server(patch_bin, test_data_dir)
        time.sleep(2)
    else:
        master_version = None
        patch_version = None

    # Results storage
    # Use old master hash if using old master results
    effective_master_hash = master_hash
    if args.old_master_results and old_results:
        effective_master_hash = old_results.get("master_hash", master_hash)
    results = {
        "timestamp": datetime.now().isoformat(),
        "baseline": baseline_name,
        "testbranch": testbranch_name,
        "master_hash": effective_master_hash,
        "patch_hash": patch_hash,
        "master_version": master_version,
        "patch_version": patch_version,
        "mode": mode_name,
        "runs": args.runs,
        "io_settings": {
            "effective_io_concurrency": args.effective_io_concurrency,
            "io_combine_limit": args.io_combine_limit,
            "io_max_combine_limit": args.io_max_combine_limit,
            "io_max_concurrency": args.io_max_concurrency,
            "io_workers": args.io_workers,
            "io_method": args.io_method,
            "direct_io": getattr(args, "direct_io", False),
        },
        "queries": {},
        "master_results_from": old_master_file if args.old_master_results else None,
        "master_stats": master_stats,
    }

    # Initialize query results structure
    for query_id in selected_queries:
        query_def = queries_dict[query_id]
        # Use old master data if available, otherwise initialize empty
        if old_master_data and query_id in old_master_data:
            master_result = old_master_data[query_id].copy()
        else:
            master_result = {"times": [], "explains": [], "avg": None, "min": None, "max": None, "explain": None}
        results["queries"][query_id] = {
            "name": query_def["name"],
            "master": master_result,
            "patch_off": {"times": [], "explains": [], "avg": None, "min": None, "max": None, "explain": None},
            "patch_on": {"times": [], "explains": [], "avg": None, "min": None, "max": None, "explain": None},
        }

    # Run all queries on master (skip if using old master results)
    if args.old_master_results:
        print(f"\n{'=' * 60}")
        print(f"SKIPPING {baseline_label} (using previous results)")
        print(f"{'=' * 60}")
        master_start_time = 0
        master_end_time = 0
        # Get master version from old results if available
        if old_results and old_results.get("master_version"):
            master_version = old_results["master_version"]

        # Identify queries below --exclude-ms threshold (when using old master results)
        if args.exclude_ms is not None:
            stat_label = get_stat_label()
            for query_id in selected_queries:
                master_times = results["queries"][query_id]["master"].get("times", [])
                master_rep = get_representative(master_times)
                if master_rep is not None and master_rep < args.exclude_ms:
                    excluded_query_ids.add(query_id)

            if excluded_query_ids:
                print(f"\nExcluding {len(excluded_query_ids)} queries below {args.exclude_ms:.1f} ms threshold ({stat_label}) from patch runs:")
                for qid in sorted(excluded_query_ids):
                    ms = get_representative(results["queries"][qid]["master"].get("times", []))
                    print(f"  {qid}: {ms:.3f} ms")
    else:
        print(f"\n{'=' * 60}")
        print(f"Running all queries on BASELINE ({baseline_name})")
        print(f"{'=' * 60}")
        master_start_time = time.time()
        start_server(master_bin, baseline_name, baseline_data_dir, baseline_conn, args)
        try:
            master_conn = psycopg.connect(**baseline_conn)
            pin_backend(master_conn.info.backend_pid, args.benchmark_cpu, enabled=args.pin)
            if master_version is None:
                master_version = get_pg_version(baseline_conn)
            with master_conn.cursor() as cur:
                cur.execute("CREATE EXTENSION IF NOT EXISTS pg_prewarm")
                cur.execute("CREATE EXTENSION IF NOT EXISTS pg_buffercache")

            for query_id in selected_queries:
                query_def = queries_dict[query_id]
                print(f"\n{query_id}: {query_def['name']} ({args.runs} runs)...")
                prepare_query(master_conn, query_def["sql"],
                              gucs=query_def.get("gucs", {}), is_master=True)
                for run in range(args.runs):
                    exec_time, explain_output = run_query(
                        master_conn, query_def, args.cached,
                        is_master=True, prefetch_setting=None,
                        benchmark_cpu=args.benchmark_cpu,
                        serialize=not args.no_serialize,
                        direct_io=args.direct_io,
                        skip_prewarm=(args.cached and run > 0)
                    )
                    if exec_time is not None:
                        results["queries"][query_id]["master"]["times"].append(exec_time)
                        results["queries"][query_id]["master"]["explains"].append(explain_output)
                        if not args.terse:
                            print(f"  Run {run + 1}: {exec_time:.3f} ms")
                deallocate_query(master_conn)

            master_conn.close()
        finally:
            stop_server(master_bin, baseline_data_dir)
            time.sleep(2)
        master_end_time = time.time()

        # Identify queries below --exclude-ms threshold
        if args.exclude_ms is not None:
            stat_label = get_stat_label()
            for query_id in selected_queries:
                master_times = results["queries"][query_id]["master"]["times"]
                if master_times:
                    master_rep = get_representative(master_times)
                    if master_rep < args.exclude_ms:
                        excluded_query_ids.add(query_id)

            if excluded_query_ids and not args.terse:
                print(f"\nExcluding {len(excluded_query_ids)} queries below {args.exclude_ms:.1f} ms threshold ({stat_label}) from patch runs:")
                for qid in sorted(excluded_query_ids):
                    ms = get_representative(results["queries"][qid]["master"]["times"])
                    print(f"  {qid}: {ms:.3f} ms")

    # Run all queries on testbranch (both prefetch=off and prefetch=on, or plain if no prefetch GUC)
    print(f"\n{'=' * 60}")
    print(f"Running all queries on TESTBRANCH ({testbranch_name})")
    print(f"{'=' * 60}")
    patch_start_time = time.time()
    start_server(patch_bin, testbranch_name, test_data_dir, test_conn, args)
    try:
        patch_conn = psycopg.connect(**test_conn)
        pin_backend(patch_conn.info.backend_pid, args.benchmark_cpu, enabled=args.pin)
        if patch_version is None:
            patch_version = get_pg_version(test_conn)
        with patch_conn.cursor() as cur:
            cur.execute("CREATE EXTENSION IF NOT EXISTS pg_prewarm")
            cur.execute("CREATE EXTENSION IF NOT EXISTS pg_buffercache")

        for query_id in selected_queries:
            # Skip queries below --exclude-ms threshold
            if query_id in excluded_query_ids:
                continue

            query_def = queries_dict[query_id]
            # When testbranch lacks the prefetch GUC, treat it like a baseline
            test_is_master = not test_has_prefetch
            prepare_query(patch_conn, query_def["sql"],
                          gucs=query_def.get("gucs", {}), is_master=test_is_master)

            # Run with prefetch OFF (skip if --prefetch-only or testbranch has no prefetch GUC)
            if not args.prefetch_only and test_has_prefetch:
                print(f"\n{query_id}: {query_def['name']} (prefetch=off, {args.runs} runs)...")
                for run in range(args.runs):
                    exec_time, explain_output = run_query(
                        patch_conn, query_def, args.cached,
                        is_master=False, prefetch_setting="off",
                        benchmark_cpu=args.benchmark_cpu,
                        serialize=not args.no_serialize,
                        direct_io=args.direct_io,
                        skip_prewarm=(args.cached and run > 0)
                    )
                    if exec_time is not None:
                        results["queries"][query_id]["patch_off"]["times"].append(exec_time)
                        results["queries"][query_id]["patch_off"]["explains"].append(explain_output)
                        if not args.terse:
                            print(f"  Run {run + 1}: {exec_time:.3f} ms")

            # Run with prefetch ON (skip if --prefetch-disabled)
            # When testbranch has no prefetch GUC, run queries plain (results stored in patch_on)
            if not args.prefetch_disabled:
                if test_has_prefetch:
                    print(f"\n{query_id}: {query_def['name']} (prefetch=on, {args.runs} runs)...")
                    pf_setting = "on"
                else:
                    print(f"\n{query_id}: {query_def['name']} ({args.runs} runs)...")
                    pf_setting = None
                for run in range(args.runs):
                    exec_time, explain_output = run_query(
                        patch_conn, query_def, args.cached,
                        is_master=test_is_master, prefetch_setting=pf_setting,
                        benchmark_cpu=args.benchmark_cpu,
                        serialize=not args.no_serialize,
                        direct_io=args.direct_io,
                        skip_prewarm=(args.cached and run > 0)
                    )
                    if exec_time is not None:
                        results["queries"][query_id]["patch_on"]["times"].append(exec_time)
                        results["queries"][query_id]["patch_on"]["explains"].append(explain_output)
                        if not args.terse:
                            print(f"  Run {run + 1}: {exec_time:.3f} ms")

            deallocate_query(patch_conn)

        patch_conn.close()
    finally:
        stop_server(patch_bin, test_data_dir)
        time.sleep(2)
    patch_end_time = time.time()

    # Cleanup tmpfs if it was created
    if tmpfs_mount:
        cleanup_tmpfs(tmpfs_mount)

    # Calculate statistics
    for query_id in selected_queries:
        query_results = results["queries"][query_id]
        for config in ["master", "patch_off", "patch_on"]:
            times = query_results[config]["times"]
            if times:
                query_results[config]["avg"] = mean(times)
                query_results[config]["min"] = min(times)
                query_results[config]["median"] = median(times)
                query_results[config]["max"] = max(times)
            # Select the explain output from the run closest to the representative value
            explains = query_results[config].get("explains", [])
            if times and explains:
                rep = get_representative(times)
                best_idx = min(range(len(times)), key=lambda i: abs(times[i] - rep))
                query_results[config]["explain"] = explains[best_idx]
            # Drop the explains list before saving to JSON (it's bulky and redundant)
            query_results[config].pop("explains", None)

    # Print per-query detail (suppressed in terse mode)
    if not args.terse:
        print(f"\n{'=' * 60}")
        print("RESULTS SUMMARY")
        print(f"{'=' * 60}")

        for query_id in selected_queries:
            query_results = results["queries"][query_id]

            stat_label = get_stat_label()
            master_rep = get_representative(query_results["master"]["times"])
            patch_off_rep = get_representative(query_results["patch_off"]["times"])
            patch_on_rep = get_representative(query_results["patch_on"]["times"])

            # ANSI bold escape codes
            BOLD = "\033[1m"
            RESET = "\033[0m"

            print(f"\n{BOLD}{query_id}: {query_results['name']}{RESET}")
            if master_rep:
                print(f"  {baseline_name} ({stat_label}):               {master_rep:10.3f} ms "
                      f"(avg={query_results['master']['avg']:.3f}, max={query_results['master']['max']:.3f})")
            if patch_off_rep and master_rep:
                ratio_off = patch_off_rep / master_rep
                print(f"  patch (prefetch=off) ({stat_label}): {patch_off_rep:10.3f} ms "
                      f"(avg={query_results['patch_off']['avg']:.3f}, max={query_results['patch_off']['max']:.3f}) "
                      f"[{BOLD}{ratio_off:.3f}x{RESET} vs {baseline_name}]")
            if patch_on_rep and master_rep:
                ratio_on = patch_on_rep / master_rep
                print(f"  patch (prefetch=on) ({stat_label}):  {patch_on_rep:10.3f} ms "
                      f"(avg={query_results['patch_on']['avg']:.3f}, max={query_results['patch_on']['max']:.3f}) "
                      f"[{BOLD}{ratio_on:.3f}x{RESET} vs {baseline_name}]")

            # Print query text and EXPLAIN ANALYZE outputs
            # Show prefetch=off only if --prefetch-disabled, otherwise show prefetch=on
            print()
            query_sql = queries_dict[query_id]["sql"].strip()
            print("  Query:")
            for line in query_sql.split('\n'):
                print(f"    {line.strip()}")
            print()
            if query_results["master"]["explain"]:
                print(f"  {baseline_name} EXPLAIN ANALYZE:")
                for line in query_results["master"]["explain"].split('\n'):
                    print(f"    {line}")
            if args.prefetch_disabled:
                if query_results["patch_off"]["explain"]:
                    print()
                    print("  patch (prefetch=off) EXPLAIN ANALYZE:")
                    for line in query_results["patch_off"]["explain"].split('\n'):
                        print(f"    {line}")
            else:
                if query_results["patch_on"]["explain"]:
                    print()
                    print("  patch (prefetch=on) EXPLAIN ANALYZE:")
                    for line in query_results["patch_on"]["explain"].split('\n'):
                        print(f"    {line}")

    # Save results
    # Update versions in case they were fetched during benchmark run (--skip-load)
    results["master_version"] = master_version
    results["patch_version"] = patch_version

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    json_file = os.path.join(OUTPUT_DIR, f"{mode_name}_{timestamp}.json")
    txt_file = os.path.join(OUTPUT_DIR, f"{mode_name}_{timestamp}.txt")

    # Save JSON
    with open(json_file, "w") as f:
        json.dump(results, f, indent=2)
    if not args.terse:
        print(f"\nResults saved to: {json_file}")

    # Save human-readable text
    with open(txt_file, "w") as f:
        f.write("=" * 70 + "\n")
        f.write(f"{title} Results\n")
        f.write("=" * 70 + "\n\n")
        f.write(f"Timestamp: {results['timestamp']}\n")
        f.write(f"{baseline_label} git hash: {results['master_hash']}\n")
        f.write(f"Patch git hash: {results['patch_hash']}\n")
        f.write(f"Mode: {results['mode']}\n")
        f.write(f"Runs per query: {results['runs']}\n\n")

        stat_label = get_stat_label()
        for query_id, qr in results["queries"].items():
            f.write(f"\n{query_id}: {qr['name']}\n")
            f.write("-" * 50 + "\n")

            master_rep = get_representative(qr["master"]["times"])
            patch_off_rep = get_representative(qr["patch_off"]["times"])
            patch_on_rep = get_representative(qr["patch_on"]["times"])

            if master_rep:
                f.write(f"  {baseline_name} ({stat_label}):               {master_rep:10.3f} ms "
                        f"(avg={qr['master']['avg']:.3f}, max={qr['master']['max']:.3f})\n")
            if patch_off_rep and master_rep:
                ratio_off = patch_off_rep / master_rep
                f.write(f"  patch (prefetch=off) ({stat_label}): {patch_off_rep:10.3f} ms "
                        f"(avg={qr['patch_off']['avg']:.3f}, max={qr['patch_off']['max']:.3f}) "
                        f"[{ratio_off:.3f}x vs {baseline_name}]\n")
            if patch_on_rep and master_rep:
                ratio_on = patch_on_rep / master_rep
                f.write(f"  patch (prefetch=on) ({stat_label}):  {patch_on_rep:10.3f} ms "
                        f"(avg={qr['patch_on']['avg']:.3f}, max={qr['patch_on']['max']:.3f}) "
                        f"[{ratio_on:.3f}x vs {baseline_name}]\n")

            # Write query text and EXPLAIN ANALYZE outputs
            # Show prefetch=off only if --prefetch-disabled, otherwise show prefetch=on
            f.write("\n")
            query_sql = queries_dict[query_id]["sql"].strip()
            f.write("  Query:\n")
            for line in query_sql.split('\n'):
                f.write(f"    {line.strip()}\n")
            f.write("\n")
            if qr["master"]["explain"]:
                f.write(f"  {baseline_name} EXPLAIN ANALYZE:\n")
                for line in qr["master"]["explain"].split('\n'):
                    f.write(f"    {line}\n")
            if args.prefetch_disabled:
                if qr["patch_off"]["explain"]:
                    f.write("\n")
                    f.write("  patch (prefetch=off) EXPLAIN ANALYZE:\n")
                    for line in qr["patch_off"]["explain"].split('\n'):
                        f.write(f"    {line}\n")
            else:
                if qr["patch_on"]["explain"]:
                    f.write("\n")
                    f.write("  patch (prefetch=on) EXPLAIN ANALYZE:\n")
                    for line in qr["patch_on"]["explain"].split('\n'):
                        f.write(f"    {line}\n")

    if not args.terse:
        print(f"Results saved to: {txt_file}")

    # Update latest symlink
    latest_link = os.path.join(OUTPUT_DIR, "latest.txt")
    if os.path.exists(latest_link):
        os.remove(latest_link)
    os.symlink(os.path.basename(txt_file), latest_link)

    # Print total run times
    master_duration = master_end_time - master_start_time
    patch_duration = patch_end_time - patch_start_time
    total_duration = master_duration + patch_duration

    if not args.terse:
        print(f"\n{'=' * 60}")
        print("BENCHMARK RUN TIMES (excluding data loading)")
        print(f"{'=' * 60}")
        print(f"  {baseline_label + ':':11s}{master_duration:10.1f} seconds ({_format_duration(master_duration)})")
        print(f"  Patch:   {patch_duration:10.1f} seconds ({_format_duration(patch_duration)})")
        print(f"  Total:   {total_duration:10.1f} seconds ({_format_duration(total_duration)})")

    # Collect all patch runs with their ratios vs master (using min or median as representative)
    # Each patch configuration (prefetch=off, prefetch=on) is treated independently
    stat_label = get_stat_label()
    all_ratios = []
    for query_id in selected_queries:
        # Skip queries below --exclude-ms threshold
        if query_id in excluded_query_ids:
            continue

        qr = results["queries"][query_id]
        master_rep = get_representative(qr["master"]["times"])
        if not master_rep:
            continue

        # patch (prefetch=off)
        patch_off_rep = get_representative(qr["patch_off"]["times"])
        if patch_off_rep:
            ratio = patch_off_rep / master_rep
            all_ratios.append({
                "query_id": query_id,
                "name": qr["name"],
                "config": "prefetch=off",
                "ratio": ratio,
                "master_ms": master_rep,
                "patch_ms": patch_off_rep,
            })

        # patch (prefetch=on)
        patch_on_rep = get_representative(qr["patch_on"]["times"])
        if patch_on_rep:
            ratio = patch_on_rep / master_rep
            all_ratios.append({
                "query_id": query_id,
                "name": qr["name"],
                "config": "prefetch=on",
                "ratio": ratio,
                "master_ms": master_rep,
                "patch_ms": patch_on_rep,
            })

    # Neutral: 0.99 <= ratio <= 1.01 (essentially same speed)
    # Improvements: ratio < 0.99 (faster than master)
    # Regressions: ratio > 1.01 (slower than master)
    neutral = sorted([e for e in all_ratios if 0.99 <= e["ratio"] <= 1.01], key=lambda x: x["ratio"])
    improvements = sorted([e for e in all_ratios if e["ratio"] < 0.99], key=lambda x: x["ratio"])
    regressions = sorted([e for e in all_ratios if e["ratio"] > 1.01], key=lambda x: x["ratio"], reverse=True)

    BOLD = "\033[1m"
    RESET = "\033[0m"

    topn = args.topn

    # Print neutral section first
    print(f"\n{'=' * 60}")
    print(f"NEUTRAL vs {baseline_label} (using {stat_label}) [{len(neutral)} total]")
    print(f"{'=' * 60}")
    if neutral:
        for rank, entry in enumerate(neutral[:topn], 1):
            print(f"  #{rank}  {entry['query_id']} ({entry['config']}): {entry['name']}")
            print(f"       {BOLD}{entry['ratio']:.3f}x{RESET} - {baseline_name} ({stat_label}): {entry['master_ms']:.3f} ms, patch ({stat_label}): {entry['patch_ms']:.3f} ms")
    else:
        print("  (none)")

    # Print improvements section
    print(f"\n{'=' * 60}")
    print(f"TOP {topn} IMPROVEMENTS vs {baseline_label} (using {stat_label}) [{len(improvements)} total]")
    print(f"{'=' * 60}")
    if improvements:
        for rank, entry in enumerate(improvements[:topn], 1):
            print(f"  #{rank}  {entry['query_id']} ({entry['config']}): {entry['name']}")
            print(f"       {BOLD}{entry['ratio']:.3f}x{RESET} - {baseline_name} ({stat_label}): {entry['master_ms']:.3f} ms, patch ({stat_label}): {entry['patch_ms']:.3f} ms")
    else:
        print("  (none)")

    # Print regressions section last
    print(f"\n{'=' * 60}")
    print(f"TOP {topn} REGRESSIONS vs {baseline_label} (using {stat_label}) [{len(regressions)} total]")
    print(f"{'=' * 60}")
    if regressions:
        for rank, entry in enumerate(regressions[:topn], 1):
            print(f"  #{rank}  {entry['query_id']} ({entry['config']}): {entry['name']}")
            print(f"       {BOLD}{entry['ratio']:.3f}x{RESET} - {baseline_name} ({stat_label}): {entry['master_ms']:.3f} ms, patch ({stat_label}): {entry['patch_ms']:.3f} ms")
    else:
        print("  (none)")

    # Print overall summary comparing patch vs master
    # Determine which config to summarize based on benchmark settings
    if args.prefetch_disabled:
        summary_config = "prefetch=off"
        summary_label = f"patch + no prefetch vs {baseline_name}"
    else:
        summary_config = "prefetch=on"
        summary_label = f"patch + prefetch vs {baseline_name}"

    # Filter ratios for the applicable config
    config_ratios = [e for e in all_ratios if e["config"] == summary_config]

    if config_ratios:
        # Calculate total execution times
        total_master_ms = sum(e["master_ms"] for e in config_ratios)
        total_patch_ms = sum(e["patch_ms"] for e in config_ratios)

        # Calculate geometric mean of ratios
        log_sum = sum(math.log(e["ratio"]) for e in config_ratios)
        geomean_ratio = math.exp(log_sum / len(config_ratios))

        print(f"\n{'=' * 60}")
        print(f"OVERALL SUMMARY: {summary_label}")
        print(f"{'=' * 60}")
        total_ratio = total_patch_ms / total_master_ms
        print(f"  Total execution time ({baseline_name}): {total_master_ms:10.3f} ms")
        print(f"  Total execution time (patch):  {total_patch_ms:10.3f} ms")
        print(f"  Ratio of exec time totals:     {BOLD}{total_ratio:.3f}x{RESET}")
        print(f"  Geometric mean of ratios:      {BOLD}{geomean_ratio:.3f}x{RESET}")
        print(f"  Number of queries:             {len(config_ratios)}")

        # Calculate improved/regressed/neutral counts using same thresholds
        config_improved = [e for e in config_ratios if e["ratio"] < 0.99]
        config_regressed = [e for e in config_ratios if e["ratio"] > 1.01]
        config_neutral = [e for e in config_ratios if 0.99 <= e["ratio"] <= 1.01]
        best_ratio = min(e["ratio"] for e in config_ratios)
        worst_ratio = max(e["ratio"] for e in config_ratios)
        print(f"  Queries improved:              {len(config_improved)}")
        print(f"  Queries regressed:             {len(config_regressed)}")
        print(f"  Queries neutral:               {len(config_neutral)}")
        print(f"  Best case ratio:               {best_ratio:.3f}x")
        print(f"  Worst case ratio:              {worst_ratio:.3f}x")


def run_stress_test(args):
    """Run stress test mode: randomly generate queries to find regressions."""
    # Setup tmpfs with hugepages if requested
    tmpfs_mount = None
    baseline_name = getattr(args, "baseline", "master")
    baseline_label = baseline_name.upper()
    baseline_bin_orig, baseline_data_dir, baseline_source_dir, baseline_conn = BASELINE_CONFIGS[baseline_name]
    testbranch_name = getattr(args, "testbranch", "patch")
    test_bin_orig, test_data_dir, test_source_dir, test_conn = TESTBRANCH_CONFIGS[testbranch_name]
    test_has_prefetch = BUILD_HAS_PREFETCH[testbranch_name]
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

    # In cached mode, single-sample comparisons are too noisy for small
    # regressions.  Use multiple runs per query and compare representative
    # values (min or median) instead.
    stress_runs = args.runs if args.cached else 1

    print("=" * 60)
    print("STRESS TEST MODE")
    print("=" * 60)
    print(f"Looking for regressions >= {STRESS_REGRESSION_THRESHOLD:.0%} slower than {baseline_name}")
    print(f"Generating {STRESS_QUERIES_PER_BATCH} queries per batch")
    print(f"Runs per query: {stress_runs}" + (" (cached mode)" if stress_runs > 1 else ""))
    print(f"Minimum query duration: {args.min_query_ms:.1f} ms")
    mode_word = ("\033[32m💵  cached\033[0m" if args.cached
                 else "\033[34m💾  uncached\033[0m")
    print(f"Mode: {mode_word}")
    print("=" * 60)

    # Get git hashes
    master_hash = get_git_hash(baseline_source_dir)
    patch_hash = get_git_hash(test_source_dir)
    print(f"\nBaseline ({baseline_name}) git hash: {master_hash}")
    print(f"Patch git hash: {patch_hash}")
    print_io_settings(args)

    # We assume data is already loaded (--skip-load behavior for stress test)
    # User should run the normal benchmark first to ensure data exists

    iteration = 0
    total_queries_tested = 0

    try:
        while True:
            iteration += 1
            print(f"\n{'=' * 60}")
            print(f"ITERATION {iteration}")
            print(f"{'=' * 60}")

            # Generate batch of random queries
            queries = []
            for i in range(STRESS_QUERIES_PER_BATCH):
                query_num = total_queries_tested + i + 1
                queries.append((f"S{query_num}", generate_random_query(query_num)))

            # Print generated queries
            print(f"\nGenerated {len(queries)} queries:")
            for query_id, query_def in queries:
                print(f"  {query_id}: {query_def['name']}")

            # Results storage for this batch
            results = {}
            for query_id, query_def in queries:
                results[query_id] = {
                    "query_def": query_def,
                    "master": {"times": [], "min": None, "explain": None},
                    "patch_off": {"times": [], "min": None, "explain": None},
                    "patch_on": {"times": [], "min": None, "explain": None},
                }

            # Run all queries on baseline, replacing any that are too fast
            print(f"\n--- Running on BASELINE ({baseline_name}) ---")
            start_server(master_bin, baseline_name, baseline_data_dir, baseline_conn, args)
            try:
                master_conn = psycopg.connect(**baseline_conn)
                pin_backend(master_conn.info.backend_pid, args.benchmark_cpu, enabled=args.pin)
                with master_conn.cursor() as cur:
                    cur.execute("CREATE EXTENSION IF NOT EXISTS pg_prewarm")
                    cur.execute("CREATE EXTENSION IF NOT EXISTS pg_buffercache")
                    # Stress test GUC settings
                    cur.execute("SET enable_bitmapscan = off")
                    cur.execute("SET random_page_cost = 1.1")
                    cur.execute("SET max_parallel_workers_per_gather = 0")

                # Process each query slot, replacing too-fast queries
                for i in range(len(queries)):
                    query_id, query_def = queries[i]
                    replacement_count = 0

                    while True:
                        print(f"\n  {query_id}: {query_def['name']}")
                        # Print query text
                        for line in query_def['sql'].strip().split('\n'):
                            print(f"    {line.strip()}")
                        print(f"  Running...", end=" ", flush=True)
                        try:
                            prepare_query(master_conn, query_def["sql"],
                                          gucs=query_def.get("gucs", {}), is_master=True)
                            exec_time, explain_output = run_query(
                                master_conn, query_def, args.cached,
                                is_master=True, prefetch_setting=None,
                                benchmark_cpu=args.benchmark_cpu,
                                direct_io=args.direct_io
                            )
                            if exec_time is not None:
                                if exec_time < args.min_query_ms:
                                    deallocate_query(master_conn)
                                    replacement_count += 1
                                    print(f"{exec_time:.3f} ms (too fast, regenerating...)")
                                    # Generate a replacement query with same ID
                                    query_num = int(query_id[1:])  # Extract number from "S123"
                                    query_def = generate_random_query(query_num + replacement_count * 1000)
                                    queries[i] = (query_id, query_def)
                                    # Update results dict for new query
                                    results[query_id] = {
                                        "query_def": query_def,
                                        "master": {"times": [], "min": None, "explain": None},
                                        "patch_off": {"times": [], "min": None, "explain": None},
                                        "patch_on": {"times": [], "min": None, "explain": None},
                                    }
                                    continue  # Try again with new query
                                else:
                                    results[query_id]["master"]["times"].append(exec_time)
                                    results[query_id]["master"]["explain"] = explain_output
                                    print(f"{exec_time:.3f} ms")
                                    # Additional runs for cached mode
                                    for extra in range(stress_runs - 1):
                                        t, _ = run_query(
                                            master_conn, query_def, args.cached,
                                            is_master=True, prefetch_setting=None,
                                            benchmark_cpu=args.benchmark_cpu,
                                            direct_io=args.direct_io,
                                            skip_prewarm=args.cached
                                        )
                                        if t is not None:
                                            results[query_id]["master"]["times"].append(t)
                                            print(f"  Run {extra + 2}: {t:.3f} ms")
                                    results[query_id]["master"]["min"] = get_representative(
                                        results[query_id]["master"]["times"])
                                    deallocate_query(master_conn)
                                    break  # Query is acceptable, move to next slot
                            else:
                                print("FAILED - could not extract execution time")
                                sys.exit(1)
                        except Exception as e:
                            print(f"ERROR: {e}")
                            print("\nGenerated invalid SQL! This is a bug in the query generator.")
                            print(f"Query: {query_def['sql']}")
                            sys.exit(1)

                master_conn.close()
            finally:
                stop_server(master_bin, baseline_data_dir)
                time.sleep(2)

            # Run all queries on testbranch
            print(f"\n--- Running on TESTBRANCH ({testbranch_name}) ---")
            start_server(patch_bin, testbranch_name, test_data_dir, test_conn, args)
            try:
                patch_conn = psycopg.connect(**test_conn)
                pin_backend(patch_conn.info.backend_pid, args.benchmark_cpu, enabled=args.pin)
                with patch_conn.cursor() as cur:
                    cur.execute("CREATE EXTENSION IF NOT EXISTS pg_prewarm")
                    cur.execute("CREATE EXTENSION IF NOT EXISTS pg_buffercache")
                    # Stress test GUC settings
                    cur.execute("SET enable_bitmapscan = off")
                    cur.execute("SET random_page_cost = 1.1")
                    cur.execute("SET max_parallel_workers_per_gather = 0")

                BOLD = "\033[1m"
                RESET = "\033[0m"
                test_is_master = not test_has_prefetch

                for query_id, query_def in queries:
                    master_min = results[query_id]["master"]["min"]

                    print(f"\n  {query_id}: {query_def['name']}")
                    # Print query text
                    for line in query_def['sql'].strip().split('\n'):
                        print(f"    {line.strip()}")

                    prepare_query(patch_conn, query_def["sql"],
                                  gucs=query_def.get("gucs", {}), is_master=test_is_master)

                    # Test with prefetch OFF (skip if --prefetch-only or no prefetch GUC)
                    if not args.prefetch_only and test_has_prefetch:
                        print(f"  prefetch=off...", end=" ", flush=True)
                        try:
                            for run_i in range(stress_runs):
                                exec_time, explain_output = run_query(
                                    patch_conn, query_def, args.cached,
                                    is_master=False, prefetch_setting="off",
                                    benchmark_cpu=args.benchmark_cpu,
                                    direct_io=args.direct_io,
                                    skip_prewarm=(args.cached and run_i > 0)
                                )
                                if exec_time is not None:
                                    results[query_id]["patch_off"]["times"].append(exec_time)
                                    if run_i == 0:
                                        results[query_id]["patch_off"]["explain"] = explain_output
                                    if stress_runs > 1:
                                        print(f"{exec_time:.3f}", end=" " if run_i < stress_runs - 1 else "", flush=True)
                                else:
                                    print("FAILED - could not extract execution time")
                                    sys.exit(1)
                            patch_off_rep = get_representative(results[query_id]["patch_off"]["times"])
                            results[query_id]["patch_off"]["min"] = patch_off_rep
                            if master_min:
                                ratio = patch_off_rep / master_min
                                if stress_runs > 1:
                                    print(f"→ {patch_off_rep:.3f} ms ({BOLD}{ratio:.3f}x{RESET} vs baseline)")
                                else:
                                    print(f"{patch_off_rep:.3f} ms ({BOLD}{ratio:.3f}x{RESET} vs baseline)")
                            else:
                                print(f"→ {patch_off_rep:.3f} ms" if stress_runs > 1 else f"{patch_off_rep:.3f} ms")
                        except Exception as e:
                            print(f"ERROR: {e}")
                            print("\nGenerated invalid SQL! This is a bug in the query generator.")
                            print(f"Query: {query_def['sql']}")
                            sys.exit(1)

                    # Test with prefetch ON (skip if --prefetch-disabled)
                    # When testbranch has no prefetch GUC, run queries plain
                    if not args.prefetch_disabled:
                        if test_has_prefetch:
                            print(f"  prefetch=on...", end=" ", flush=True)
                            pf_setting = "on"
                        else:
                            print(f"  running...", end=" ", flush=True)
                            pf_setting = None
                        try:
                            for run_i in range(stress_runs):
                                exec_time, explain_output = run_query(
                                    patch_conn, query_def, args.cached,
                                    is_master=test_is_master, prefetch_setting=pf_setting,
                                    benchmark_cpu=args.benchmark_cpu,
                                    direct_io=args.direct_io,
                                    skip_prewarm=(args.cached and run_i > 0)
                                )
                                if exec_time is not None:
                                    results[query_id]["patch_on"]["times"].append(exec_time)
                                    if run_i == 0:
                                        results[query_id]["patch_on"]["explain"] = explain_output
                                    if stress_runs > 1:
                                        print(f"{exec_time:.3f}", end=" " if run_i < stress_runs - 1 else "", flush=True)
                                else:
                                    print("FAILED - could not extract execution time")
                                    sys.exit(1)
                            patch_on_rep = get_representative(results[query_id]["patch_on"]["times"])
                            results[query_id]["patch_on"]["min"] = patch_on_rep
                            if master_min:
                                ratio = patch_on_rep / master_min
                                if stress_runs > 1:
                                    print(f"→ {patch_on_rep:.3f} ms ({BOLD}{ratio:.3f}x{RESET} vs baseline)")
                                else:
                                    print(f"{patch_on_rep:.3f} ms ({BOLD}{ratio:.3f}x{RESET} vs baseline)")
                            else:
                                print(f"→ {patch_on_rep:.3f} ms" if stress_runs > 1 else f"{patch_on_rep:.3f} ms")
                        except Exception as e:
                            print(f"ERROR: {e}")
                            print("\nGenerated invalid SQL! This is a bug in the query generator.")
                            print(f"Query: {query_def['sql']}")
                            sys.exit(1)

                    deallocate_query(patch_conn)

                patch_conn.close()
            finally:
                stop_server(patch_bin, test_data_dir)
                time.sleep(2)

            # Check for regressions
            print(f"\n--- Checking for regressions ---")
            regressions_found = []

            for query_id, query_def in queries:
                r = results[query_id]
                master_min = r["master"]["min"]
                patch_off_min = r["patch_off"]["min"]
                patch_on_min = r["patch_on"]["min"]

                if master_min is None:
                    continue

                # Check prefetch=off regression
                if patch_off_min is not None:
                    ratio_off = patch_off_min / master_min
                    if ratio_off >= STRESS_REGRESSION_THRESHOLD:
                        regressions_found.append({
                            "query_id": query_id,
                            "query_def": query_def,
                            "config": "prefetch=off",
                            "ratio": ratio_off,
                            "master_ms": master_min,
                            "patch_ms": patch_off_min,
                            "patch_on_ms": patch_on_min,
                        })

                # Check prefetch=on regression
                if patch_on_min is not None:
                    ratio_on = patch_on_min / master_min
                    if ratio_on >= STRESS_REGRESSION_THRESHOLD:
                        regressions_found.append({
                            "query_id": query_id,
                            "query_def": query_def,
                            "config": "prefetch=on",
                            "ratio": ratio_on,
                            "master_ms": master_min,
                            "patch_ms": patch_on_min,
                            "patch_off_ms": patch_off_min,
                        })

            total_queries_tested += len(queries)

            # Verify apparent regressions with retries to filter out spurious failures
            if regressions_found:
                print(f"\n--- Verifying {len(regressions_found)} apparent regression(s) with retries ---")
                confirmed_regressions = []
                needs_rebaseline = []  # Regressions that survived retries, need master re-baseline

                start_server(patch_bin, testbranch_name, test_data_dir, test_conn, args)
                try:
                    patch_conn = psycopg.connect(**test_conn)
                    pin_backend(patch_conn.info.backend_pid, args.benchmark_cpu, enabled=args.pin)
                    with patch_conn.cursor() as cur:
                        cur.execute("CREATE EXTENSION IF NOT EXISTS pg_prewarm")
                        cur.execute("CREATE EXTENSION IF NOT EXISTS pg_buffercache")
                        cur.execute("SET enable_bitmapscan = off")
                        cur.execute("SET random_page_cost = 1.1")
                        cur.execute("SET max_parallel_workers_per_gather = 0")

                    for reg in regressions_found:
                        query_def = reg["query_def"]
                        master_avg = reg["master_ms"]
                        if test_has_prefetch:
                            prefetch_setting = "off" if reg["config"] == "prefetch=off" else "on"
                        else:
                            prefetch_setting = None

                        print(f"\n  Verifying {reg['query_id']} ({reg['config']}, initial {reg['ratio']:.3f}x)...")

                        prepare_query(patch_conn, query_def["sql"],
                                      gucs=query_def.get("gucs", {}),
                                      is_master=test_is_master, prefetch_setting=prefetch_setting)

                        # Simple retries: 3 attempts with short delay
                        RETRY_COUNT = 3
                        RETRY_DELAY = 1.0

                        regression_confirmed = True
                        for retry in range(RETRY_COUNT):
                            print(f"    Retry {retry + 1}/{RETRY_COUNT}...", end=" ", flush=True)
                            time.sleep(RETRY_DELAY)

                            retry_times = []
                            for run_i in range(stress_runs):
                                exec_time, _ = run_query(
                                    patch_conn, query_def, args.cached,
                                    is_master=test_is_master, prefetch_setting=prefetch_setting,
                                    benchmark_cpu=args.benchmark_cpu,
                                    direct_io=args.direct_io,
                                    skip_prewarm=(args.cached and run_i > 0)
                                )
                                if exec_time is not None:
                                    retry_times.append(exec_time)

                            if retry_times:
                                retry_rep = get_representative(retry_times)
                                ratio = retry_rep / master_avg
                                if stress_runs > 1:
                                    print(f"{' '.join(f'{t:.3f}' for t in retry_times)} → {retry_rep:.3f} ms ({ratio:.3f}x)")
                                else:
                                    print(f"{retry_rep:.3f} ms ({ratio:.3f}x)")

                                if ratio < STRESS_REGRESSION_THRESHOLD:
                                    print(f"    Under threshold - spurious failure, discarding")
                                    regression_confirmed = False
                                    break
                            else:
                                print("FAILED")

                        deallocate_query(patch_conn)

                        if regression_confirmed:
                            print(f"    Retries confirm regression - needs {baseline_name} re-baseline")
                            needs_rebaseline.append(reg)

                    patch_conn.close()
                finally:
                    stop_server(patch_bin, test_data_dir)
                    time.sleep(2)

                # Phase 2: Re-baseline verification for regressions that survived retries
                # This accounts for environmental drift since the original master baseline
                if needs_rebaseline:
                    print(f"\n--- Re-baselining {len(needs_rebaseline)} regression(s) against {baseline_name} ---")

                    for reg in needs_rebaseline:
                        query_def = reg["query_def"]
                        prefetch_setting = "off" if reg["config"] == "prefetch=off" else "on"

                        print(f"\n  Re-baselining {reg['query_id']} ({reg['config']})...")

                        # Run query on baseline to establish new baseline
                        start_server(master_bin, baseline_name, baseline_data_dir, baseline_conn, args)
                        try:
                            master_conn = psycopg.connect(**baseline_conn)
                            pin_backend(master_conn.info.backend_pid, args.benchmark_cpu, enabled=args.pin)
                            with master_conn.cursor() as cur:
                                cur.execute("CREATE EXTENSION IF NOT EXISTS pg_prewarm")
                                cur.execute("CREATE EXTENSION IF NOT EXISTS pg_buffercache")
                                cur.execute("SET enable_bitmapscan = off")
                                cur.execute("SET random_page_cost = 1.1")
                                cur.execute("SET max_parallel_workers_per_gather = 0")

                            prepare_query(master_conn, query_def["sql"],
                                          gucs=query_def.get("gucs", {}), is_master=True)
                            rebaseline_master_times = []
                            for run_i in range(stress_runs):
                                t, _ = run_query(
                                    master_conn, query_def, args.cached,
                                    is_master=True, prefetch_setting=None,
                                    benchmark_cpu=args.benchmark_cpu,
                                    direct_io=args.direct_io,
                                    skip_prewarm=(args.cached and run_i > 0)
                                )
                                if t is not None:
                                    rebaseline_master_times.append(t)
                            deallocate_query(master_conn)
                            master_conn.close()
                        finally:
                            stop_server(master_bin, baseline_data_dir)
                            time.sleep(2)

                        if not rebaseline_master_times:
                            print(f"    Baseline re-baseline FAILED, discarding regression")
                            continue

                        new_master_time = get_representative(rebaseline_master_times)
                        best_master_time = min(new_master_time, reg["master_ms"])
                        if stress_runs > 1:
                            print(f"    New {baseline_name} baseline: {' '.join(f'{t:.3f}' for t in rebaseline_master_times)} → {new_master_time:.3f} ms (was {reg['master_ms']:.3f} ms, using {best_master_time:.3f} ms)")
                        else:
                            print(f"    New {baseline_name} baseline: {new_master_time:.3f} ms (was {reg['master_ms']:.3f} ms, using {best_master_time:.3f} ms)")

                        # Run query on testbranch with new baseline
                        start_server(patch_bin, testbranch_name, test_data_dir, test_conn, args)
                        try:
                            patch_conn = psycopg.connect(**test_conn)
                            pin_backend(patch_conn.info.backend_pid, args.benchmark_cpu, enabled=args.pin)
                            with patch_conn.cursor() as cur:
                                cur.execute("CREATE EXTENSION IF NOT EXISTS pg_prewarm")
                                cur.execute("CREATE EXTENSION IF NOT EXISTS pg_buffercache")
                                cur.execute("SET enable_bitmapscan = off")
                                cur.execute("SET random_page_cost = 1.1")
                                cur.execute("SET max_parallel_workers_per_gather = 0")

                            prepare_query(patch_conn, query_def["sql"],
                                          gucs=query_def.get("gucs", {}),
                                          is_master=test_is_master, prefetch_setting=prefetch_setting)
                            rebaseline_patch_times = []
                            for run_i in range(stress_runs):
                                t, _ = run_query(
                                    patch_conn, query_def, args.cached,
                                    is_master=test_is_master, prefetch_setting=prefetch_setting,
                                    benchmark_cpu=args.benchmark_cpu,
                                    direct_io=args.direct_io,
                                    skip_prewarm=(args.cached and run_i > 0)
                                )
                                if t is not None:
                                    rebaseline_patch_times.append(t)
                            deallocate_query(patch_conn)
                            patch_conn.close()
                        finally:
                            stop_server(patch_bin, test_data_dir)
                            time.sleep(2)

                        if not rebaseline_patch_times:
                            print(f"    Patch re-run FAILED, discarding regression")
                            continue

                        new_patch_time = get_representative(rebaseline_patch_times)
                        best_patch_time = min(new_patch_time, reg["patch_ms"])
                        best_ratio = best_patch_time / best_master_time
                        if stress_runs > 1:
                            print(f"    New patch time: {' '.join(f'{t:.3f}' for t in rebaseline_patch_times)} → {new_patch_time:.3f} ms (was {reg['patch_ms']:.3f} ms, using {best_patch_time:.3f} ms)")
                        else:
                            print(f"    New patch time: {new_patch_time:.3f} ms (was {reg['patch_ms']:.3f} ms, using {best_patch_time:.3f} ms)")
                        print(f"    Best ratio: {best_ratio:.3f}x (orig {reg['ratio']:.3f}x, rebaseline {new_patch_time / new_master_time:.3f}x)")

                        if best_ratio >= STRESS_REGRESSION_THRESHOLD:
                            print(f"    Regression CONFIRMED with fresh baseline")
                            # Update reg with best measurements from either run
                            reg["master_ms"] = best_master_time
                            reg["patch_ms"] = best_patch_time
                            reg["ratio"] = best_ratio
                            confirmed_regressions.append(reg)
                        else:
                            print(f"    Below threshold - discarding (orig {reg['ratio']:.3f}x, rebaseline {new_patch_time / new_master_time:.3f}x, best {best_ratio:.3f}x)")

                regressions_found = confirmed_regressions

            if regressions_found:
                # Sort by ratio (worst first)
                regressions_found.sort(key=lambda x: x["ratio"], reverse=True)
                worst = regressions_found[0]

                failure_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                print()
                print("=" * 60)
                print("REGRESSION FOUND!")
                print(f"Time: {failure_time}")
                print("=" * 60)
                pct = (worst["ratio"] - 1) * 100
                print(f"Patch ({worst['config']}) is {worst['ratio']:.3f}x slower than {baseline_name} ({pct:.1f}% regression)")
                print()
                print(f"{baseline_label + ':':18s}{worst['master_ms']:.3f} ms")

                query_def = worst["query_def"]
                r = results[worst["query_id"]]

                if r["patch_off"]["min"]:
                    ratio_off = r["patch_off"]["min"] / worst["master_ms"]
                    marker = " <-- REGRESSION" if worst["config"] == "prefetch=off" else ""
                    print(f"Patch (off):      {r['patch_off']['min']:.3f} ms ({ratio_off:.3f}x vs {baseline_name}){marker}")

                if r["patch_on"]["min"]:
                    ratio_on = r["patch_on"]["min"] / worst["master_ms"]
                    marker = " <-- REGRESSION" if worst["config"] == "prefetch=on" else ""
                    print(f"Patch (on):       {r['patch_on']['min']:.3f} ms ({ratio_on:.3f}x vs {baseline_name}){marker}")

                # Print EXPLAIN ANALYZE output
                print()
                print("EXPLAIN ANALYZE output:")
                print()
                if r["master"]["explain"]:
                    print(f"  {baseline_name}:")
                    for line in r["master"]["explain"].split('\n'):
                        print(f"    {line}")
                if r["patch_off"]["explain"]:
                    print()
                    print("  patch (prefetch=off):")
                    for line in r["patch_off"]["explain"].split('\n'):
                        print(f"    {line}")
                if r["patch_on"]["explain"]:
                    print()
                    print("  patch (prefetch=on):")
                    for line in r["patch_on"]["explain"].split('\n'):
                        print(f"    {line}")

                # Print query definition ready to add to QUERIES
                print()
                print("Add this to QUERIES dict:")
                print()

                # Format evict list
                evict_str = ", ".join(f'"{t}"' for t in query_def["evict"])
                prewarm_idx_str = ", ".join(f'"{i}"' for i in query_def["prewarm_indexes"])
                prewarm_tbl_str = ", ".join(f'"{t}"' for t in query_def["prewarm_tables"])

                # Clean up SQL formatting
                sql_lines = query_def["sql"].strip().split('\n')
                sql_formatted = '\n'.join('            ' + line.strip() for line in sql_lines)

                print(f'    ("STRESS_{total_queries_tested}", {{')
                print(f'        "name": "{query_def["name"]}",')
                print(f'        "sql": """')
                print(sql_formatted)
                print(f'        """,')
                print(f'        "evict": [{evict_str}],')
                print(f'        "prewarm_indexes": [{prewarm_idx_str}],')
                print(f'        "prewarm_tables": [{prewarm_tbl_str}],')
                if query_def.get("gucs"):
                    gucs_str = ", ".join(f'"{k}": "{v}"' for k, v in query_def["gucs"].items())
                    print(f'        "gucs": {{{gucs_str}}},')
                print(f'    }}),')
                print()
                print(f"Total queries tested: {total_queries_tested}")
                print(f"Iterations: {iteration}")
                return  # Stop on confirmed regression

            else:
                print(f"No regressions found in this batch.")
                print(f"Total queries tested so far: {total_queries_tested}")
                print("Generating next batch...")

    except KeyboardInterrupt:
        print(f"\n\nStress test interrupted after {total_queries_tested} queries ({iteration} iterations)")
        print("No regressions found.")
    finally:
        # Cleanup tmpfs if it was created
        if tmpfs_mount:
            cleanup_tmpfs(tmpfs_mount)


def _format_duration(seconds):
    mins = int(seconds // 60)
    secs = int(seconds % 60)
    if mins == 0:
        return f"{secs} seconds"
    elif mins == 1:
        return f"1 minute {secs} seconds"
    else:
        return f"{mins} minutes {secs} seconds"


def _print_wall_time(wall_start):
    wall_end = time.time()
    start_dt = datetime.fromtimestamp(wall_start)
    end_dt = datetime.fromtimestamp(wall_end)
    print(f"\n{'=' * 60}")
    print(f"Started:       {start_dt.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Finished:      {end_dt.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Total runtime: {_format_duration(wall_end - wall_start)}")
    print(f"{'=' * 60}")


def main():
    global USE_MEDIAN
    wall_start = time.time()
    args = parse_arguments()

    # Set global median flag
    USE_MEDIAN = args.use_median

    # Update data dirs if using delay pgdata
    if args.delay_pgdata:
        # Update baseline config
        base_bin, _base_data, base_src, base_conn = BASELINE_CONFIGS[args.baseline]
        base_dir = os.path.dirname(_base_data)
        base_delay = os.path.join(base_dir, "data-delay")
        BASELINE_CONFIGS[args.baseline] = (base_bin, base_delay, base_src, base_conn)
        # Update testbranch config
        test_bin, _test_data, test_src, test_conn = TESTBRANCH_CONFIGS[args.testbranch]
        test_dir = os.path.dirname(_test_data)
        test_delay = os.path.join(test_dir, "data-delay")
        TESTBRANCH_CONFIGS[args.testbranch] = (test_bin, test_delay, test_src, test_conn)
        print(f"Using delayed I/O data directories:")
        print(f"  Baseline ({args.baseline}): {base_delay}")
        print(f"  Testbranch ({args.testbranch}): {test_delay}")

    if args.list_modes:
        modes_json = []
        for suite in BENCHMARK_SUITES:
            modes_json.append({
                "mode_prefix": suite["mode_prefix"],
                "cli_flag": suite["cli_flag"],
                "uncached_runs": suite["uncached_runs"],
                "cached_runs": suite["cached_runs"],
            })
        print(json.dumps(modes_json))
        return

    if args.stress_test:
        run_stress_test(args)
        _print_wall_time(wall_start)
        return

    # Find the selected suite (default is first entry, "benchmark")
    selected = BENCHMARK_SUITES[0]
    for suite in BENCHMARK_SUITES[1:]:
        if suite["cli_dest"] and getattr(args, suite["cli_dest"], False):
            selected = suite
            break

    cache_tag = "cached" if args.cached else "uncached"
    run_generic_benchmark(args, selected["queries"],
                          f"{selected['mode_prefix']}_{cache_tag}",
                          selected["title"],
                          selected["verify_fn"], selected["load_fn"],
                          sync_stats=selected.get("sync_stats", False),
                          tables=selected.get("tables"),
                          expect_all_visible=selected.get("expect_all_visible", True))
    _print_wall_time(wall_start)


if __name__ == "__main__":
    main()
