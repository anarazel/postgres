/*-------------------------------------------------------------------------
 *
 * pg_backtrace.h
 *	  Support for generating backtraces
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/pg_backtrace.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_BACKTRACE_H
#define PG_BACKTRACE_H

#include "lib/stringinfo.h"

extern bool pg_bt_is_supported(void);
extern bool pg_bt_is_signal_safe(void);

extern void pg_bt_initialize(const char *progname, bool threaded);

extern void pg_bt_print_to_fd(int fd, bool indent);
extern bool pg_bt_print_to_stringinfo(StringInfo si, int num_skip,
									  const char *line_start, const char *line_end);

extern bool pg_bt_setup_crash_handler(void);

#endif							/* PG_BACKTRACE_H */
