/*-------------------------------------------------------------------------
 *
 * backtrace.c
 *	  Support for generating backtraces
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/backtrace.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <unistd.h>
#include <signal.h>


#if defined(HAVE_BACKTRACE_CREATE_STATE) && defined(HAVE_BACKTRACE_H)
#define USE_LIBBACKTRACE
#elif defined(HAVE_BACKTRACE_SYMBOLS) && defined(HAVE_EXECINFO_H)
#define USE_EXECINFO
#endif

#if defined(USE_LIBBACKTRACE)
#include <backtrace.h>
#include <backtrace-supported.h>
#elif defined(USE_EXECINFO)
#include <execinfo.h>
#endif

#include "common/pg_backtrace.h"
#include "common/logging.h"
#include "lib/stringinfo.h"


/*
 * Helpers for doing very basic IO in a signal safe way. Extern just because
 * it's too complicated ifdefer-y to make them just available where needed.
 */
extern void sigsafe_ultoa(uint64 value, int base, char *a);
extern void sigsafe_ltoa(int64 value, int base, char *a);
extern void sigwrite(int fd, const char *s);


#if defined(USE_LIBBACKTRACE)
struct pg_backtrace_print_state;
typedef void (*pg_backtrace_out_cb) (struct pg_backtrace_print_state *ps,
									 const char *s);

typedef struct pg_backtrace_print_state
{
	/* state across a whole backtrace computation */
	StringInfo	si;
	int			fd;
	const char *line_start;
	const char *line_end;

	bool		errored;

	pg_backtrace_out_cb out;

	/* state for the current symbol */
	const char *symbol_name;
	uintptr_t	symbol_start;
	uintptr_t	symbol_size;
} pg_backtrace_print_state;
#endif							/* USE_LIBBACKTRACE */

static bool pg_bt_initialized = false;
static bool pg_bt_threaded = false;

#ifdef FRONTEND

/*
 * FIXME: This should obviously be implemented somewhere central instead, but
 * ...
 */
#ifdef __GNUC__
#define pg_bt_thread_local __thread
#elif defined(_MSC_VER)
#define pg_bt_thread_local __declspec(thread)
#endif

static pg_bt_thread_local bool pg_bt_crash_handler_initialized;
#else
static bool pg_bt_crash_handler_initialized;
#endif

#if defined(USE_LIBBACKTRACE)
static struct backtrace_state *backtrace_state;
#endif							/* USE_LIBBACKTRACE */


/*
 * Does the current configuration support backtrace generation.
 *
 * Can be called before pg_bt_initialize().
 */
bool
pg_bt_is_supported(void)
{
#if defined(USE_LIBBACKTRACE)
	return BACKTRACE_SUPPORTED;
#elif defined(USE_EXECINFO)
	return true;
#else
	return false;
#endif
}

/*
 * Does the current configuration support backtrace generation in signal
 * handlers.
 *
 * Can be called before pg_bt_initialize().
 */
bool
pg_bt_is_signal_safe(void)
{
#if defined(USE_LIBBACKTRACE)
	return BACKTRACE_USES_MALLOC == 0;
#elif defined(USE_EXECINFO)
	return true;
#else
	return false;
#endif
}

#if defined(USE_LIBBACKTRACE)
static void
pg_backtrace_create_error_cb(void *data, const char *msg,
							 int errnum)
{
#if FRONTEND
	pg_log_warning("backtrace state creation failure: msg: %s, errnum: %d",
				   msg, errnum);
#else
	elog(WARNING, "backtrace state creation failure: msg: %s, errnum: %d",
		 msg, errnum);
#endif
}
#endif							/* USE_LIBBACKTRACE */

/*
 * Initialize backtrace generation. Needs to happen before any backtraces may
 * be generated.
 */
