# RI Trigger test
#
# Test trigger-based referential integrity enforcement.
#
# Any overlap between the transactions must cause a serialization failure.

setup
{
 CREATE TABLE rt_parent (parent_id SERIAL NOT NULL PRIMARY KEY);
 CREATE TABLE rt_child (child_id SERIAL NOT NULL PRIMARY KEY, parent_id INTEGER NOT NULL);
 CREATE FUNCTION ri_parent() RETURNS TRIGGER LANGUAGE PLPGSQL AS $body$
 BEGIN
  PERFORM TRUE FROM rt_child WHERE parent_id = OLD.parent_id;
  IF FOUND THEN
    RAISE SQLSTATE '23503' USING MESSAGE = 'rt_child row exists';
  END IF;
  IF TG_OP = 'DELETE' THEN
    RETURN OLD;
  END IF;
  RETURN NEW;
 END;
 $body$;
 CREATE TRIGGER ri_parent BEFORE UPDATE OR DELETE ON rt_parent FOR EACH ROW EXECUTE PROCEDURE ri_parent();
 CREATE FUNCTION ri_child() RETURNS TRIGGER LANGUAGE PLPGSQL AS $body$
 BEGIN
  PERFORM TRUE FROM rt_parent WHERE parent_id = NEW.parent_id;
  IF NOT FOUND THEN
    RAISE SQLSTATE '23503' USING MESSAGE = 'rt_parent row missing';
  END IF;
  RETURN NEW;
 END;
 $body$;
 CREATE TRIGGER ri_child BEFORE INSERT OR UPDATE ON rt_child FOR EACH ROW EXECUTE PROCEDURE ri_child();
 INSERT INTO rt_parent VALUES(0);
}

teardown
{
 DROP TABLE rt_parent, rt_child;
 DROP FUNCTION ri_parent();
 DROP FUNCTION ri_child();
}

session s1
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step wxry1	{ INSERT INTO rt_child (parent_id) VALUES (0); }
step c1		{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step r2		{ SELECT TRUE; }
step wyrx2	{ DELETE FROM rt_parent WHERE parent_id = 0; }
step c2		{ COMMIT; }
