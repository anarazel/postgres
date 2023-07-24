#!/usr/bin/env bash

set -e

PATH_OLD=$PATH
DATADIR=$1
BUILDDIR=$2

if [ "$DATADIR" == "" ]; then
	echo "DATADIR not specified"
	exit 1
fi

BUILDDIR=$2
BUILDS="master complex"

# check that all builds exist
for b in $BUILDS; do
	if [ ! -f "$BUILDDIR/$b/bin/postgres" ]; then
		echo "$BUILDDIR/$b/bin/postgres does not exist"
		exit 1
	fi
done

ROWS=10000000
STEPS=10

TS=$(date +%Y%m%d-%H%M%S)

killall -9 postgres || true
sleep 1

rm -Rf $DATADIR

# we need 'master' to initialize instance
PATH=$BUILDS/master/bin:$PATH_OLD

pg_ctl -D $DATADIR init
pg_checksums --disable $DATADIR
pg_ctl -D $DATADIR start
createdb test
pg_ctl -D $DATADIR stop

cp $DATADIR/postgresql.conf ./

echo 'shared_buffers = 16GB' >> postgresql.conf

mkdir $TS
mkdir $TS/samples

for step in 32 1 16 2 8 4; do

	fuzz=-1;

	while [[ "$fuzz" -le "$ROWS" ]]; do

		for fuzz2 in 0 2 8 32 128 512 2048; do
			echo "$step $fuzz $fuzz2" >> $TS/parameters.list
		done

		if [ "$fuzz" == "-1" ]; then
			fuzz=0
		elif [ "$fuzz" == "0" ]; then
			fuzz=1
		else
			fuzz=$((fuzz*4))
		fi

	done

done

sort -R $TS/parameters.list > $TS/parameters.random

echo did qid fillfactor ROWS distinct relpages fuzz fuzz2 step iomethod ioworkers eic direction run iorder order b values start end time_uncached time_cached distance_uncached distance_cached buffers_read buffers_hit >> $TS/results.csv

c=0
qid=0
did=0

fillfactor=20

while IFS= read -r line; do

	IFS=', ' read -r -a strarray <<< "$line"

	step="${strarray[0]}"
	fuzz="${strarray[1]}"
	fuzz2="${strarray[2]}"

	for direction in increase decrease; do

		coeff="1"
		if [ "$direction" == "decrease" ]; then
			coeff="-1"
		fi

		for iorder in asc desc; do

			io=""
			if [ "$iorder" == "asc" ]; then
				io="ASC"
			elif [ "$iorder" == "desc" ]; then
				io="DESC"
			fi

			echo $fillfactor $fuzz $fuzz2 $iorder

			PATH=$BUILDDIR/master/bin:$PATH_OLD

			pg_ctl -D $DATADIR -l $TS/pg.log start

			distinct=$((ROWS/step))

			psql test -c "drop table if exists t"
			psql test -c "create unlogged table t (a bigint, b text) with (fillfactor = $fillfactor)"

			if [ "$fuzz" == "-1" ]; then
				insert="insert into t select $coeff * a, b from (select r, a, b, generate_series(0,$step-1) AS p from (select row_number() over () as r, a, b from (select i AS a, md5(i::text) AS b from generate_series(1, $distinct) s(i) ORDER BY random()) foo) bar) baz ORDER BY ((r * $step + p) + $fuzz2 * (random() - 0.5))"
			else
				insert="insert into t select $coeff * a, b from (select r, a, b, generate_series(0,$step-1) AS p from (select row_number() over () AS r, a, b from (select i AS a, md5(i::text) AS b from generate_series(1, $distinct) s(i) ORDER BY (i + $fuzz * (random() - 0.5))) foo) bar) baz ORDER BY ((r * $step + p) + $fuzz2 * (random() - 0.5))"
			fi

			did=$((did+1))

			echo "$did : $insert" >> $TS/datasets.log 2>&1
			echo "$did : create index idx on t(a $io)" >> $TS/indexes.log 2>&1

			psql test -c "$insert"

			psql test -c "create index idx on t(a $io)"
			psql test -c "vacuum analyze t"

			relpages=$(psql -t -A test -c "select relpages from pg_class where relname = 't'")

			psql test -c "select * from t limit 10000" > $TS/samples/$ROWS-$step-$fuzz-$fuzz2.data 2>&1

			pg_ctl -D $DATADIR -l $TS/pg.log stop

			for iomethod in worker; do

				for ioworkers in 12; do

					for eic in 16; do

						for order in asc desc; do

							o=""
							if [ "$order" == "asc" ]; then
								o="ORDER BY a ASC"
							elif [ "$order" == "desc" ]; then
								o="ORDER BY a DESC"
							fi

							for run in $(seq 1 3); do

								values=1
								while /bin/true; do

									if [ "$values" -ge "$distinct" ]; then
										break;
									fi

									for branch in $BUILDS; do

										qid=$((qid+1))

										PATH=$BUILDDIR/$branch/bin:$PATH_OLD

										n=$((distinct - values - 1))
										start=$((RANDOM % n))
										end=$((start + values - 1))

										# flip the range, add minus
										if [ "$direction" == "decrease" ]; then
											end_new="-$start"
											start_new="-$end"

											start=$start_new
											end=$end_new
										fi

										sudo ./drop-caches.sh

										echo "$qid : SELECT * FROM t WHERE a BETWEEN $start AND $end $o" >> $TS/queries.log 2>&1

										pg_ctl -D $DATADIR -l $TS/pg.log start

										echo "=========== DATASET: $did QUERY: $qid fillfactor: $fillfactor rows: $rows fuzz: $fuzz fuzz2: $fuzz2 step: $step iomethod: $iomethod ioworkers: $ioworkers eic: $eic iorder: $iorder order: $order build: $branch values: $values start: $start end: $end ===========" >> $TS/explains.log

										psql test > tmp.log 2>&1 <<EOF
