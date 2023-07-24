#!/usr/bin/env python3
"""
Shared module for PostgreSQL benchmark scripts.

Contains common configuration, query definitions, cache management,
tmpfs handling, and data loading utilities used by both prefetch_benchmark.py
and perf_flamegraph.py.
"""

import re
import subprocess
import sys
import time
from collections import OrderedDict

import psycopg

# --- Configuration ---

MASTER_BIN = "/mnt/nvme/postgresql/master/install_meson_rc/bin"
PATCH_BIN = "/mnt/nvme/postgresql/patch/install_meson_rc/bin"
MASTER_DATA_DIR = "/mnt/nvme/postgresql/master/data"
PATCH_DATA_DIR = "/mnt/nvme/postgresql/patch/data"
MASTER_SOURCE_DIR = "/mnt/nvme/postgresql/master/source"
PATCH_SOURCE_DIR = "/mnt/nvme/postgresql/patch/source"

REL18_BIN = "/mnt/nvme/postgresql/REL_18_STABLE/install_meson_rc/bin"
REL18_DATA_DIR = "/mnt/nvme/postgresql/REL_18_STABLE/data"
REL18_SOURCE_DIR = "/mnt/nvme/postgresql/REL_18_STABLE/source"

MASTER_CONN = {
    "dbname": "regression",
    "user": "pg",
    "host": "/tmp",
    "port": 5555,
}
PATCH_CONN = {
    "dbname": "regression",
    "user": "pg",
    "host": "/tmp",
    "port": 5432,
}
REL18_CONN = {
    "dbname": "regression",
    "user": "pg",
    "host": "/tmp",
    "port": 5418,
}

# Map baseline name to (bin, data_dir, source_dir, conn) for easy lookup
BASELINE_CONFIGS = {
    "master": (MASTER_BIN, MASTER_DATA_DIR, MASTER_SOURCE_DIR, MASTER_CONN),
    "rel18":  (REL18_BIN, REL18_DATA_DIR, REL18_SOURCE_DIR, REL18_CONN),
}

# Map testbranch name to (bin, data_dir, source_dir, conn) for easy lookup
TESTBRANCH_CONFIGS = {
    "patch":  (PATCH_BIN, PATCH_DATA_DIR, PATCH_SOURCE_DIR, PATCH_CONN),
    "master": (MASTER_BIN, MASTER_DATA_DIR, MASTER_SOURCE_DIR, MASTER_CONN),
}

# Whether each build has the enable_indexscan_prefetch GUC
BUILD_HAS_PREFETCH = {
    "patch": True,
    "master": False,
    "rel18": False,
}

# Tables to verify for data existence
EXPECTED_TABLES = ['prefetch_orders', 'prefetch_customers', 'prefetch_products']

# --- Query Definitions ---