void
pg_bt_initialize(const char *progname, bool threaded)
{
	Assert(!pg_bt_initialized);
	pg_bt_initialized = true;
	pg_bt_threaded = threaded;

#if defined(USE_LIBBACKTRACE)

#ifdef WIN32

	/*
	 * libbacktrace can't figure the filename out on windows. Nor is our
	 * progname useful.
	 */
	{
		static char windows_filename[MAXPGPATH];

		GetModuleFileNameA(NULL, windows_filename, sizeof(windows_filename));
		for (int i = 0; i < MAXPGPATH; i++)
		{
			if (windows_filename[i] == '\\')
				windows_filename[i] = '/';
		}
		progname = windows_filename;
	}
#endif

	/*
	 * The state is long-lived and can't be freed. The error callback, if
	 * necessary, will be called while backtrace_create_state() is running.
	 */
	Assert(backtrace_state == NULL);
	backtrace_state = backtrace_create_state(
											 progname, threaded,
											 pg_backtrace_create_error_cb, NULL);

	/*
	 * XXX: In the backend it might be worth loading debuginfo here to avoid
	 * having to do so over and over in each processs.
	 */
#elif defined(USE_EXECINFO)
	{
		/*
		 * backtrace() doesn't allocate memory, but using it for the first
		 * time could trigger dynamic linker activity (see NOTES in manpage).
		 * Trigger that now.
		 */
		void	   *buf[100];

		backtrace(buf, lengthof(buf));
	}
#endif
}

#ifdef USE_LIBBACKTRACE

/* Sign + the most decimal digits an 8-byte number could have */
/*
 * FIXME: Unfortunately the correct value (20) triggers reams of spurious
 * -Wstringop-overflow= warnings
 */
#define MAXINT8LEN 32

static void
out_string(pg_backtrace_print_state *ps,
		   const char *s)
{
	ps->out(ps, s);
}

static void
out_int(pg_backtrace_print_state *ps,
		int64 i)
{
	char		buf[MAXINT8LEN];

	sigsafe_ltoa(i, 10, buf);
	ps->out(ps, buf);
}

static void
out_hex(pg_backtrace_print_state *ps,
		uint64 i)
{
	char		buf[MAXINT8LEN];

	sigsafe_ultoa(i, 16, buf);
	ps->out(ps, buf);
}

static void
pg_backtrace_out_si(pg_backtrace_print_state *ps,
					const char *s)
{
	appendStringInfoString(ps->si, s);
}

static void
pg_backtrace_out_fd(pg_backtrace_print_state *ps,
					const char *s)
{
	sigwrite(ps->fd, s);
}

static void
pg_backtrace_error_cb(void *data, const char *msg, int errnum)
{
	pg_backtrace_print_state *ps = (pg_backtrace_print_state *) data;

	if (!ps->errored)
	{
		out_string(ps, "backtrace failure: msg: ");
		out_string(ps, msg);
		out_string(ps, ", errnum: ");
		out_int(ps, errnum);
		out_string(ps, "\n");
		ps->errored = true;
	}
}

static void
pg_backtrace_syminfo(void *data, uintptr_t pc,
					 const char *symname,
					 uintptr_t symval,
					 uintptr_t symsize)
{
	pg_backtrace_print_state *printstate = (pg_backtrace_print_state *) data;

	printstate->symbol_name = symname;
	printstate->symbol_start = symval;
	printstate->symbol_size = symsize;
}

static int
pg_backtrace_pcinfo_cb(void *data, uintptr_t pc,
					   const char *filename, int lineno,
					   const char *function)
{
	pg_backtrace_print_state *ps = (pg_backtrace_print_state *) data;
	bool		is_inlined;

	/* signal to the caller that no useful pcinfo was found */
	if (!filename || !function)
		return 1;

	is_inlined = ps->symbol_name &&
		strcmp(function, ps->symbol_name) != 0;

	out_string(ps, ps->line_start);
	out_string(ps, "[0x");
	out_hex(ps, pc);
	out_string(ps, "] ");
	out_string(ps, function);
	out_string(ps, "+0x");
	out_hex(ps, pc - ps->symbol_start);
	if (is_inlined)
		out_string(ps, " (inlined)");
	out_string(ps, ": ");
	out_string(ps, filename);
	out_string(ps, ":");
	out_int(ps, lineno);
	out_string(ps, ps->line_end);

	return 0;
}

