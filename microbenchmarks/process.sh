#!/usr/bin/bash

DIR=$1

rm -f results.db

sqlite3 results.db "create table results (iomethod text, branch text, nrows int, fill int, ndistinct int, type text, data text, narray_values int, step int, run int, cache text, scan text, timing numeric)"

sqlite3 results.db <<EOF
.mode csv
.separator ' '
.import $DIR/master_25000000_results.csv results
.import $DIR/master_10000000_results.csv results
.import $DIR/master_1000000_results.csv results
.import $DIR/master_100000_results.csv results
.import $DIR/patched_25000000_results.csv results
.import $DIR/patched_10000000_results.csv results
.import $DIR/patched_1000000_results.csv results
.import $DIR/patched_100000_results.csv results
EOF

sqlite3 results.db "create table results_aggregated (iomethod text, branch text, nrows int, fill int, ndistinct int, type text, data text, narray_values int, step int, cache text, scan text, timing numeric)"

sqlite3 results.db <<EOF
insert into results_aggregated select iomethod, branch, nrows, fill, ndistinct, type, data, narray_values, step, cache, scan, avg(timing) from results group by iomethod, branch, nrows, fill, ndistinct, type, data, narray_values, step, cache, scan;
EOF

sqlite3 results.db <<EOF
.mode table
select
  scan, data, nrows, fill, ndistinct, type, iomethod, narray_values, step,
  m_cold.timing AS cold_master, p_cold.timing AS cold_patched, cast((p_cold.timing * 100.0 / m_cold.timing - 100) as int) AS cold_diff,
  m_hot.timing AS hot_master, p_hot.timing AS hot_patched, cast((p_hot.timing * 100.0 / m_hot.timing - 100) as int) AS hot_diff
  from results_aggregated m_hot
     join results_aggregated m_cold using (iomethod, nrows, fill, ndistinct, type, data, narray_values, step, scan)
     join results_aggregated p_hot using (iomethod, nrows, fill, ndistinct, type, data, narray_values, step, scan)
     join results_aggregated p_cold using (iomethod, nrows, fill, ndistinct, type, data, narray_values, step, scan)
  where
     m_hot.branch = 'master' and m_hot.cache = 'hot' and
     m_cold.branch = 'master' and m_cold.cache = 'cold' and
     p_hot.branch = 'patched' and p_hot.cache = 'hot' and
     p_cold.branch = 'patched' and p_cold.cache = 'cold'
  order by scan, data, nrows, fill, ndistinct, type, iomethod, narray_values, step;
EOF
