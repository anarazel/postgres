#!/usr/bin/env python3
import math
import psycopg2
import random
import time
import traceback

order_by_forward = " ORDER BY a, b, c, d"
order_by_backward = " ORDER BY a DESC, b DESC, c DESC, d DESC"
order_by_forward_nullsfirst = " ORDER BY a NULLS FIRST, b NULLS FIRST, c NULLS FIRST, d NULLS FIRST"
order_by_backward_nullsfirst = " ORDER BY a DESC NULLS LAST, b DESC NULLS LAST, c DESC NULLS LAST, d DESC NULLS LAST"

def biased_random_int(min_val, max_val):
    """
    Generate a random integer between min_val and max_val (inclusive),
    with reduced probability near the boundaries.

    Args:
        min_val: Minimum possible value
        max_val: Maximum possible value

    Returns:
        int: A random integer with reduced probability near the bounds
    """
    # Use beta distribution to create a bell-shaped probability curve
    # Alpha = Beta = 2 gives a parabolic shape with lower probability at extremes
    alpha = 2
    beta = 2

    # Generate a random value between 0 and 1 with beta distribution
    random_val = random.betavariate(alpha, beta)

    # Scale to our range and round to integer
    scaled_val = min_val + random_val * (max_val - min_val)
    return round(scaled_val)