static int
pg_backtrace_cb(void *data, uintptr_t pc)
{
	pg_backtrace_print_state *ps = (pg_backtrace_print_state *) data;
	int			ret;

	if (pc == ~(uintptr_t) 0)
		return 1;
#if 1
	backtrace_syminfo(backtrace_state, pc,
					  pg_backtrace_syminfo,
					  pg_backtrace_error_cb,
					  data);

	/*
	 * If debug information is available, we will be able to get line
	 * information as well. The pcinfo callback might be called multiple
	 * times, if we pc is inside an inlined function.
	 */
	ret = backtrace_pcinfo(backtrace_state, pc,
						   pg_backtrace_pcinfo_cb,
						   pg_backtrace_error_cb,
						   data);
#else
	ret = 1;
#endif
	if (ret)
	{
		out_string(ps, ps->line_start);
		out_string(ps, "[0x");
		out_hex(ps, pc);
		out_string(ps, "] ");
		if (ps->symbol_name)
		{
			out_string(ps, ps->symbol_name);
			out_string(ps, "+0x");
			out_hex(ps, pc - ps->symbol_start);
		}
		else
			out_string(ps, "[unknown]");
		out_string(ps, ps->line_end);
	}

	/* XXX: is there a reason to not stop tracing after reaching main? */
	if (ps->symbol_name && strcmp(ps->symbol_name, "main") == 0)
		return 1;

	return 0;
}


#endif

bool
pg_bt_print_to_stringinfo(StringInfo si, int num_skip, const char *line_start, const char *line_end)
{
#if defined(USE_LIBBACKTRACE)
	if (!backtrace_state)
		return false;
	else
	{
		pg_backtrace_print_state printstate = {
			.si = si,
			.out = pg_backtrace_out_si,
			.fd = -1,
			.line_start = "\n",
			.line_end = "",
		};

		backtrace_simple(backtrace_state, num_skip,
						 pg_backtrace_cb,
						 pg_backtrace_error_cb,
						 &printstate);
		return true;
	}
#elif defined(USE_EXECINFO)
	{
		void	   *buf[100];
		int			nframes;
		char	  **strfrms;

		nframes = backtrace(buf, lengthof(buf));
		strfrms = backtrace_symbols(buf, nframes);
		if (strfrms == NULL)
			false;

		for (int i = num_skip; i < nframes; i++)
			appendStringInfo(si, "%s%s%s",
							 line_start, line_end, strfrms[i]);
		free(strfrms);

		return true;
	}
#endif

	return false;
}

void
pg_bt_print_to_fd(int fd, bool indent)
{
	/* If we have support for it, dump a simple backtrace */
#if defined(USE_LIBBACKTRACE)
	if (!backtrace_state)
		return;
	else
	{
		pg_backtrace_print_state printstate = {
			.si = NULL,
			.out = pg_backtrace_out_fd,
			.fd = fd,
			.line_start = "\t",
			.line_end = "\n",
		};

		backtrace_simple(backtrace_state, 1,
						 pg_backtrace_cb,
						 pg_backtrace_error_cb,
						 &printstate);
	}
#elif defined(USE_EXECINFO)
	{
		void	   *buf[100];
		int			nframes;

		nframes = backtrace(buf, lengthof(buf));
		backtrace_symbols_fd(buf, nframes, fileno(stderr));
	}
#endif
}

void
sigwrite(int fd, const char *s)
{
	/*
	 * There's nothing we can do if the write fails as we're executing in a
	 * signal handler. Unfortunately a (void) cast doesn't quiesce gcc...
	 */
	if (write(fd, s, strlen(s)))
		return;
	return;
}

/*
 * Copy of a simple implementation of ltoa, for the purpose of
 */
void
sigsafe_ultoa(uint64 value, int base, char *a)
{
	char	   *start = a;

	if (base != 10 && base != 16)
	{
		*a = '\0';
		return;
	}

	/* Compute the result string backwards. */
	do
	{
		int64		remainder;
		int64		oldval = value;

		value /= base;
		remainder = oldval - value * base;
		if (remainder < 10)
			*a++ = '0' + remainder;
		else
			*a++ = 'a' + (remainder - 10);

	} while (value != 0);

	/* Add trailing NUL byte, and back up 'a' to the last character. */
	*a-- = '\0';

	/* Reverse string. */
	while (start < a)
	{
		char		swap = *start;

		*start++ = *a;
		*a-- = swap;
	}
}

void
sigsafe_ltoa(int64 value, int base, char *a)
{
	uint64		uvalue = (uint64) value;

	if (value < 0)
	{
		*a++ = '-';
		uvalue = (uint64) 0 - uvalue;
	}

	sigsafe_ultoa(uvalue, base, a);
}

