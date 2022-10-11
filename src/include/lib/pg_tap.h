/*
 * Simple macros for writing tests in C that print results in TAP format,
 * as consumed by "prove".
 *
 * https://testanything.org/
 */

#ifndef PG_TAP_H
#define PG_TAP_H

#include <stdio.h>
#include <string.h>

static int pg_test_count;
static int pg_fail_count;

/*
 * Require an expression to be true.  Used for set-up steps that are not
 * reported as a test.
 */
#define PG_REQUIRE(expr) \
if (!(expr)) { \
	printf("Bail out! requirement (" #expr ") failed at %s:%d\n", \
		__FILE__, __LINE__); \
	exit(1); \
}

/*
 * Like PG_REQUIRE, but logging the strerror(errno) before bailing.
 */
#define PG_REQUIRE_SYS(expr) \
if (!(expr)) { \
	printf("Bail out! requirement (" #expr ") failed at %s:%d, error: %s\n", \
		__FILE__, __LINE__, strerror(errno)); \
	exit(1); \
}

/*
 * Test that an expression is true, logging the expression if not.
 */
#define PG_EXPECT(expr, message) \
do { \
	pg_test_count++; \
	if (expr) { \
		printf("ok %d - %s\n", pg_test_count, message); \
	} else { \
		pg_fail_count++; \
		printf("not ok %d - %s (at %s:%d)\n", pg_test_count, \
			message, __FILE__, __LINE__); \
	} \
} while (0)

/*
 * Test that an expression is true, logging the expression and strerror(errno)
 * it not.
 */
#define PG_EXPECT_SYS(expr, message) \
do { \
	pg_test_count++; \
	if (expr) { \
		printf("ok %d - %s\n", pg_test_count, message); \
	} else { \
		pg_fail_count++; \
		printf("not ok %d - %s (at %s:%d), error: %s\n", pg_test_count, \
			message, __FILE__, __LINE__, strerror(errno)); \
	} \
} while (0)


/*
 * Test that one int expression is equal to another, logging the values if not.
 */
#define PG_EXPECT_EQ(expr1, expr2, message) \
do { \
	int expr1_val = (expr1); \
	int expr2_val = (expr2); \
	pg_test_count++; \
	if (expr1_val == expr2_val) { \
		printf("ok %d - %s\n", pg_test_count, message); \
	} else { \
		pg_fail_count++; \
		printf("not ok %d - failed %d != %d (at %s:%d)\n", pg_test_count, \
			expr1_val, expr2_val, __FILE__, __LINE__); \
	} \
} while (0)

#define PG_BEGIN_TESTS() /* TAP version 14 */
#define PG_END_TESTS() printf("1..%d\n", pg_test_count)

#endif