SET enable_bitmapscan = off;
SET enable_seqscan = off;
SET max_parallel_workers_per_gather = 0;
SET effective_io_concurrency = $eic;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, VERBOSE, SETTINGS) SELECT * FROM t WHERE a BETWEEN $start AND $end $o;
EOF

										cat tmp.log >> $TS/explains.log

										time_uncached=$(grep '^ Execution Time:' tmp.log | awk '{print $3}')
										distance_uncached=$(grep 'Prefetch Distance' tmp.log | awk '{print $3}')

										buffers_hit=$(grep 'Buffers: shared' tmp.log | head -n 1 | awk '{print $3}' | sed 's/hit=//')
										buffers_read=$(grep 'Buffers: shared' tmp.log | head -n 1 | awk '{print $4}' | sed 's/read=//')

										if [ "$distance_uncached" == "" ]; then
											distance_uncached="-1"
										fi

										psql test > tmp.log 2>&1 <<EOF
SET enable_bitmapscan = off;
SET enable_seqscan = off;
SET max_parallel_workers_per_gather = 0;
SET effective_io_concurrency = $eic;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, VERBOSE, SETTINGS) SELECT * FROM t WHERE a BETWEEN $start AND $end $o;
EOF

										cat tmp.log >> $TS/explains.log

										time_cached=$(grep '^ Execution Time:' tmp.log | awk '{print $3}')
										distance_cached=$(grep 'Prefetch Distance' tmp.log | awk '{print $3}')

										if [ "$distance_cached" == "" ]; then
											distance_cached="-1"
										fi

										pg_ctl -D $DATADIR -l $TS/pg.log stop

										echo $did $qid $fillfactor $ROWS $distinct $relpages $fuzz $fuzz2 $step $iomethod $ioworkers $eic $direction $run $iorder $order $branch $values $start $end $time_uncached $time_cached $distance_uncached $distance_cached $buffers_read $buffers_hit >> $TS/results.csv

									done

									m=$((distinct/10))
									if [ "$values" -ge "$m" ]; then
										values=$((values+m))
									else
										values=$((values*2))
									fi

								done

							done

						done

					done

				done

			done

		done

	done

done < $TS/parameters.random