QUERIES = OrderedDict([
    ("Q1", {
        "name": "Simple date range scan, plain scan, 25x faster when uncached",
        "sql": """
            SELECT order_id, customer_id, amount
            FROM prefetch_orders
            WHERE order_date BETWEEN '2023-06-01' AND '2023-06-15'
            ORDER BY order_date
            LIMIT 50000
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("Q2", {
        "name": "JOIN Orders + Customers, merge join, plain index scan on both sides",
        "sql": """
            SELECT o.order_id, c.customer_name, o.amount, o.order_date
            FROM prefetch_orders o
            JOIN prefetch_customers c ON c.customer_id = o.customer_id
            WHERE o.customer_id BETWEEN 5000 AND 5500
              AND o.order_date BETWEEN '2023-06-01' AND '2023-06-30'
            ORDER BY o.order_date
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("Q4", {
        "name": "Aggregation with IN list, plain index scan, uses hash agg + sort",
        "sql": """
            SELECT order_date, sum(amount) AS total, count(*) AS orders
            FROM prefetch_orders
            WHERE customer_id IN (1000, 2000, 3000, 4000, 5000,
                                  6000, 7000, 8000, 9000, 10000)
              AND order_date BETWEEN '2023-01-01' AND '2023-12-31'
            GROUP BY order_date
            ORDER BY order_date
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("Q5", {
        "name": "Backwards scan with JOIN",
        "sql": """
            SELECT o.order_id, c.customer_name, o.amount, o.order_date
            FROM prefetch_orders o
            JOIN prefetch_customers c ON c.customer_id = o.customer_id
            WHERE o.customer_id BETWEEN 10000 AND 10500
              AND o.order_date BETWEEN '2023-09-01' AND '2023-09-30'
            ORDER BY o.customer_id DESC, o.order_date DESC
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
        "gucs": {"enable_sort": "off"},
    }),
    ("Q6", {
        "name": "correlated subplan, plain index scan, subplan uses plain index scan",
        "sql": """
            SELECT c.customer_name, (
              SELECT sum(amount) FROM prefetch_orders o
              WHERE o.customer_id = c.customer_id
            )
            FROM prefetch_customers c
            WHERE c.customer_id BETWEEN 1000 AND 1100
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("Q7", {
        "name": "LIMIT with large OFFSET, plain index scan",
        "sql": """
            SELECT * FROM prefetch_orders
            WHERE customer_id = 5000
            ORDER BY order_date
            OFFSET 400 LIMIT 100
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("Q8", {
        "name": "Nested loop semi-join with EXISTS, plain outer, plain inner with filter qual",
        "sql": """
            SELECT c.customer_id, c.customer_name
            FROM prefetch_customers c
            WHERE EXISTS (
              SELECT 1 FROM prefetch_orders o
              WHERE o.customer_id = c.customer_id
                AND o.order_date BETWEEN '2023-07-01' AND '2023-07-31'
                AND o.amount > 900
            )
            AND c.region_id = 5
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("Q9", {
        "name": "Lateral join (Top N per group), nestloop join, plain on both sides, Limit node above inner ",
        "sql": """
            SELECT c.customer_name, recent.*
            FROM prefetch_customers c,
            LATERAL (
              SELECT order_id, order_date, amount
              FROM prefetch_orders o
              WHERE o.customer_id = c.customer_id
              ORDER BY o.order_date DESC
              LIMIT 5
            ) recent
            WHERE c.customer_id BETWEEN 2000 AND 2100
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    # --- MergeJoin mark/restore tests ---
    # These isolate the mark/restore code path in the prefetch machinery.
    # MergeJoin inner side uses mark/restore (not full rescan), which resets
    # prefetchPos and tears down the read stream on every restore.
    # Self-join on orders: outer has ~20 rows/customer (duplicate join keys),
    # forcing ~20 restores per group on the inner side.
    ("MR1", {
        "name": "MergeJoin mark/restore, self-join, ~20 restores/group, ~62 inner rows/group",
        "sql": """
            SELECT count(*)
            FROM prefetch_orders o1
            JOIN prefetch_orders o2 ON o1.customer_id = o2.customer_id
            WHERE o1.customer_id BETWEEN 5000 AND 5100
              AND o1.order_date BETWEEN '2023-06-01' AND '2023-06-30'
              AND o2.order_date BETWEEN '2023-01-01' AND '2023-03-31'
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
        "gucs": {
            "enable_hashjoin": "off",
            "enable_nestloop": "off",
        },
    }),
    ("MR2", {
        "name": "MergeJoin mark/restore, self-join, 501 groups, ~10 inner rows/group, high restore frequency",
        "sql": """
            SELECT count(*)
            FROM prefetch_orders o1
            JOIN prefetch_orders o2 ON o1.customer_id = o2.customer_id
            WHERE o1.customer_id BETWEEN 5000 AND 5500
              AND o1.order_date BETWEEN '2023-06-01' AND '2023-06-30'
              AND o2.order_date BETWEEN '2023-01-01' AND '2023-01-15'
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
        "gucs": {
            "enable_hashjoin": "off",
            "enable_nestloop": "off",
        },
    }),
    ("MR3", {
        "name": "MR1 plain index scan variant",
        "sql": """
            SELECT count(*)
            FROM prefetch_orders o1
            JOIN prefetch_orders o2 ON o1.customer_id = o2.customer_id
            WHERE o1.customer_id BETWEEN 5000 AND 5100
              AND o1.order_date BETWEEN '2023-06-01' AND '2023-06-30'
              AND o2.order_date BETWEEN '2023-01-01' AND '2023-03-31'
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
        "gucs": {
            "enable_hashjoin": "off",
            "enable_nestloop": "off",
            "enable_indexonlyscan": "off",
        },
    }),
    ("MR4", {
        "name": "MR2 plain index scan variant",
        "sql": """
            SELECT count(*)
            FROM prefetch_orders o1
            JOIN prefetch_orders o2 ON o1.customer_id = o2.customer_id
            WHERE o1.customer_id BETWEEN 5000 AND 5500
              AND o1.order_date BETWEEN '2023-06-01' AND '2023-06-30'
              AND o2.order_date BETWEEN '2023-01-01' AND '2023-01-15'
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
        "gucs": {
            "enable_hashjoin": "off",
            "enable_nestloop": "off",
            "enable_indexonlyscan": "off",
        },
    }),
    # --- Zero-row rescan tests ---
    # These test the per-rescan overhead of the prefetch machinery when the
    # inner index scan frequently finds 0 rows.  The setup/teardown cost of
    # the read stream dominates when there is no actual IO benefit.
    ("ZR1", {
        "name": "LATERAL, 1001 rescans, ~32% find 0 rows (single-date probe per customer)",
        "sql": """
            SELECT c.customer_id, o.order_id, o.amount
            FROM prefetch_customers c,
            LATERAL (
                SELECT order_id, amount
                FROM prefetch_orders
                WHERE customer_id = c.customer_id
                  AND order_date = '2023-06-15'
            ) o
            WHERE c.customer_id BETWEEN 1000 AND 2000
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("ZR2", {
        "name": "NOT EXISTS, 5001 rescans, ~32% find 0 rows, pure per-rescan overhead",
        "sql": """
            SELECT count(*)
            FROM prefetch_customers c
            WHERE c.customer_id BETWEEN 1000 AND 6000
              AND NOT EXISTS (
                  SELECT 1 FROM prefetch_orders o
                  WHERE o.customer_id = c.customer_id
                    AND o.order_date = '2023-06-15'
              )
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    # Adversarial queries
    ("A1", {
        "name": "Sequential heap access (adversarial)",
        "sql": """
            SELECT * FROM prefetch_sequential
            WHERE id BETWEEN 1 AND 200000
            ORDER BY id
        """,
        "evict": ["prefetch_sequential"],
        "prewarm_indexes": ["prefetch_sequential_idx"],
        "prewarm_tables": ["prefetch_sequential"],
        "setup_table": "prefetch_sequential",
    }),
    ("A3", {
        "name": "High cache hit ratio, plain index scan",
        "sql": """
            SELECT * FROM prefetch_orders
            WHERE customer_id = 1
              AND order_date BETWEEN '2023-01-01' AND '2023-12-31'
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A4", {
        "name": "Early LIMIT termination plain index scan (adversarial)",
        "sql": """
            SELECT * FROM prefetch_orders
            WHERE order_date BETWEEN '2023-01-01' AND '2023-12-31'
            ORDER BY order_date
            LIMIT 10
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A5", {
        "name": "Sparse - 1 TID per block (adversarial)",
        "sql": """
            SELECT id, category FROM prefetch_sparse
            WHERE category BETWEEN 10 AND 20
        """,
        "evict": ["prefetch_sparse"],
        "prewarm_indexes": ["prefetch_sparse_cat_idx"],
        "prewarm_tables": ["prefetch_sparse"],
        "setup_table": "prefetch_sparse",
    }),
    ("A6", {
        "name": "NL inner with LIMIT 1 (adversarial)",
        "sql": """
            SELECT c.customer_name, (
              SELECT amount FROM prefetch_orders o
              WHERE o.customer_id = c.customer_id
              ORDER BY o.order_date DESC
              LIMIT 1
            )
            FROM prefetch_customers c
            WHERE c.customer_id BETWEEN 1000 AND 2000
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("A7", {
        "name": "Index Only Scan (adversarial)",
        "sql": """
            SELECT c.customer_id, c.customer_name
            FROM prefetch_customers c
            WHERE NOT EXISTS (
              SELECT 1 FROM prefetch_orders o
              WHERE o.customer_id = c.customer_id
                AND o.order_date BETWEEN '2023-12-01' AND '2023-12-31'
            )
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("A8", {
        "name": "LATERAL regression, nestloop join with limit on inner size, both sides use plain index scans",
        "sql": """
            SELECT c.customer_id, o.order_id, o.order_date, o.amount
            FROM prefetch_customers c,
            LATERAL (
            SELECT order_id, order_date, amount
            FROM prefetch_orders
            WHERE customer_id = c.customer_id
            AND order_date BETWEEN '2023-05-01' AND '2023-05-20'

            ORDER BY order_date
            LIMIT 3
            ) o
            WHERE c.customer_id BETWEEN 42976 AND 43285
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("A9", {
        "name": "Found a bug in hold-onto-pin-after-all IoS fix",
        "sql": """
            SELECT order_date, count(*) as cnt
            FROM prefetch_orders
            WHERE order_date BETWEEN '2023-11-21' AND '2023-12-01'

            GROUP BY order_date
            ORDER BY order_date
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A10", {
        "name": "regression: LATERAL, outer index-only scan, inner side plain index scan backwards",
        "sql": """
            SELECT c.customer_id, o.order_id, o.order_date, o.amount
            FROM prefetch_customers c,
            LATERAL (
            SELECT order_id, order_date, amount
            FROM prefetch_orders
            WHERE customer_id = c.customer_id
            AND order_date BETWEEN '2023-11-05' AND '2023-11-06'

            ORDER BY order_date DESC
            LIMIT 3
            ) o
            WHERE c.customer_id BETWEEN 67633 AND 68569
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("A11", {
        "name": "Merge Anti Join regression, index-only scan on outer side, plain index scan on inner side",
        "sql": """
            SELECT o.customer_id, o.order_date
            FROM prefetch_orders o
            WHERE o.order_date BETWEEN '2023-04-16' AND '2023-05-01'
            AND o.customer_id BETWEEN 46586 AND 47418
            AND NOT EXISTS (
            SELECT 1 FROM prefetch_customers c
            WHERE c.customer_id = o.customer_id
            AND c.region_id = 14
            )
            ORDER BY o.order_date
            LIMIT 37895
        """,
        "evict": ["prefetch_customers", "prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_customers", "prefetch_orders"],
    }),
    ("A12", {
        "name": "Merge Anti Join regression, index-only",
        "sql": """
            SELECT o.customer_id, o.order_date
            FROM prefetch_orders o
            WHERE o.order_date BETWEEN '2023-10-21' AND '2023-10-23'
            AND o.customer_id BETWEEN 15454 AND 23078
            AND NOT EXISTS (
            SELECT 1 FROM prefetch_customers c
            WHERE c.customer_id = o.customer_id
            AND c.region_id = 18
            )
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("A13", {
        "name": "New index-only scan aggregate regression",
        "sql": """
            SELECT order_date, count(*) as cnt
            FROM prefetch_orders
            WHERE order_date BETWEEN '2023-02-18' AND '2023-02-28'

            GROUP BY order_date
            ORDER BY order_date
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A14", {
        "name": "Regressed range-scan, index-only",
        "sql": """
            SELECT product_id
            FROM prefetch_orders
            WHERE product_id BETWEEN 3787 AND 6238
            ORDER BY product_id
            LIMIT 49763
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_prod_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A15", {
        "name": "regressed anti-join, index-only, relies most on pausing mechanism",
        "sql": """
            SELECT o.customer_id, o.order_date
            FROM prefetch_orders o
            WHERE o.order_date BETWEEN '2023-02-09' AND '2023-02-23'
            AND o.customer_id BETWEEN 36307 AND 37126
            AND NOT EXISTS (
            SELECT 1 FROM prefetch_customers c
            WHERE c.customer_id = o.customer_id
            AND c.region_id = 4
            )
            ORDER BY o.order_date
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("A16", {
        "name": "correlated query with sort, index scan, and index scan subplan, regressed with --cache mode only",
        "sql": """
            SELECT o.order_id, o.customer_id, o.amount,
            (SELECT c.customer_name FROM prefetch_customers c
            WHERE c.customer_id = o.customer_id) as cust_name
            FROM prefetch_orders o
            WHERE o.order_date BETWEEN '2023-01-27' AND '2023-04-11'
            AND o.customer_id BETWEEN 77305 AND 77845
            ORDER BY o.order_date
        """,
        "evict": ["prefetch_customers", "prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_customers", "prefetch_orders"],
    }),
    ("A17", {
        "name": "regressed aggregate index-only scan",
        "sql": """
            SELECT order_date, count(*) as cnt
            FROM prefetch_orders
            WHERE order_date BETWEEN '2023-11-15' AND '2023-11-17'

            GROUP BY order_date
            ORDER BY order_date
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A18", {
        "name": "Stress #416: LATERAL, backwards, plain scans on both sides, Limit above inner node",
        "sql": """
            SELECT c.customer_id, o.order_id, o.order_date, o.amount
            FROM prefetch_customers c,
            LATERAL (
            SELECT order_id, order_date, amount
            FROM prefetch_orders
            WHERE customer_id = c.customer_id
            AND order_date BETWEEN '2023-08-06' AND '2023-08-19'

            ORDER BY order_date DESC
            LIMIT 5
            ) o
            WHERE c.customer_id BETWEEN 74197 AND 75043
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("A19", {
        "name": "Stress #1041: range-scan, index-only",
        "sql": """
            SELECT customer_id, order_date
            FROM prefetch_orders
            WHERE customer_id BETWEEN 91411 AND 91684
            AND order_date BETWEEN '2023-02-15' AND '2023-02-23'
            ORDER BY order_date
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A20", {
        "name": "1.087x slower than master anti-join, outer side index filter qual excludes many rows, LIMIT can't get pushed under join",
        "sql": """
            SELECT o.order_id, o.customer_id, o.amount
            FROM prefetch_orders o
            WHERE o.order_date BETWEEN '2023-11-16' AND '2023-11-30'
            AND o.customer_id BETWEEN 71079 AND 71939
            AND NOT EXISTS (
            SELECT 1 FROM prefetch_customers c
            WHERE c.customer_id = o.customer_id
            AND c.region_id = 18
            )
            ORDER BY o.order_date
            LIMIT 2
        """,
        "evict": ["prefetch_customers", "prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_customers", "prefetch_orders"],
    }),
    ("A21", {
        "name": "correlated query, index scan, and index scan subplan, regressed 1.063x with --cache",
        "sql": """
            SELECT o.order_id, o.customer_id, o.amount,
            (SELECT c.customer_name FROM prefetch_customers c
            WHERE c.customer_id = o.customer_id) as cust_name
            FROM prefetch_orders o
            WHERE o.order_date BETWEEN '2023-05-30' AND '2023-06-08'
            AND o.customer_id BETWEEN 82712 AND 83606
        """,
        "evict": ["prefetch_customers", "prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_customers", "prefetch_orders"],
    }),
    ("A22", {
        "name": "correlated query, index scan, and index scan subplan, regressed 1.072x with --cache",
        "sql": """
            SELECT o.order_id, o.customer_id, o.amount,
            (SELECT c.customer_name FROM prefetch_customers c
            WHERE c.customer_id = o.customer_id) as cust_name
            FROM prefetch_orders o
            WHERE o.order_date BETWEEN '2023-10-07' AND '2023-12-08'
            AND o.customer_id BETWEEN 27730 AND 28371
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("A23", {
        "name": "regressed uncached aggregate index-only scan",
        "sql": """
            SELECT order_date, count(*) as cnt
            FROM prefetch_orders
            WHERE order_date BETWEEN '2023-09-22' AND '2023-10-07'

            GROUP BY order_date
            ORDER BY order_date
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A24", {
        "name": "regressed range-scan, index-only #1",
        "sql": """
            SELECT product_id
            FROM prefetch_orders
            WHERE product_id BETWEEN 2944 AND 3177
            ORDER BY product_id
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_prod_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A25", {
        "name": "regressed range-scan, index-only #2",
        "sql": """
            SELECT order_date
            FROM prefetch_orders
            WHERE order_date BETWEEN '2023-10-10' AND '2023-10-12'
            ORDER BY order_date
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A26", {
        "name": "LATERAL, once broke LIMIT heuristic, nestloop join with plain scans on both sides",
        "sql": """
            SELECT c.customer_id, o.order_id, o.order_date, o.amount
            FROM prefetch_customers c,
            LATERAL (
            SELECT order_id, order_date, amount
            FROM prefetch_orders
            WHERE customer_id = c.customer_id
            AND order_date BETWEEN '2023-09-09' AND '2023-10-16'

            ORDER BY order_date
            LIMIT 10
            ) o
            WHERE c.customer_id BETWEEN 10634 AND 10636
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("A27", {
        "name": "Merge join, 2 extra buffer hits on inner plain index scan, problem with --cached",
        "sql": """
            SELECT o.order_id, p.product_name, o.amount, o.order_date
            FROM prefetch_orders o
            JOIN prefetch_products p ON p.product_id = o.product_id
            WHERE o.product_id BETWEEN 705 AND 714
            AND o.order_date BETWEEN '2023-09-11' AND '2023-11-07'
            ORDER BY o.order_date
        """,
        "evict": ["prefetch_products", "prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_prod_idx", "prefetch_products_pkey"],
        "prewarm_tables": ["prefetch_products", "prefetch_orders"],
    }),
    ("A28", {
        "name": "Regressed plain index range-scan, problem with --cached, about 1.023x",
        "sql": """
            SELECT order_id, product_id, amount
            FROM prefetch_orders
            WHERE product_id BETWEEN 7445 AND 7453
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_prod_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A29", {
        "name": "Stress #15: correlated",
        "sql": """
            SELECT o.order_id, o.customer_id, o.amount,
            (SELECT c.customer_name FROM prefetch_customers c
            WHERE c.customer_id = o.customer_id) as cust_name
            FROM prefetch_orders o
            WHERE o.order_date BETWEEN '2023-01-02' AND '2023-02-18'
            AND o.customer_id BETWEEN 76340 AND 76504
            ORDER BY o.order_date
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
    ("A30", {
        "name": "Stress #142: aggregate, filter",
        "sql": """
            SELECT order_date, sum(amount) as total
            FROM prefetch_orders
            WHERE order_date BETWEEN '2023-01-27' AND '2023-01-29'
            AND amount BETWEEN 495 AND 894
            GROUP BY order_date
            ORDER BY order_date
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_date_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A31", {
        "name": "Stress #34: range-scan",
        "sql": """
            SELECT order_id, product_id, amount
            FROM prefetch_orders
            WHERE product_id BETWEEN 3254 AND 3262
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_prod_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A32", {
        "name": "Stress #89: range-scan",
        "sql": """
            SELECT order_id, product_id, amount
            FROM prefetch_orders
            WHERE product_id BETWEEN 291 AND 514
            ORDER BY product_id
            LIMIT 27953
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_prod_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A33", {
        "name": "Stress #1295: range-scan",
        "sql": """
            SELECT order_id, product_id, amount
            FROM prefetch_orders
            WHERE product_id BETWEEN 4305 AND 8368
            ORDER BY product_id
            LIMIT 25205
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_prod_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A34", {
        "name": "Stress #263: range-scan, filter",
        "sql": """
            SELECT order_id, product_id, amount
            FROM prefetch_orders
            WHERE product_id BETWEEN 4239 AND 4403
            AND amount BETWEEN 150 AND 417
            ORDER BY product_id
            LIMIT 10
        """,
        "evict": ["prefetch_orders"],
        "prewarm_indexes": ["prefetch_orders_prod_idx"],
        "prewarm_tables": ["prefetch_orders"],
    }),
    ("A35", {
        "name": "Regression when noncached, lateral join with inner plain scan, outer index-only scan",
        "sql": """
            SELECT c.customer_id, o.order_id, o.order_date, o.amount
            FROM prefetch_customers c,
            LATERAL (
            SELECT order_id, order_date, amount
            FROM prefetch_orders
            WHERE customer_id = c.customer_id
            AND order_date BETWEEN '2023-04-29' AND '2023-05-15'

            ORDER BY order_date
            LIMIT 9
            ) o
            WHERE c.customer_id BETWEEN 97085 AND 97089
        """,
        "evict": ["prefetch_orders", "prefetch_customers"],
        "prewarm_indexes": ["prefetch_orders_cust_date_idx", "prefetch_customers_pkey"],
        "prewarm_tables": ["prefetch_orders", "prefetch_customers"],
    }),
])


# --- Data Loading SQL ---

DATA_LOADING_SQL = """
-- Create extensions
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

-- Ensure deterministic data loading across PG versions
SET synchronize_seqscans = off;

-- Drop existing tables
DROP TABLE IF EXISTS prefetch_orders CASCADE;
DROP TABLE IF EXISTS prefetch_customers CASCADE;
DROP TABLE IF EXISTS prefetch_products CASCADE;
DROP TABLE IF EXISTS prefetch_sequential CASCADE;
DROP TABLE IF EXISTS prefetch_sparse CASCADE;

-- Main fact table: ~50M rows with low fillfactor
CREATE TABLE prefetch_orders (
  order_id bigint,
  customer_id int,
  product_id int,
  order_date date,
  region_id int,
  amount numeric(10,2)
) WITH (fillfactor = 40);

-- Dimension tables
CREATE TABLE prefetch_customers (
  customer_id int PRIMARY KEY,
  region_id int,
  customer_name text
);

CREATE TABLE prefetch_products (
  product_id int PRIMARY KEY,
  category_id int,
  product_name text
);

-- Load customers (100K)
INSERT INTO prefetch_customers (customer_id, region_id, customer_name)
SELECT i, (i % 20) + 1, 'Customer_' || i
FROM generate_series(1, 100000) i;

-- Load products (10K)
INSERT INTO prefetch_products (product_id, category_id, product_name)
SELECT i, (i % 50) + 1, 'Product_' || i
FROM generate_series(1, 10000) i;

-- Set deterministic seed
SELECT setseed(0.5);

-- Load orders with controlled scatter pattern
INSERT INTO prefetch_orders (order_id, customer_id, product_id, order_date, region_id, amount)
SELECT
  row_number() over () as order_id,
  customer_id,
  product_id,
  order_date,
  (customer_id % 20) + 1 as region_id,
  (random() * 1000)::numeric(10,2) as amount
FROM (
  SELECT
    ((g.i - 1) % 100000) + 1 as customer_id,
    ((g.i - 1) % 10000) + 1 as product_id,
    '2023-01-01'::date + ((g.i - 1) % 730) as order_date
  FROM generate_series(1, 50000000) g(i)
  ORDER BY (g.i / 32) + (random() * 4 - 2)::int, g.i
) sub;

-- Create indexes
CREATE INDEX prefetch_orders_cust_date_idx
  ON prefetch_orders(customer_id, order_date)
  WITH (deduplicate_items=off);

CREATE INDEX prefetch_orders_date_idx
  ON prefetch_orders(order_date)
  WITH (deduplicate_items=off);

CREATE INDEX prefetch_orders_prod_idx
  ON prefetch_orders(product_id)
  WITH (deduplicate_items=off);

CREATE INDEX prefetch_orders_id_idx
  ON prefetch_orders(order_id)
  WITH (deduplicate_items=off);

-- VACUUM FREEZE ANALYZE
VACUUM FREEZE ANALYZE prefetch_orders;
VACUUM FREEZE ANALYZE prefetch_customers;
VACUUM FREEZE ANALYZE prefetch_products;

-- Adversarial table: sequential heap access
CREATE TABLE prefetch_sequential (
  id bigint,
  val1 int,
  val2 text
);
INSERT INTO prefetch_sequential
SELECT i, i % 1000, 'value_' || i
FROM generate_series(1, 500000) i;
CREATE INDEX prefetch_sequential_idx ON prefetch_sequential(id);
VACUUM ANALYZE prefetch_sequential;

-- Adversarial table: sparse (1 TID per block)
CREATE TABLE prefetch_sparse (
  id bigint,
  category int,
  padding text
);
ALTER TABLE prefetch_sparse ALTER COLUMN padding SET STORAGE plain;
SELECT setseed(0.7890123456789012);
INSERT INTO prefetch_sparse
SELECT i, (i % 50) + 1, repeat('x', 4000)
FROM generate_series(1, 50000) i
ORDER BY random();
CREATE INDEX prefetch_sparse_cat_idx ON prefetch_sparse(category);
VACUUM ANALYZE prefetch_sparse;

CHECKPOINT;
"""


# --- Tmpfs Hugepages Functions ---

TMPFS_SCRIPT = "/home/pg/dotfiles/bin/pg_tmpfs_hugepages.sh"


def setup_tmpfs_hugepages():
    """Mount a tmpfs with huge=always and return the mount point path."""
    result = subprocess.run(
        ["sudo", TMPFS_SCRIPT, "mount", "-s", "500M"],
        capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        print(f"Error: Failed to mount tmpfs: {result.stderr.strip()}")
        sys.exit(1)
    return result.stdout.strip()


def copy_binaries_to_tmpfs(src_bin_dir, tmpfs_mount, version_name):
    """Copy PostgreSQL binaries to tmpfs and return the new path."""
    result = subprocess.run(
        [TMPFS_SCRIPT, "copy", src_bin_dir, version_name],
        capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        print(f"Error: Failed to copy binaries: {result.stderr.strip()}")
        sys.exit(1)
    return result.stdout.strip()


def cleanup_tmpfs(tmpfs_mount):
    """Unmount the tmpfs filesystem."""
    result = subprocess.run(
        ["sudo", TMPFS_SCRIPT, "umount"],
        capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        print(f"Warning: Failed to unmount tmpfs: {result.stderr.strip()}")
    elif result.stderr:
        print(result.stderr.strip())


# --- Cache Management Functions ---

def clear_os_cache():
    """Clear the OS page cache."""
    result = subprocess.run(
        ["sudo", "clear_cache.sh"],
        capture_output=True,
        check=False
    )
    if result.returncode != 0:
        print("Warning: Failed to clear OS cache")


def evict_relations(conn, relations):
    """Evict relations from PostgreSQL buffer cache."""
    max_retries = 3
    with conn.cursor() as cur:
        for rel in relations:
            for attempt in range(max_retries):
                try:
                    cur.execute(f"SELECT * FROM pg_buffercache_evict_relation('{rel}')")
                    row = cur.fetchone()
                    if row:
                        buffers_evicted, buffers_flushed, buffers_skipped = row
                        if buffers_skipped > 0:
                            if attempt < max_retries - 1:
                                time.sleep(0.1)
                                continue  # Retry
                            print(f"Warning: Failed to evict {buffers_skipped} buffers from {rel} "
                                  f"after {max_retries} attempts "
                                  f"(evicted={buffers_evicted}, flushed={buffers_flushed})")
                    break  # Success or no row returned
                except Exception as e:
                    conn.rollback()
                    print(f"Warning: Failed to evict {rel}: {e}")
                    break


def prewarm_relations(conn, relations, include_vm=False, vm_only=False):
    """Prewarm relations into PostgreSQL buffer cache.

    If include_vm is True, also prewarm the visibility map fork.
    If vm_only is True, prewarm ONLY the visibility map fork (not main).
    include_vm should be set for heap relations (tables) but NOT for indexes,
    which have no VM fork.
    """
    with conn.cursor() as cur:
        for rel in relations:
            try:
                if not vm_only:
                    cur.execute(f"SELECT pg_prewarm('{rel}')")
                if include_vm or vm_only:
                    cur.execute(f"SELECT pg_prewarm('{rel}', 'buffer', 'vm')")
            except Exception as e:
                conn.rollback()
                print(f"Warning: Failed to prewarm {rel}: {e}")


def set_gucs(conn, gucs, is_master=False, prefetch_setting=None):
    """Set GUCs for query execution."""
    with conn.cursor() as cur:
        # Base GUCs for all configurations
        cur.execute("SET enable_bitmapscan = off")
        cur.execute("SET enable_seqscan = off")
        cur.execute("SET max_parallel_workers_per_gather = 0")

        # Set prefetch GUC only on patch (not master)
        if not is_master and prefetch_setting is not None:
            cur.execute(f"SET enable_indexscan_prefetch = {prefetch_setting}")

        # Query-specific GUCs
        for guc, value in gucs.items():
            cur.execute(f"SET {guc} = {value}")


def reset_gucs(conn, gucs):
    """Reset query-specific GUCs to defaults."""
    with conn.cursor() as cur:
        for guc in gucs:
            cur.execute(f"RESET {guc}")


# --- Data Verification and Loading ---

def verify_data(conn_details, skip_load=False):
    """
    Verify that tables exist, have data, and are fully all-visible.
    Returns True if data is valid, False if reload is needed.
    """
    if skip_load:
        print("Skipping data verification (--skip-load)")
        return True

    tables = EXPECTED_TABLES + ['prefetch_sequential', 'prefetch_sparse']
    try:
        conn = psycopg.connect(**conn_details)
        conn.autocommit = True
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
                    print(f"Table {table} exists but has 0 rows (data needs to be reloaded)")
                    conn.close()
                    return False
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
        print(f"All tables exist, have data, and are all-visible ✓")
        return True

    except Exception as e:
        print(f"Error verifying data: {e}")
        return False


def load_data(conn_details):
    """Load benchmark data into the database."""
    print("\n" + "=" * 50)
    print("Loading benchmark data...")
    print("This will take several minutes for 50M rows.")
    print("=" * 50 + "\n")

    conn = psycopg.connect(**conn_details)
    conn.autocommit = True

    # Parse SQL into individual statements, handling comments properly
    statements = []
    current_stmt = []
    for line in DATA_LOADING_SQL.split('\n'):
        stripped = line.strip()
        # Skip pure comment lines and empty lines when building statements
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
            # Print progress for long operations BEFORE executing
            if 'INSERT INTO prefetch_orders' in statement and 'generate_series(1, 50000000)' in statement:
                print("Loading 50M orders... (this takes a while)")
            elif 'CREATE INDEX' in statement:
                idx_match = re.search(r'CREATE INDEX (\S+)', statement)
                idx_name = idx_match.group(1) if idx_match else "index"
                print(f"Creating index {idx_name}...")
            elif 'VACUUM' in statement:
                print(f"Running {statement.split()[0]} {statement.split()[1] if len(statement.split()) > 1 else ''}...")

            with conn.cursor() as cur:
                cur.execute(statement)

        except Exception as e:
            print(f"Error executing: {statement[:80]}...")
            print(f"Error: {e}")
            conn.close()
            sys.exit(1)

    conn.close()
    print("Data loading complete.")


def extract_execution_time(explain_output):
    """Extract execution time from EXPLAIN ANALYZE output."""
    for line in explain_output:
        match = re.search(r'Execution Time: ([\d.]+) ms', line[0])
        if match:
            return float(match.group(1))
    return None