#ifndef WIN32

static void
pg_fatalsig_handler(int signo, siginfo_t * siginfo, void *ucontext)
{
	int			pid;
	char		buf[128];
	const char *signame;
	bool		have_addr = false;
	bool		by_user;
	int			save_errno = errno;

	if (signo == SIGSEGV)
	{
		signame = "SIGSEGV";
		have_addr = true;
	}
	else if (signo == SIGILL)
	{
		signame = "SIGILL";
		have_addr = true;
	}
	else if (signo == SIGBUS)
	{
		signame = "SIGBUS";
		have_addr = true;
	}
	else if (signo == SIGABRT)
	{
		signame = "SIGABRT";
	}
	else
	{
		signame = "other";
	}


	by_user = siginfo->si_code <= SI_USER;
	pid = getpid();

	/*
	 * Start with a newline, the crash could have happened in the middle of a
	 * line.
	 */
	sigwrite(STDERR_FILENO, "\nprocess with pid: ");
	sigsafe_ltoa(pid, 10, buf);
	sigwrite(STDERR_FILENO, buf);

#ifdef HAVE_GETTID
	if (pg_bt_threaded)
	{
		/*
		 * FIXME: need to make this conditional on having gettid(). But I
		 * don't think we can dare calling into pthread at this point.
		 */
		sigwrite(STDERR_FILENO, ", tid: ");
		sigsafe_ltoa(gettid(), 10, buf);
		sigwrite(STDERR_FILENO, buf);
	}
#endif

	sigwrite(STDERR_FILENO, " received signal: ");
	sigwrite(STDERR_FILENO, signame);

	sigwrite(STDERR_FILENO, ", si_code: ");
	sigsafe_ltoa(siginfo->si_code, 10, buf);
	sigwrite(STDERR_FILENO, buf);


	/*
	 * If the signal was (likely) triggered by a user, print the pid of the
	 * sending user.
	 */
	if (by_user)
	{
		sigwrite(STDERR_FILENO, ", si_pid: ");
		sigsafe_ltoa(siginfo->si_pid, 10, buf);
		sigwrite(STDERR_FILENO, buf);
	}

	/*
	 * Not much point in logging the address if triggered by a user.
	 */
	if (!by_user && have_addr)
	{
		sigwrite(STDERR_FILENO, ", si_addr: 0x");
		sigsafe_ultoa((uintptr_t) siginfo->si_addr, 16, buf);
		sigwrite(STDERR_FILENO, buf);
	}

	sigwrite(STDERR_FILENO, "\n");

	if (pg_bt_is_signal_safe())
		pg_bt_print_to_fd(STDERR_FILENO, true);

	/*
	 * We've used SA_RESETHAND when setting up the signal handler. Execution
	 * will continue and the same error will be raised again, this time
	 * terminating the execution. This is advantageous because it means the
	 * caller will get the same information we got.
	 *
	 * However that doesn't work if the signal has explicitly been raise()d.
	 * Execution could just continue. Thus, if somebody / something sent us
	 * the signal, reraise explicitly. On kernels inspected, values smaller
	 * than SI_USER (0) are for userspace sent signals.
	 */
	if (siginfo->si_code <= SI_USER)
		raise(signo);

	/* not that it matters, but let's just follow the rules... */
	errno = save_errno;
}

typedef void (*sa_sigaction_handler) (int, siginfo_t *, void *);

/*
 * FIXME: This should be obsoleted by
 * a) using SA_SIGINFO in pqsignal()
 * b) providing a version of the normal pqsignal() that allows to specify
 *    SA_ONSTACK
 */
static bool
pqsignal_crash(int signo, sa_sigaction_handler handler)
{
	struct sigaction act,
				oact;

	act.sa_sigaction = handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART | SA_ONSTACK | SA_RESETHAND | SA_SIGINFO;

	if (sigaction(signo, &act, &oact) < 0)
		return false;

	return true;
}

