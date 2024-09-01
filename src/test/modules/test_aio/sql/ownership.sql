-----
-- IO handles
----

-- leak warning: implicit xact
SELECT handle_get();

-- leak warning: explicit xact
BEGIN; SELECT handle_get(); COMMIT;

-- leak warning + error: released in different command (thus resowner)
BEGIN; SELECT handle_get(); SELECT handle_release_last(); COMMIT;

-- no leak, same command
BEGIN; SELECT handle_get() UNION ALL SELECT handle_release_last(); COMMIT;

-- leak warning: subtrans
BEGIN; SAVEPOINT foo; SELECT handle_get(); COMMIT;

-- normal handle use
SELECT handle_get_release();

-- should error out, API violation
SELECT handle_get_twice();

-- recover after error in implicit xact
SELECT handle_get_and_error(); SELECT handle_get_release();

-- recover after error in explicit xact
BEGIN; SELECT handle_get_and_error(); ROLLBACK; SELECT handle_get_release();

-- recover after error in subtrans
BEGIN; SAVEPOINT foo; SELECT handle_get_and_error(); ROLLBACK TO SAVEPOINT foo; SELECT handle_get_release(); ROLLBACK;


-----
-- Bounce Buffers handles
----

-- leak warning: implicit xact
SELECT bb_get();

-- leak warning: explicit xact
BEGIN; SELECT bb_get(); COMMIT;

-- missing leak warning: we should warn at command boundaries, not xact boundaries
BEGIN; SELECT bb_get(); SELECT bb_release_last(); COMMIT;

-- leak warning: subtrans
BEGIN; SAVEPOINT foo; SELECT bb_get(); COMMIT;

-- normal bb use
SELECT bb_get_release();

-- should error out, API violation
SELECT bb_get_twice();

-- recover after error in implicit xact
SELECT bb_get_and_error(); SELECT bb_get_release();

-- recover after error in explicit xact
BEGIN; SELECT bb_get_and_error(); ROLLBACK; SELECT bb_get_release();

-- recover after error in subtrans
BEGIN; SAVEPOINT foo; SELECT bb_get_and_error(); ROLLBACK TO SAVEPOINT foo; SELECT bb_get_release(); ROLLBACK;
