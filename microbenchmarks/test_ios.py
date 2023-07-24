#!/usr/bin/env python3
#
# Taken from https://postgr.es/m/873c33c5-ef9e-41f6-80b2-2f5e11869f1c@garret.ru
import random
import threading
import time
import psycopg2

mydatabase="regression"
myhost="/tmp"

def test_ios():
    con = psycopg2.connect(database=mydatabase, host=myhost)
    n_records = 1000
    n_oltp_writers = 10
    n_oltp_readers = 5
    n_olap_readers = 1

    con.autocommit = True
    cur = con.cursor()

    cur.execute("DROP table if exists t")
    cur.execute("CREATE TABLE t(pk bigint not null) with (autovacuum_enabled=off, autovacuum_analyze_scale_factor=0.99, vacuum_truncate=off)")
    cur.execute(f"insert into t values (generate_series(1,{n_records}))")
    cur.execute("create index on t(pk) with (fillfactor=50)")
    cur.execute("vacuum t")

    running = True

    def oltp_writer():
        con = psycopg2.connect(database=mydatabase, host=myhost)
        con.autocommit = True
        cur = con.cursor()
        while running:
            pk = random.randrange(1, n_records)
            cur.execute(f"update t set pk={n_records-pk+1} where pk={pk}")

    def oltp_reader():
        con = psycopg2.connect(database=mydatabase, host=myhost)
        con.autocommit = True
        cur = con.cursor()
        cur.execute("set effective_io_concurrency=0")
        cur.execute("set max_parallel_workers_per_gather=0")
        cur.execute("set enable_seqscan=off")
        cur.execute("set enable_indexscan=off")
        while running:
            pk = random.randrange(1, n_records)
            cur.execute(f"select count(*) from t where pk between {pk} and {pk+1000}")

    def olap_reader():
        con = psycopg2.connect(database=mydatabase, host=myhost)
        con.autocommit = True
        cur = con.cursor()
        cur.execute("set effective_io_concurrency=0")
        cur.execute("set max_parallel_workers_per_gather=0")
        cur.execute("set enable_seqscan=off")
        cur.execute("set enable_indexscan=off")
        while running:
            cur.execute("select count(*) from t")
            count = cur.fetchall()[0][0]
            assert count == n_records

    oltp_writers = []
    for _ in range(n_oltp_writers):
        t = threading.Thread(target=oltp_writer)
        oltp_writers.append(t)
        t.start()

    oltp_readers = []
    for _ in range(n_oltp_readers):
        t = threading.Thread(target=oltp_reader)
        oltp_readers.append(t)
        t.start()

    olap_readers = []
    for _ in range(n_olap_readers):
        t = threading.Thread(target=olap_reader)
        olap_readers.append(t)
        t.start()

    time.sleep(100)
    running = False

    for t in oltp_writers:
        t.join()
    for t in oltp_readers:
        t.join()
    for t in olap_readers:
        t.join()

if __name__=="__main__":
    test_ios()