static bool
setup_sigaltstack(void)
{
	stack_t		ss_old = {0};
	stack_t		ss_new;

	/*
	 * Allocate an alternative stack to execute fatal signal handlers on.
	 * Without that it's much more likely that the signal handler just
	 * confuses debugging (e.g. by crashing due to executing on an overflowed
	 * stack).
	 */
	ss_new.ss_sp = palloc(SIGSTKSZ);
	ss_new.ss_size = SIGSTKSZ;
	ss_new.ss_flags = 0;
	if (sigaltstack(&ss_new, &ss_old) == -1)
	{
#ifdef FRONTEND
		pg_log_error("sigaltstack failed: %m");
#else
		elog(ERROR, "sigaltstack failed: %m");
#endif
		return false;
	}

	/*
	 * It's possible that some tool (e.g. asan) already set up an alternative
	 * stack. In that case we'll just rely on that, to avoid interfering.
	 */
	if (ss_old.ss_sp)
	{
		if (sigaltstack(&ss_old, NULL) == -1)
		{
#ifdef FRONTEND
			pg_log_error("reverting sigaltstack failed: %m");
#else
			elog(ERROR, "reverting sigaltstack failed: %m");
#endif
		}
		pfree(ss_new.ss_sp);

		/* we still consider this a success */
	}

	return true;
}

#endif

#ifdef WIN32

static LONG WINAPI
pg_fatal_handler(EXCEPTION_POINTERS * ep)
{
	EXCEPTION_RECORD *er = ep->ExceptionRecord;
	int			pid;
	char		buf[128];
	const char *reason;

	pid = getpid();

	sigwrite(STDERR_FILENO, "\nprocess with pid: ");
	sigsafe_ltoa(pid, 10, buf);
	sigwrite(STDERR_FILENO, buf);
	sigwrite(STDERR_FILENO, " crashed");
	/* TODO: print out more useful information */

#define ME(e) case CppConcat(EXCEPTION_, e): reason = CppAsString(e); break
	switch (er->ExceptionCode)
	{
			ME(ACCESS_VIOLATION);
			ME(STACK_OVERFLOW);
			ME(ILLEGAL_INSTRUCTION);
			ME(IN_PAGE_ERROR);
			ME(DATATYPE_MISALIGNMENT);
		default:
			reason = NULL;
			break;
	}
#undef ME

	sigwrite(STDERR_FILENO, " due to ");
	if (reason)
		sigwrite(STDERR_FILENO, reason);
	else
	{
		sigwrite(STDERR_FILENO, "unknown reason ");
		sigsafe_ltoa(er->ExceptionCode, 10, buf);
		sigwrite(STDERR_FILENO, buf);
	}

	sigwrite(STDERR_FILENO, " at address 0x");
	sigsafe_ultoa((uintptr_t) er->ExceptionAddress, 16, buf);
	sigwrite(STDERR_FILENO, buf);

	sigwrite(STDERR_FILENO, "\n");

	/*
	 * Unfortunately that currently won't be true. In a pinch it can be useful
	 * to just use the backtraces anyway...
	 */
	if (pg_bt_is_signal_safe())
		pg_bt_print_to_fd(STDERR_FILENO, true);

	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/*
 * Configure the current thread to intercept crashes, to print out a backtrace
 * in that case, before re-raising the error.
 *
 * Needs to be called exactly once in every thread and first on the main
 * thread (the latter could be relaxed, it's just a way to not need
 * locks). The reason that this needs to be called once on each thread is that
 * we need to configure separate signal stacks for each.
 */
bool
pg_bt_setup_crash_handler(void)
{
	static bool registered_handler = false;

	Assert(!pg_bt_crash_handler_initialized);
	pg_bt_crash_handler_initialized = true;

#ifndef WIN32

	/*
	 * Register the handler even if we can't safely print backtraces in the
	 * signal handler - even just the information that/why the process crashed
	 * is useful.
	 */
	if (!registered_handler)
	{
		registered_handler = true;

		if (!pqsignal_crash(SIGSEGV, pg_fatalsig_handler))
			return false;

		if (!pqsignal_crash(SIGILL, pg_fatalsig_handler))
			return false;

		if (!pqsignal_crash(SIGBUS, pg_fatalsig_handler))
			return false;

		if (!pqsignal_crash(SIGABRT, pg_fatalsig_handler))
			return false;

		return true;
	}

	if (!setup_sigaltstack())
		return false;
#else
	if (!registered_handler)
	{
		SetUnhandledExceptionFilter(pg_fatal_handler);

		return true;
	}
#endif

	return false;
}
