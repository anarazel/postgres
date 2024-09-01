SELECT count(*) FROM tbl_a WHERE ctid = '(1, 1)';

SELECT corrupt_rel_block('tbl_a', 1);

-- FIXME: Should report the error
SELECT redact($$
  SELECT read_corrupt_rel_block('tbl_a', 1);
$$);

-- verify the error is reported
SELECT redact($$
  SELECT count(*) FROM tbl_a WHERE ctid = '(1, 1)';
$$);
SELECT redact($$
  SELECT count(*) FROM tbl_a;
$$);
