#!/bin/bash

OUTDIR=$1
branch=$2
nrows=$3
IOMETHOD=$4

resultfile="$OUTDIR/$branch"
resultfile+="_$nrows"
resultfile+="_results.csv"

DEBUG="$OUTDIR/debug_${branch}_${nrows}.log"

mkdir $OUTDIR/$nrows

# export PGPORT=5555

PSQL_PAGER=""

function query_duration() {
	scan=$1
	query=$2

	seqscan=off
	bitmapscan=off
	indexscan=off
	indexonlyscan=off

	if [ "$scan" == "seqscan" ]; then
		seqscan=on
	elif [ "$scan" == "bitmapscan" ]; then
		bitmapscan=on
	elif [ "$scan" == "indexscan" ]; then
		indexscan=on
	elif [ "$scan" == "indexonlyscan" ]; then
		indexscan=on
		indexonlyscan=on
	fi

	psql --no-psqlrc $dbname > tmp.log 2>&1 <<EOF
SET enable_seqscan=$seqscan;
SET enable_bitmapscan=$bitmapscan;
SET enable_indexscan=$indexscan;
SET enable_indexonlyscan=$indexonlyscan;
SET max_parallel_workers_per_gather=0;
EXPLAIN SELECT * FROM ($query OFFSET 0) foo OFFSET 1000000000;
SELECT extract(epoch from now()), 'start';
SELECT * FROM ($query OFFSET 0) foo OFFSET 1000000000;
SELECT extract(epoch from now()), 'end';
EOF

	cat tmp.log >> $DEBUG 2>&1

	start=$(grep 'start' tmp.log | awk '{print $1}')
	end=$(grep 'end' tmp.log | awk '{print $1}')

	echo "($end - $start) * 1000" | bc
}

ndistinct=1

# while /bin/true; do
for fill in 100 10; do

	dbname="test_${nrows}_${fill}"
	dropdb --if-exists $dbname
	createdb $dbname

	psql $dbname -c "create extension pg_buffercache"

	for ndistinct_prop in 1 5 100 1000; do

		ndistinct=$(($nrows / $ndistinct_prop))

		if [[ $ndistinct -gt $nrows ]]; then
			echo Failed! >&2
			break
		fi

		for type in int ; do

			# for data in random sequential cycle correlated; do
			for data in random sequential cycle correlated; do

				tablename="t"
				tablename+="$ndistinct_prop"
				tablename+="_"
				tablename+="$type"
				tablename+="_"
				tablename+="$data"

				echo "===== NDISTINCT $ndistinct TYPE $type DATA $data ====="

				#psql $dbname -c "drop table if exists $tablename";

				psql $dbname -c "create unlogged table $tablename (v $type, x text) with (fillfactor = $fill)";

				psql $dbname -c "create index on $tablename (v)";

				if [ "$data" == "random" ]; then
					echo  "insert into $tablename select $ndistinct * random() from generate_series(1, $nrows) s(i)"
					psql $dbname -c "select setseed(0.12345); insert into $tablename select $ndistinct * random(), md5(i::text) from generate_series(1, $nrows) s(i)"
				elif [ "$data" == "sequential" ]; then
					echo "insert into $tablename select i::float * $ndistinct / $nrows from generate_series(1, $nrows) s(i)"
					psql $dbname -c "select setseed(0.12345); insert into $tablename select i::float * $ndistinct / $nrows, md5(i::text) from generate_series(1, $nrows) s(i)"
				elif [ "$data" == "cycle" ]; then
					echo "insert into $tablename select mod(i,$ndistinct) from generate_series(1, $nrows) s(i)"
					psql $dbname -c "select setseed(0.12345); insert into $tablename select mod(i,$ndistinct), md5(i::text) from generate_series(1, $nrows) s(i)"
				elif [ "$data" == "correlated" ]; then
					echo "insert into $tablename select i::float * $ndistinct / $nrows + random() * sqrt($ndistinct) from generate_series(1, $nrows) s(i)"
					psql $dbname -c "select setseed(0.12345); insert into $tablename select i::float * $ndistinct / $nrows + random() * sqrt($ndistinct), md5(i::text) from generate_series(1, $nrows) s(i)"
				fi

				psql $dbname -c "copy $tablename to stdout" | gzip -c -1 > $OUTDIR/$nrows/$tablename.$branch.dump.gz

				psql $dbname -c "vacuum (analyze,freeze) $tablename";
				psql $dbname -c "checkpoint"

				SEED=42
				RANDOM=$SEED

				for run in $(seq 1 10); do

					for scan in indexonlyscan indexscan; do

						for narray_values in 1 10 100 1000; do

							for step in -1 1 5 10; do

								if [[ $narray_values -eq 1 && $step -ne 1 ]]; then
									continue
								fi

								if [ "$step" == "-1" ]; then

									if [[ $narray_values -gt $ndistinct ]]; then
										continue
									fi

									values=$(psql --no-psqlrc $dbname -t -A -c "select string_agg(random(0,${ndistinct})::text,', ') from generate_series(1,${narray_values})")
								else
									range=$((narray_values * step))

									if [[ $range -gt $ndistinct ]]; then
										continue
									fi

									if [[ $ndistinct -gt $range ]]; then
										value=$((RANDOM % (ndistinct - range)))
									else
										value=0
									fi

									values="$value"

									for v in $(seq 1 $((narray_values-1))); do
										value=$((value + step))
										values="$values, $value"
									done
								fi

								if [ "$scan" == "indexscan" ]; then
									query="select * from $tablename where v = any('{$values}')"
								else
									query="select v from $tablename where v = any('{$values}')"
								fi

								echo "----- narray_values $narray_values step $step run $run query $query scan $scan -----"

								psql $dbname -c "select pg_buffercache_evict_all()"
								sudo ./drop-caches.sh

								echo "===== $IOMETHOD $branch $nrows $fill $ndistinct $type $data $narray_values $step $run $scan =====" >> $DEBUG 2>&1

								echo $sql >> $DEBUG 2>&1

								t=$(query_duration $scan "$query")
								echo "$IOMETHOD $branch $nrows $fill $ndistinct $type $data $narray_values $step $run cold $scan $t" >> "$resultfile"

								t=$(query_duration $scan "$query")
								echo "$IOMETHOD $branch $nrows $fill $ndistinct $type $data $narray_values $step $run hot $scan $t" >> "$resultfile"

							done

						done

					done

				done

			done

		done

	done

done
