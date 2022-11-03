/*-------------------------------------------------------------------------
 *
 * assert.c
 *	  Assert support code.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/assert.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "common/pg_backtrace.h"

/*
 * ExceptionalCondition - Handles the failure of an Assert()
 *
 * We intentionally do not go through elog() here, on the grounds of
 * wanting to minimize the amount of infrastructure that has to be
 * working to report an assertion failure.
 */
void
ExceptionalCondition(const char *conditionName,
					 const char *fileName,
					 int lineNumber)
{
	/* Report the failure on stderr (or local equivalent) */
	if (!PointerIsValid(conditionName)
		|| !PointerIsValid(fileName))
		write_stderr("TRAP: ExceptionalCondition: bad arguments in PID %d\n",
					 (int) getpid());
	else
		write_stderr("TRAP: failed Assert(\"%s\"), File: \"%s\", Line: %d, PID: %d\n",
					 conditionName, fileName, lineNumber, (int) getpid());

	/* Usually this shouldn't be needed, but make sure the msg went out */
	fflush(stderr);

	/*
	 * If we have support for it, dump a simple backtrace. Be paranoid and use
	 * the variant printing the backtraces to stderr, in case global state is
	 * corrupted.
	 */
	if (pg_bt_is_supported())
		pg_bt_print_to_fd(STDERR_FILENO, true);

	/*
	 * If configured to do so, sleep indefinitely to allow user to attach a
	 * debugger.  It would be nice to use pg_usleep() here, but that can sleep
	 * at most 2G usec or ~33 minutes, which seems too short.
	 */
#ifdef SLEEP_ON_ASSERT
	sleep(1000000);
#endif

	abort();
}
