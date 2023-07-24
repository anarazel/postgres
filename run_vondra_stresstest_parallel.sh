#!/usr/bin/bash -x
#
# Taken from: https://postgr.es/m/b05aa7ac-ae3a-49e1-a9da-22594bed3379@vondra.me

# exit on error
set -e

BINDIR=/home/user/builds/master/bin
DATADIR=/home/user/tmp/data
PSQL="$BINDIR/psql --no-psqlrc"

# number of rows to generate
ROWS="1000000 10000000"

# number of distinct values
DISTINCT="100 1000 10000 100000 1000000 10000000"

# how skewed should the dataset be?
# 1 means no skew (uniform distribution), >1 means skew
SKEW="1 2 4 8"

# number of random queries to run for each combination of parameters
QUERIES=10

# number of runs for each query
RUNS=3

# number of values in the IN() clause
VALUES="1 2 4 8 16 32 64 128 256 512 1024"

ts=$(date +%s)
RESULTS=results-$ts.csv
RESULTDIR=results-$ts
mkdir $RESULTDIR

# simple deterministic pseudorandom generator - seed and max are enough
# to fully determine the result for a particular value
function prng() {
	seed=$1
	max=$2
	value=$3

	echo $(((seed + 786433 * value) % max))
}

SEQ=0

for r in $ROWS; do

	for d in $DISTINCT; do

		# too many distinct values (more than rows)
		if [[ $d -gt $r ]]; then
			continue
		fi

		for s in $SKEW; do

			echo "========== rows $r distinct $d skew $s ==========" >> debug.log 2>&1

			table="t_$r_$d_$s"

			c=$($PSQL test -t -A -c "select count(*) from pg_class where relname = '$table'")

			if [ "$c" == "0" ]; then
				$PSQL test -c "create table $table (id bigint, val text)" >> debug.log 2>&1
				$PSQL test -c "insert into $table select pow(random(), $s) * $d, md5(i::text) from generate_series(1,$r) s(i)" >> debug.log 2>&1
				$PSQL test -c "create index on $table (id)" >> debug.log 2>&1
				$PSQL test -c "vacuum analyze" >> debug.log 2>&1
				$PSQL test -c "checkpoint" >> debug.log 2>&1
			fi

			for v in $VALUES; do

				# too many values (more than distinct values)
				if [[ $v -gt $d ]]; then
					continue
				fi

				for q in $(seq 1 $QUERIES); do


					# max parallel workers
					for w in 0 4; do

						# seed for this particular combination of parameters
						seed=$(((389 * r) + (24593 * d) + (s * 769) + (v * 3079) + (q * 98317) + (w * 1543)))

						# generate the right number of random values for the IN clause
						vals=""
						for i in $(seq 1 $v); do

							if [ "$vals" != "" ]; then
								vals="$vals,"
							fi

							x=$(prng $seed $d $i)

							vals="$vals $x"
						done

						for z in $(seq 1 $RUNS); do

							# index scan
							sql="select * from $table where id in ($vals)"

							$PSQL test > explain.log <<EOF
-- to make parallel queries likely (if enabled)
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
-- force index scan
SET max_parallel_workers_per_gather = $w;
SET enable_bitmapscan = off;
SET enable_seqscan = off;
explain $sql;
EOF

							# count different plan types
							is=$(grep 'Index Scan' explain.log | wc -l)
							ios=$(grep 'Index Only Scan' explain.log | wc -l)
							gather=$(grep 'Gather' explain.log | wc -l)

							echo "===== $sql run $r index-scan =====" >> explains.log 2>&1
							cat explain.log  >> explains.log 2>&1

							# hash of results

							$PSQL -t -A test > output <<EOF
-- to make parallel queries likely (if enabled)
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
-- force index scan
SET max_parallel_workers_per_gather = $w;
SET enable_bitmapscan = off;
SET enable_seqscan = off;
$sql;
EOF
							# the parallel index scan does not maintain the index order, so sort explicitly
							if [ "$w" != "0" ]; then
								h=$(md5sum output | sort | awk '{print $1}')
							else
								h=$(md5sum output | awk '{print $1}')
							fi

							# uncached

							$BINDIR/pg_ctl -D $DATADIR -l pg.log restart >> debug.log 2>&1
							# sudo ./drop-caches.sh >> debug.log 2>&1

							$PSQL test > explain-analyze.log <<EOF
\timing on
-- to make parallel queries likely (if enabled)
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
-- force index scan
SET max_parallel_workers_per_gather = $w;
SET enable_bitmapscan = off;
SET enable_seqscan = off;
explain (analyze, timing off) $sql;
EOF

							workers=$(grep 'Workers Launched' explain-analyze.log | awk '{print $3}')
							if [ "$workers" == "" ]; then workers="0"; fi

							echo "===== $sql run $r index-scan not-ncached =====" >> explains-analyze.log 2>&1
							cat explain-analyze.log  >> explains-analyze.log 2>&1

							t=$(grep '^Time:' explain-analyze.log | awk '{print $2}' | tail -n 1)

							SEQ=$((SEQ+1))
							echo $SEQ $(date +%s) $r $d $s $v $q $seed $w $z index-scan not-cached $h $t $is $ios $gather $workers "$sql" >> $RESULTS
							cp output $RESULTDIR/$SEQ

							# cached

							$PSQL test > explain-analyze.log <<EOF