class PostgreSQLSkipScanTester:
    def __init__(self, conn_params, table_name, num_rows, num_samples,
                 num_tests, report_interval):
        self.conn_params = conn_params
        self.table_name = table_name
        self.num_rows = num_rows
        self.num_samples = num_samples
        self.num_tests = num_tests
        self.report_interval = report_interval
        self.columns = ['a', 'b', 'c', 'd']
        self.equality_operators = ['=', 'IN', 'IS NULL']
        self.inequality_operators = ['<', '<=', '>=', '>', 'IS NOT NULL']
        self.conn = None

    def connect(self):
        """Establish connection to PostgreSQL database"""
        try:
            self.conn = psycopg2.connect(**self.conn_params)
            print("Successfully connected")
        except Exception as e:
            print(f"Connection error: {e}")
            traceback.print_exc()
            raise

    def setup_test_environment(self):
        """Create test table and populate with random data"""
        try:
            cursor = self.conn.cursor()

            try:
                # First, check if the table exists in the database
                cursor.execute("""
                    SELECT EXISTS (
                        SELECT FROM information_schema.tables
                        WHERE table_name = %s
                    )
                """, (self.table_name,))
                table_exists = cursor.fetchone()[0]

                if table_exists:
                    # If table exists, check if it has the right number of rows
                    cursor.execute(f"SELECT count(*) FROM {self.table_name}")
                    idx_results = cursor.fetchall()
                    if idx_results[0][0] == self.num_rows:
                        print("Found preexisting table loaded from previous run")
                        return

            except Exception as e:
                pass

            # Create test table
            cursor.execute(f"DROP TABLE IF EXISTS {self.table_name}")
            cursor.execute(f"""
                create table {self.table_name} (
                    id serial primary key,
                    a integer,
                    b integer,
                    c integer,
                    d integer)
            """)

            # Create the composite index first (makes suffix truncation
            # effective)
            cursor.execute(f"""
                CREATE INDEX idx_{self.table_name}_abcd
                ON {self.table_name} (a, b, c, d)
            """)
            cursor.execute(f"""
                CREATE INDEX idx_{self.table_name}_abcd_nulls_first
                ON {self.table_name} (a nulls first, b nulls first, c nulls first, d nulls first)
            """)
            cursor.execute(f"""
                alter table {self.table_name} set (parallel_workers=0);
            """)
            # Insert random data (with some NULL values)
            for _ in range(self.num_rows):
                # Randomly decide whether to include NULL values for each column
                a_val = None if random.random() < 0.05 else random.randint(1, 20)
                b_val = None if random.random() < 0.05 else random.randint(1, 20)
                c_val = None if random.random() < 0.05 else random.randint(1, 100)
                d_val = None if random.random() < 0.05 else random.randint(1, 10_000)

                cursor.execute(f"""
                    INSERT INTO {self.table_name} (a, b, c, d)
                    VALUES (%s, %s, %s, %s)
                """, (a_val, b_val, c_val, d_val))

            self.conn.commit()

            # VACUUM
            self.conn.set_session(autocommit=True) # So that we can run VACUUM, etc
            cursor.execute(f"""
                vacuum analyze {self.table_name}
            """)

            print(f"Created test table {self.table_name} with {self.num_rows} rows")
            cursor.execute(f"""
                set max_parallel_workers_per_gather to 0;
            """)

        except Exception as e:
            self.conn.rollback()
            print(f"Setup error: {e}")
            traceback.print_exc()
            raise

    def generate_cond(self, column, operator_type=None,
                      eq_weights = [0.78, 0.20, 0.02], # Use IS NULL much less often
                      ineq_weights = [0.20, 0.20, 0.20, 0.17, 0.03]):   # Use IS NOT NULL much less often
        """
        Generate a single condition for a column using the specified operator type.

        Args:
            column: The column name to generate condition for
            operator_type: 'equality', 'inequality', or None (random)

        Returns:
            A condition string like "a > 5" or "b IS NULL"
        """
        if operator_type == 'equality':
            op = random.choices(self.equality_operators, weights=eq_weights)[0]
        elif operator_type == 'inequality':
            op = random.choices(self.inequality_operators, weights=ineq_weights)[0]
        else:
            # Random operator from all operators
            all_operators = self.equality_operators + self.inequality_operators
            op = random.choice(all_operators)

        # Handle operators that don't need values up front
        if op in ('IS NULL', 'IS NOT NULL'):
            return f"{column} {op}"

        # Define column value ranges
        column_ranges = {
            'a': (-1, 21),
            'b': (-1, 21),
            'c': (-1, 101),
            'd': (-1, 10_001)
        }

        # Get range for current column
        value_range = column_ranges.get(column, (-1, 21))  # Default range if unknown column

        # Handle IN
        if op == 'IN':
            nelements = random.randint(2, 20)
            elements = {biased_random_int(*value_range) for _ in range(nelements)}
            values_str = ", ".join(str(x) for x in sorted(elements))
            return f"{column} IN ({values_str})"

        # Handle all operators that need a single scalar value
        value = biased_random_int(*value_range)
        return f"{column} {op} {value}"

    def find_matching_operator_indices(self, all_conditions):
        matching_indices = set()
        # Define related operator pairs to avoid redundancies
        related_pairs = [(0, 1), (2, 3)]  # (<, <=) and (>=, >)

        for index, operator in enumerate(self.inequality_operators):
            for dynamic_string in all_conditions:
                if operator in dynamic_string:
                    matching_indices.add(index)
                    # Add related operator to avoid redundancies
                    for pair in related_pairs:
                        if index in pair:
                            matching_indices.update(pair)
                    break  # Found one match for this operator, no need to check other strings

        return matching_indices

    def sort_constraints(self, constraints):
        """
        Sort constraints with column name treated most significant, followed by operator order.
        Lower bound operators (>, >=) sort before upper bound operators (<, <=).  This presents
        things in a consistent order, that seems more readable.

        Args:
            constraints: List of constraint strings (e.g., ["a <= 5", "a > 8", "a >= 4", "b < 5"])

        Returns:
            list: Sorted list of constraints
        """
        # Define operator precedence (lower values = higher precedence)
        operator_priority = {
            ">": 0,   # Highest priority for lower bounds
            ">=": 1,
            "<": 2,   # Lower priority for upper bounds
            "<=": 3
        }

        def get_sort_key(constraint):
            # Parse the constraint into column and operator parts
            parts = constraint.split()
            if len(parts) >= 2:
                column = parts[0]
                operator = parts[1]

                # Return a tuple for sorting (column name, operator priority)
                return (column, operator_priority.get(operator, 999))
            return (constraint, 999)  # Fallback for unparseable constraints

        # Sort the constraints using the custom sort key
        return sorted(constraints, key=get_sort_key)

    def resolve_contradictions(self, conditions):
        """
        Allowing contradictory quals seems to be a poor use of available test cycles.
        Resolves contradictions in a list of conditions by swapping integer constants
        when needed, specifically for conditions on the same column that are impossible
        to satisfy simultaneously.

        Args:
            conditions: List of condition strings (e.g., ["b > 16", "b < 9", "d IS NOT NULL"])

        Returns:
            list: Modified list of conditions with contradictions resolved via
            constant swapping
        """
        # Parse conditions into a more usable format
        parsed_conditions = []
        for condition in conditions:
            parts = condition.split()
            # Only process numeric comparisons
            if len(parts) == 3 and parts[1] in ['>', '<', '>=', '<=']:
                try:
                    column = parts[0]
                    operator = parts[1]
                    value = int(parts[2])
                    parsed_conditions.append((column, operator, value, condition))
                except ValueError:
                    # If value isn't an integer, just keep the original condition
                    parsed_conditions.append((None, None, None, condition))
            else:
                # For non-comparison conditions like "IS NOT NULL"
                parsed_conditions.append((None, None, None, condition))

        # Group conditions by column
        column_conditions = {}
        for col, op, val, cond in parsed_conditions:
            if col is not None:
                if col not in column_conditions:
                    column_conditions[col] = []
                column_conditions[col].append((op, val, cond))

        # Check and resolve contradictions
        modified_conditions = conditions.copy()

        for column, col_conditions in column_conditions.items():
            lower_bounds = []  # > and >=
            upper_bounds = []  # < and <=

            # Separate into lower and upper bounds
            for op, val, cond in col_conditions:
                if op in ['>', '>=']:
                    lower_bounds.append((op, val, cond))
                elif op in ['<', '<=']:
                    upper_bounds.append((op, val, cond))

            # Check for contradictions between lower and upper bounds
            for lower_op, lower_val, lower_cond in lower_bounds:
                for upper_op, upper_val, upper_cond in upper_bounds:
                    # Contradiction if lower bound >= upper bound
                    is_contradiction = False

                    if lower_op == '>' and upper_op == '<' and lower_val >= upper_val:
                        is_contradiction = True
                    elif lower_op == '>' and upper_op == '<=' and lower_val >= upper_val:
                        is_contradiction = True
                    elif lower_op == '>=' and upper_op == '<' and lower_val >= upper_val:
                        is_contradiction = True
                    elif lower_op == '>=' and upper_op == '<=' and lower_val > upper_val:
                        is_contradiction = True

                    # If contradiction, swap the values
                    if is_contradiction:
                        # Create new conditions with swapped values
                        new_lower_cond = f"{column} {lower_op} {upper_val}"
                        new_upper_cond = f"{column} {upper_op} {lower_val}"

                        # Replace the old conditions with new ones
                        modified_conditions[modified_conditions.index(lower_cond)] = new_lower_cond
                        modified_conditions[modified_conditions.index(upper_cond)] = new_upper_cond

        return modified_conditions

    def generate_random_where_clause(self):
        """
        Generate a random WHERE clause with conditions on columns.
        For columns without equality conditions, sometimes generate multiple inequality conditions.
        """
        # Decide which columns will have conditions (at least one, at most four)
        assert(len(self.columns) == 4)
        weights = [0.05, 0.05, 0.05, 0.85]  # Probabilities for 1, 2, 3, or 4 columns
        num_columns_with_conditions = random.choices([1, 2, 3, 4], weights=weights)[0]
        columns_with_conditions = random.sample(self.columns, num_columns_with_conditions)

        # Avoid just having one column with conditions when that column is "a";
        # make it on "b", instead
        if num_columns_with_conditions == 1 and columns_with_conditions[0] == 'a':
            columns_with_conditions[0] = 'b'

        # Track which columns have equality/IS NULL conditions
        columns_with_equality = set()
        all_conditions = []

        # First pass: decide on equality vs inequality for each column
        for col in columns_with_conditions:
            # 50% chance of having an equality condition
            if random.random() < 0.5:
                condition = self.generate_cond(col, 'equality')
                all_conditions.append(condition)
                columns_with_equality.add(col)
            else:
                # Will handle inequalities in the next pass
                pass

        # Second pass: handle inequality conditions, possibly multiple per column
        for col in columns_with_conditions:
            if col not in columns_with_equality:
                # This column doesn't have an equality condition, so it can have multiple inequalities

                # Determine how many inequality conditions to add (1-3)
                weights = [0.25, 0.7, 0.05]  # Probabilities for 1, 2, or 3 inequality conditions
                num_conditions = random.choices([1, 2, 3], weights=weights)[0]
                # ineq_weights is self.inequality_operators-offset-wise list:
                ineq_weights = [0.20, 0.20, 0.20, 0.17, 0.03]

                for _ in range(num_conditions):
                    zero_weights = self.find_matching_operator_indices(all_conditions)
                    if zero_weights == set([0, 1, 2, 3, 4]):
                        break
                    for index, value in enumerate(zero_weights):
                        ineq_weights[value] = 0
                    condition = self.generate_cond(col, 'inequality',
                                                   ineq_weights=ineq_weights)
                    all_conditions.append(condition)

        # If we somehow ended up with no conditions (unlikely), add one
        if not all_conditions:
            col = random.choice(self.columns)
            all_conditions.append(self.generate_cond(col))

        all_conditions = self.sort_constraints(all_conditions)
        all_conditions = self.resolve_contradictions(all_conditions)
        if random.random() < 0.50:
            where_clause_str = " AND ".join(all_conditions) + (order_by_forward if random.random() < 0.50 else
                                                               order_by_backward)
        else:
            where_clause_str = " AND ".join(all_conditions) + (order_by_forward_nullsfirst if random.random() < 0.50 else
                                                               order_by_backward_nullsfirst)
        return where_clause_str

    def execute_test_query(self, where_clause):
        """Execute a test query with both sequential scan and index scan"""
        cursor = self.conn.cursor()

        # Force sequential scan
        cursor.execute("SET enable_indexscan = off; SET enable_bitmapscan = off;")
        seq_query = f"EXPLAIN ANALYZE SELECT * FROM {self.table_name} WHERE {where_clause}"
        cursor.execute(seq_query)
        seq_plan = cursor.fetchall()

        # Get sequential scan results
        cursor.execute(f"SELECT * FROM {self.table_name} WHERE {where_clause}")
        seq_results = cursor.fetchall()

        # Force index scan
        cursor.execute("SET enable_indexscan = on; SET enable_seqscan = off; SET enable_bitmapscan = off;")
        idx_query = f"EXPLAIN ANALYZE SELECT * FROM {self.table_name} WHERE {where_clause}"
        cursor.execute(idx_query)
        idx_plan = cursor.fetchall()

        # Get index scan results
        cursor.execute(f"SELECT * FROM {self.table_name} WHERE {where_clause}")
        idx_results = cursor.fetchall()

        # Reset scan settings
        cursor.execute("RESET enable_indexscan; RESET enable_seqscan; RESET enable_bitmapscan;")

        return {
            'where_clause': where_clause,
            'seq_plan': seq_plan,
            'idx_plan': idx_plan,
            'seq_results': seq_results,
            'idx_results': idx_results,
            'results_match': sorted(seq_results) == sorted(idx_results),
            'seq_count': len(seq_results),
            'idx_count': len(idx_results)
        }

    def verify_scan_results(self, test_result):
        """Verify that sequential scan and index scan results match"""
        if not test_result['results_match']:
            print("\n❌ TEST FAILED: Results do not match!")
            print(f"Query: SELECT * FROM {self.table_name} WHERE {test_result['where_clause']}")
            print(f"Sequential scan found {test_result['seq_count']} rows")
            print(f"Index scan found {test_result['idx_count']} rows")
            return False
        return True

    def run_fuzzing_queries(self):
        """Run a batch of random test queries and verify results"""
        print(f"\nRunning {self.num_tests} random test queries...")

        start_time = time.time()
        failures = 0
        multiple_inequality_count = 0

        for i in range(1, self.num_tests + 1):
            where_clause = self.generate_random_where_clause()

            # Count queries with multiple inequalities on the same column (fixed)
            multiple_inequalities = False
            for column in self.columns:
                # Count occurrences of inequality operators for this column
                ninequalities_for_column = sum(1 for op in ['<', '<=', '>=', '>', 'IS NOT NULL']
                                               if f"{column} {op}" in where_clause)
                if ninequalities_for_column > 1:
                    multiple_inequalities = True
                    break

            if multiple_inequalities:
                multiple_inequality_count += 1

            test_result = self.execute_test_query(where_clause)

            if not self.verify_scan_results(test_result):
                failures += 1

            if i % self.report_interval == 0:
                print(f"Completed {i} tests. Failures: {failures}")

        end_time = time.time()
        duration = end_time - start_time

        print(f"\nCompleted {self.num_tests} tests in {duration:.2f} seconds")
        print(f"Queries with multiple inequalities on the same column: {multiple_inequality_count}")
        print(f"Total failures: {failures}")

        if failures == 0:
            print("✅ All tests passed!")
        else:
            print(f"❌ {failures} tests failed!")

        return failures == 0

    def dump_plan_samples(self):
        """Analyze and print execution plans for a few sample queries"""
        print(f"\nAnalyzing execution plans for {self.num_samples} sample queries...")

        for i in range(self.num_samples):
            where_clause = self.generate_random_where_clause()
            test_result = self.execute_test_query(where_clause)

            print(f"\nQuery {i+1}: SELECT * FROM {self.table_name} WHERE {where_clause}")
            print("\nSequential scan plan:")
            for line in test_result['seq_plan']:
                print(line[0])

            print("\nIndex scan plan:")
            for line in test_result['idx_plan']:
                print(line[0])

            print(f"\nResults match: {test_result['results_match']}")
            print(f"Row count: {test_result['seq_count']}")
            print("-" * 80)

    def cleanup(self):
        """Close connection"""
        if self.conn:
            try:
                cursor = self.conn.cursor()
                self.conn.commit()
                self.conn.close()
                print(f"Closed connection")
            except Exception as e:
                print(f"Cleanup error: {e}")
                traceback.print_exc()

    def run_all_tests(self):
        """Run all test types"""
        try:
            self.connect()
            self.setup_test_environment()

            # Analyze some sample execution plans first, to preview the work
            # that run_fuzzing_queries() will do (runs quickly)
            self.dump_plan_samples()

            # The real work happens in run_fuzzing_queries() (takes a while)
            return self.run_fuzzing_queries()

        except Exception as e:
            print(f"Test error: {e}")
            traceback.print_exc()
            return False
        finally:
            self.cleanup()


if __name__ == "__main__":
    # Connection parameters - adjust as needed
    conn_params = {
        "host": "localhost",
        "database": "regression",
        "user": "pg",
    }

    # Create and run the tester
    tester = PostgreSQLSkipScanTester(
        conn_params=conn_params,
        table_name="fuzz_skip_scan",
        num_rows=150_000, # rows in `table_name` table
        num_samples=10, # Number of plan samples to dump (previews test query structure)
        num_tests=5_000_000, # Number of test queries
        report_interval=1000 # Report progress each time this many queries run
    )

    success = tester.run_all_tests()

    if success:
        print("\n✅ All tests completed successfully")
    else:
        print("\n❌ Test failures detected")
