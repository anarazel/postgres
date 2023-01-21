setup {
  CREATE EXTENSION postgres_fdw;

  DO $d$
      BEGIN
          EXECUTE $$CREATE SERVER loopback FOREIGN DATA WRAPPER postgres_fdw
              OPTIONS (dbname '$$||current_database()||$$',
                       port '$$||current_setting('port')||$$'
              )$$;
      END;
  $d$;

  CREATE USER MAPPING FOR CURRENT_USER SERVER loopback;

  CREATE TABLE local_t1 (id int);
  CREATE FOREIGN TABLE remote_t1 (id int) SERVER loopback OPTIONS (table_name 'local_t1');

  -- wait until postgres_fdw is waiting, isolation tester can't see that wait edge
  CREATE FUNCTION wait_for_s1() RETURNS void LANGUAGE plpgsql AS $f$
    BEGIN
      WHILE NOT EXISTS(
      SELECT * FROM pg_stat_activity WHERE datname = current_database() AND application_name = 'isolation/interrupt/s1' AND wait_event = 'Extension')
      LOOP
      -- don't use too much CPU
      PERFORM pg_sleep(0.05);
      END LOOP;
    END;
  $f$
}

teardown {
    DROP EXTENSION postgres_fdw CASCADE;
    DROP TABLE local_t1;
    DROP FUNCTION wait_for_s1();
}

session "s1"
step s1_select_remote {SELECT count(*) FROM remote_t1;}

session "s2"
step s2_begin { BEGIN; }
step s2_hang_logins { LOCK pg_db_role_setting; }
step s2_hang_select { LOCK local_t1; }
step s2_commit { COMMIT; }

session "s3"
step s3_cancel_s1 {
  SELECT wait_for_s1();
  SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE datname = current_database() AND application_name = 'isolation/interrupt/s1';
}

step s3_terminate_s1 {
  SELECT wait_for_s1();
  SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = current_database() AND application_name = 'isolation/interrupt/s1';
}

# test that postgres_fdw can be interrupted during connection establishment
permutation s2_begin s2_hang_logins s1_select_remote(*) s3_cancel_s1 s2_commit

# for completeness: verify that committing allows to continue too
permutation s2_begin s2_hang_logins s1_select_remote(*) s2_commit

# test that statements can be interrupted
permutation s2_begin s2_hang_select s1_select_remote(*) s3_cancel_s1 s2_commit
permutation s2_begin s2_hang_select s1_select_remote(*) s2_commit

# test that terminations work - needs to be last, as the connection is gone afterwards
permutation s2_begin s2_hang_logins s1_select_remote(*) s3_terminate_s1 s2_commit