\timing on
-- to make parallel queries likely (if enabled)
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
-- force index scan
SET max_parallel_workers_per_gather = $w;
SET enable_bitmapscan = off;
SET enable_seqscan = off;
explain (analyze, timing off) $sql;
EOF

							workers=$(grep 'Workers Launched' explain-analyze.log | awk '{print $3}')
							if [ "$workers" == "" ]; then workers="0"; fi

							echo "===== $sql run $r index-scan cached =====" >> explains-analyze.log 2>&1
							cat explain-analyze.log  >> explains-analyze.log 2>&1

							t=$(grep '^Time:' explain-analyze.log | awk '{print $2}' | tail -n 1)

							SEQ=$((SEQ+1))
							echo $SEQ $(date +%s) $r $d $s $v $q $seed $w $z index-scan cached $h $t $is $ios $gather $workers "$sql" >> $RESULTS
							cp output $RESULTDIR/$SEQ

							# index-only scan

							sql="select id from $table where id in ($vals)"

							$PSQL test > explain.log <<EOF
-- to make parallel queries likely (if enabled)
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
-- force index scan
SET max_parallel_workers_per_gather = $w;
SET enable_bitmapscan = off;
SET enable_seqscan = off;
explain $sql;
EOF

							# count different plan types
							is=$(grep 'Index Scan' explain.log | wc -l)
							ios=$(grep 'Index Only Scan' explain.log | wc -l)
							gather=$(grep 'Gather' explain.log | wc -l)

							# hash of results

							$PSQL -t -A test > output <<EOF
-- to make parallel queries likely (if enabled)
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
-- force index scan
SET max_parallel_workers_per_gather = $w;
SET enable_bitmapscan = off;
SET enable_seqscan = off;
$sql;
EOF

							# the parallel index scan does not maintain the index order, so sort explicitly
							if [ "$w" != "0" ]; then
								h=$(md5sum output | sort | awk '{print $1}')
							else
								h=$(md5sum output | awk '{print $1}')
							fi

							# in non-parallel case we can check the ordering
							if [ "$w" == "0" ]; then

								# hash with SET filtered-out
								h1=$(grep -v SET output | md5sum | awk '{print $1}')

								# hash with SET filtered-out, and sorted
								h2=$(grep -v SET output | sort -n | md5sum | awk '{print $1}')

								echo "index-only scan h1 $h1 h2 $h2 $sql"

								if [ "$h1" != "$h2" ]; then
									echo "ERROR: $sql"
									echo "h1 $h1 h2 $h2"
									exit 1
								fi

							fi

							echo "===== $sql run $r index-only-scan =====" >> explains.log 2>&1
							cat explain.log  >> explains.log 2>&1

							# uncached

							$BINDIR/pg_ctl -D $DATADIR -l pg.log restart >> debug.log 2>&1
							# sudo ./drop-caches.sh >> debug.log 2>&1

							$PSQL test > explain-analyze.log <<EOF
\timing on
-- to make parallel queries likely (if enabled)
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
-- force index scan
SET max_parallel_workers_per_gather = $w;
SET enable_bitmapscan = off;
SET enable_seqscan = off;
explain (analyze, timing off) $sql;
EOF

							workers=$(grep 'Workers Launched' explain-analyze.log | awk '{print $3}')
							if [ "$workers" == "" ]; then workers="0"; fi

							echo "===== $sql run $r index-only-scan not-cached =====" >> explains-analyze.log 2>&1
							cat explain-analyze.log  >> explains-analyze.log 2>&1

							t=$(grep '^Time:' explain-analyze.log | awk '{print $2}' | tail -n 1)

							SEQ=$((SEQ+1))
							echo $SEQ $(date +%s) $r $d $s $v $q $seed $w $z index-only-scan not-cached $h $t $is $ios $gather $workers "$sql" >> $RESULTS
							cp output $RESULTDIR/$SEQ

							# cached

							$BINDIR/pg_ctl -D $DATADIR -l pg.log restart >> debug.log 2>&1
							# sudo ./drop-caches.sh >> debug.log 2>&1

							$PSQL test > explain-analyze.log <<EOF
\timing on
-- to make parallel queries likely (if enabled)
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
-- force index scan
SET max_parallel_workers_per_gather = $w;
SET enable_bitmapscan = off;
SET enable_seqscan = off;
explain (analyze, timing off) $sql;
EOF

							workers=$(grep 'Workers Launched' explain-analyze.log | awk '{print $3}')
							if [ "$workers" == "" ]; then workers="0"; fi

							echo "===== $sql run $r index-only-scan cached =====" >> explains-analyze.log 2>&1
							cat explain-analyze.log  >> explains-analyze.log 2>&1

							t=$(grep '^Time:' explain-analyze.log | awk '{print $2}' | tail -n 1)

							SEQ=$((SEQ+1))
							echo $SEQ $(date +%s) $r $d $s $v $q $seed $w $z index-only-scan cached $h $t $is $ios $gather $workers "$sql" >> $RESULTS
							cp output $RESULTDIR/$SEQ

						done

					done

				done

			done

		done

	done

done
