create extension if not exists pg_prewarm;
create extension if not exists pg_buffercache;
set enable_bitmapscan=off;
set enable_seqscan=off;
set max_parallel_workers_per_gather=0;
-- set enable_indexscan_prefetch = off;

-- t table (original/sequential)
select pg_buffercache_evict_relation('t');
select pg_prewarm('t_pk');
\echo '#### forwards sequential table ####'
\echo '#### set enable_indexscan_prefetch = on ####'
set enable_indexscan_prefetch = on;
\! sudo clear_cache.sh
explain (analyze, costs off, timing off)
select * from t
where a between 16336 and 49103
order by a;

select pg_buffercache_evict_relation('t');
select pg_prewarm('t_pk');
\echo '#### forwards sequential table ####'
\echo '#### set enable_indexscan_prefetch = off ####'
set enable_indexscan_prefetch = off;
\! sudo clear_cache.sh
explain (analyze, costs off, timing off)
select * from t
where a between 16336 and 49103
order by a;

select pg_buffercache_evict_relation('t');
select pg_prewarm('t_pk');
\echo '#### backwards sequential table ####'
\echo '#### set enable_indexscan_prefetch = on ####'
set enable_indexscan_prefetch = on;
\! sudo clear_cache.sh
explain (analyze, costs off, timing off)
select * from t
where a between 16336 and 49103
order by a desc;

select pg_buffercache_evict_relation('t');
select pg_prewarm('t_pk');
\echo '#### backwards sequential table ####'
\echo '#### set enable_indexscan_prefetch = off ####'
set enable_indexscan_prefetch = off;
\! sudo clear_cache.sh
explain (analyze, costs off, timing off)
select * from t
where a between 16336 and 49103
order by a desc;

-- t_randomized variant table (should be slower than before, but is actually
-- faster, at least with backwards scan case)
select pg_buffercache_evict_relation('t_randomized');
select pg_prewarm('t_randomized_pk');
\echo '#### forwards random table ####'
\echo '#### set enable_indexscan_prefetch = on ####'
set enable_indexscan_prefetch = on;
\! sudo clear_cache.sh
explain (analyze, costs off, timing off)
select * from t_randomized
where a between 16336 and 49103
order by a;

select pg_buffercache_evict_relation('t_randomized');
select pg_prewarm('t_randomized_pk');
\echo '#### forwards random table ####'
\echo '#### set enable_indexscan_prefetch = off ####'
set enable_indexscan_prefetch = off;
\! sudo clear_cache.sh
explain (analyze, costs off, timing off)
select * from t_randomized
where a between 16336 and 49103
order by a;

select pg_buffercache_evict_relation('t_randomized');
select pg_prewarm('t_randomized_pk');
\echo '#### backwards random table ####'
\echo '#### set enable_indexscan_prefetch = on ####'
set enable_indexscan_prefetch = on;
\! sudo clear_cache.sh
explain (analyze, costs off, timing off)
select * from t_randomized
where a between 16336 and 49103
order by a desc;

select pg_buffercache_evict_relation('t_randomized');
select pg_prewarm('t_randomized_pk');
\echo '#### backwards random table ####'
\echo '#### set enable_indexscan_prefetch = off ####'
set enable_indexscan_prefetch = off;
\! sudo clear_cache.sh
explain (analyze, costs off, timing off)
select * from t_randomized
where a between 16336 and 49103
order by a desc;
