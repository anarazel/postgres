#include "postgres_fe.h"

#include "port/debug.h"

#ifdef WIN32
#include <crtdbg.h>
#include <errhandlingapi.h>
#endif


#ifdef WIN32

static void
write_stderr(const char *message)
{
    (void) write(fileno(stderr), message, strlen(message));
}

int crt_report(int reportType, char *message, int *returnValue);

int
crt_report( int reportType, char *message, int *returnValue )
{
    const char *kind;

	if (reportType == _CRT_WARN)
		kind = "WARNING: ";
	else if (reportType == _CRT_ASSERT)
		kind = "ASSERT: ";
	else if (reportType == _CRT_ERROR)
		kind = "ERROR: ";
	else
		kind = "UNKNOWN: ";

    write_stderr("CRT ");
    write_stderr(kind);
	write_stderr(message);
	write_stderr("\n");

	/* trigger crash dump */
	if (reportType == _CRT_WARN)
	{
		*returnValue = 0;
		return TRUE;
	}
	else
	{
		*returnValue = 1;
		return TRUE;
	}
}

void
make_debugging_work(void)
{
	/*
	 * By default abort() only generates a crash-dump in *non* debug
	 * builds. As our Assert() / ExceptionalCondition() uses abort(),
	 * leaving the default in place would make debugging harder.
	 *
	 * MINGW's own C runtime doesn't have _set_abort_behavior(). When
	 * targeting Microsoft's UCRT with mingw, it never links to the debug
	 * version of the library and thus doesn't need the call to
	 * _set_abort_behavior() either.
	 */
#if !defined(__MINGW32__) && !defined(__MINGW64__)
	_set_abort_behavior(_CALL_REPORTFAULT | _WRITE_ABORT_MSG,
						_CALL_REPORTFAULT | _WRITE_ABORT_MSG);
#endif							/* !defined(__MINGW32__) &&
								 * !defined(__MINGW64__) */

	/*
	 * SEM_FAILCRITICALERRORS causes more errors to be reported to
	 * callers.
	 *
	 * We used to also specify SEM_NOGPFAULTERRORBOX, but that prevents
	 * windows crash reporting from working. Which includes registered
	 * just-in-time debuggers, making it unnecessarily hard to debug
	 * problems on windows. Now we try to disable sources of popups
	 * separately below (note that SEM_NOGPFAULTERRORBOX did not actually
	 * prevent all sources of such popups).
	 */
	SetErrorMode(SEM_FAILCRITICALERRORS);

	/*
	 * Show errors on stderr instead of popup box (note this doesn't
	 * affect errors originating in the C runtime, see below).
	 */
	_set_error_mode(_OUT_TO_STDERR);

	/*
	 * In DEBUG builds, errors, including assertions, C runtime errors are
	 * reported via _CrtDbgReport. By default such errors are displayed
	 * with a popup (even with NOGPFAULTERRORBOX), preventing forward
	 * progress. Instead report such errors stderr (and the debugger).
	 * This is C runtime specific and thus the above incantations aren't
	 * sufficient to suppress these popups.
	 */
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
	_CrtSetReportHook(crt_report);

	SetErrorMode(SEM_FAILCRITICALERRORS);
}

#else							/* WIN32 */

void
make_debugging_work(void)
{
}

#endif							/* WIN32 */
