SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)';

-- injected what we'd expect
SELECT inj_io_short_read_attach(8192);
SELECT invalidate_rel_block('tbl_b', 2);
SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)';
SELECT inj_io_short_read_detach();

-- injected a read shorter than a single block, expecting error
SELECT inj_io_short_read_attach(17);
SELECT invalidate_rel_block('tbl_b', 2);
SELECT redact($$
  SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)';
$$);
SELECT inj_io_short_read_detach();

-- shorten multi-block read to a single block, should retry, but that's not
-- implemented yet
SELECT inj_io_short_read_attach(8192);
SELECT invalidate_rel_block('tbl_b', 0);
SELECT invalidate_rel_block('tbl_b', 1);
SELECT invalidate_rel_block('tbl_b', 2);
SELECT redact($$
  SELECT count(*) FROM tbl_b;
$$);
SELECT inj_io_short_read_detach();

-- verify that checksum errors are detected even as part of a shortened
-- multi-block read
-- (tbl_a, block 1 is corrupted)
SELECT redact($$
  SELECT count(*) FROM tbl_a WHERE ctid < '(2, 1)';
$$);
SELECT inj_io_short_read_attach(8192);
SELECT invalidate_rel_block('tbl_a', 0);
SELECT invalidate_rel_block('tbl_a', 1);
SELECT invalidate_rel_block('tbl_a', 2);
SELECT redact($$
  SELECT count(*) FROM tbl_a WHERE ctid < '(2, 1)';
$$);
SELECT inj_io_short_read_detach();


-- FIXME: Should error
-- FIXME: errno encoding?
SELECT inj_io_short_read_attach(-5);
SELECT invalidate_rel_block('tbl_b', 2);
SELECT redact($$
  SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)';
$$);
SELECT inj_io_short_read_detach();
