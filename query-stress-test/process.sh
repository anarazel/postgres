#!/usr/bin/env bash

DIR=$1

if [ ! -f "$DIR/results.csv" ]; then
	echo "$DIR/results.csv does not exist"
	exit 1
fi

rm -f results.db

sqlite3 results.db <<EOF
CREATE TABLE results (did INT, qid INT, fillfactor INT, rows int, ndistinct int, relpages int, fuzz int, fuzz2 int, step int, iomethod text, ioworkers int, eic int, direction text, run int, index_order text, scan_order text, branch text, num_values int, start int, end int, time_uncached numeric, time_cached numeric, distance_uncached numeric, distance_cached numeric, buffers_read int, buffers_hit int);
CREATE INDEX results_idx ON results(did, qid, fillfactor, rows, ndistinct, relpages, fuzz, fuzz2, step, iomethod, ioworkers, eic, direction, run, index_order, scan_order, branch, num_values);
.mode csv
.separator ' '
.import --skip 1 $DIR/results.csv results
EOF

sqlite3 results.db <<EOF
CREATE VIEW results_agg AS
SELECT
    fillfactor, rows, ndistinct, relpages, fuzz, fuzz2, step, iomethod, ioworkers, eic, direction, index_order, scan_order, branch, num_values,
    COUNT(*) AS cnt,
    did,
    group_concat(qid, ',') AS qid,
    AVG(time_uncached) AS avg_time
FROM results
GROUP BY did, fillfactor, rows, ndistinct, relpages, fuzz, fuzz2, step, iomethod, ioworkers, eic, direction, index_order, scan_order, branch, num_values;

CREATE VIEW branches AS
SELECT
    fillfactor, rows, ndistinct, relpages, fuzz, fuzz2, step, iomethod, ioworkers, eic, branch, num_values,
    SUM(cnt) AS cnt,
    did,
    group_concat(qid, ',') AS query_ids,
    MIN(avg_time) AS min_time,
    MAX(avg_time) AS max_time,
    AVG(avg_time) AS avg_time
FROM results_agg
GROUP BY did, fillfactor, rows, ndistinct, relpages, fuzz, fuzz2, step, iomethod, ioworkers, eic, branch, num_values;

CREATE VIEW directions AS
SELECT
    fillfactor, rows, ndistinct, relpages, fuzz, fuzz2, step, iomethod, ioworkers, eic, direction, index_order, scan_order, num_values,
    SUM(cnt) AS cnt,
    did,
    group_concat(qid, ',') AS query_ids,
    MIN(avg_time) AS min_time,
    MAX(avg_time) AS max_time,
    AVG(avg_time) AS avg_time
FROM results_agg
GROUP BY did, fillfactor, rows, ndistinct, relpages, fuzz, fuzz2, step, iomethod, ioworkers, eic, direction, index_order, scan_order, num_values;

CREATE VIEW regressions AS
SELECT
    fillfactor, rows, ndistinct, relpages, fuzz, fuzz2, step, iomethod, ioworkers, eic, direction, index_order, scan_order, num_values,
    round(m.avg_time,2) AS master,
    round(c.avg_time,2) AS complex,
    (m.cnt + c.cnt) AS cnt,
    did,
    concat(m.qid, ',', c.qid) AS qid
FROM results_agg m
JOIN results_agg c USING (did, fillfactor, rows, ndistinct, relpages, fuzz, fuzz2, step, iomethod, ioworkers, eic, direction, index_order, scan_order, num_values)
WHERE m.branch = 'master'
  AND c.branch = 'complex';
EOF

# sqlite3 results.db > branches.txt <<EOF
# .mode table
# SELECT *, round(100 * ((max_time - min_time) / avg_time), 2) AS pct_diff FROM branches ORDER BY (max_time - min_time) / avg_time DESC
# EOF
# 
# sqlite3 results.db > directions.txt <<EOF
# .mode table
# SELECT *, round(100 * ((max_time - min_time) / avg_time), 2) AS pct_diff FROM directions ORDER BY (max_time - min_time) / avg_time DESC
# EOF

sqlite3 results.db > regressions.txt <<EOF
.mode table
SELECT *, round(100 * ((complex - master) / master), 2) AS pct_diff FROM regressions ORDER BY (complex - master) / master DESC
EOF

# sqlite3 results.db <<EOF
# .mode table
# SELECT * FROM results WHERE qid IN (12784,12856,12928,12785,12857,12929,12786,12858,12930);
# EOF
