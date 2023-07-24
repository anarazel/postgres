#!/bin/zsh

PSQL_PAGER= p -f microbenchmarks/bugs_parallel_load.sql

# for ((;;)) PSQL_PAGER= p -f microbenchmarks/bugs_parallel_run.sql && sleep 0.1

for ((;;)) PSQL_PAGER= p -f microbenchmarks/skipscan_parallel_index_scan_testcase_medium.sql && sleep 0.1
