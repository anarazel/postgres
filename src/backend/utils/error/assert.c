/*-------------------------------------------------------------------------
 *
 * assert.c
 *	  Assert support code.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
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
#if defined(HAVE_BACKTRACE_CREATE_STATE)
#include <backtrace.h>
#elif defined(HAVE_EXECINFO_H)
#include <execinfo.h>
#endif

#ifdef HAVE_BACKTRACE_CREATE_STATE
static void
pg_backtrace_error_callback(void *data, const char *msg, int errnum)
{
	char	   *errtrace = (char *) data;
	char		buffer[1000];

	memset(buffer, 0, sizeof(buffer));
	snprintf(buffer, sizeof(buffer), "backtrace failure: msg: %s, errnum: %d\n",
			 msg, errnum);

	strcat(errtrace, buffer);
}

static int
pg_backtrace_full_callback(void *data, uintptr_t pc,
						   const char *filename, int lineno,
						   const char *function)
{
	char	   *errtrace = (char *) data;
	char		buffer[1000];

	if (pc == 0xffffffffffffffff)
		return 1;

	memset(buffer, 0, sizeof(buffer));
	snprintf(buffer, sizeof(buffer), "[%p] %s: %s:%d\n",
			 (void *) pc,
			 function ? function : "[unknown]",
			 filename ? filename : "[unknown]", lineno);

	strcat(errtrace, buffer);

	return 0;
}
#endif

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
	if (!conditionName || !fileName)
		write_stderr("TRAP: ExceptionalCondition: bad arguments in PID %d\n",
					 (int) getpid());
	else
		write_stderr("TRAP: failed Assert(\"%s\"), File: \"%s\", Line: %d, PID: %d\n",
					 conditionName, fileName, lineNumber, (int) getpid());

	/* Usually this shouldn't be needed, but make sure the msg went out */
	fflush(stderr);

	/* If we have support for it, dump a simple backtrace */
#ifdef HAVE_BACKTRACE_CREATE_STATE
	{
		char		buf[5000];
		static struct backtrace_state *state;

		memset(buf, 0, sizeof(buf));
		buf[0] = '\0';
		state = backtrace_create_state(
									   NULL, /* threaded = */ false,
									   pg_backtrace_error_callback, buf);

		/*
		 * The state is long-lived and can't be freed. The error callback, if
		 * necessary, will be called while backtrace_create_state() is
		 * running, so it's ok to pass errtrace here.
		 */
		backtrace_full(state, 1,
					   pg_backtrace_full_callback,
					   pg_backtrace_error_callback,
					   buf);
		write_stderr("%s\n", buf);
		fflush(stderr);
	}
#elif defined(HAVE_BACKTRACE_SYMBOLS)
	{
		void	   *buf[100];
		int			nframes;

		nframes = backtrace(buf, lengthof(buf));
		backtrace_symbols_fd(buf, nframes, fileno(stderr));
	}
#endif

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
