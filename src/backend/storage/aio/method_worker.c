/*-------------------------------------------------------------------------
 *
 * method_worker.c
 *    AIO implementation using workers
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/method_worker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/io_worker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/wait_event.h"


int			io_workers = 3;


void
IoWorkerMain(char *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;

	MyBackendType = B_IO_WORKER;

	/* TODO review all signals */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, die);		/* to allow manually triggering worker restart */

	/*
	 * Ignore SIGTERM, will get explicit shutdown via SIGUSR2 later in the
	 * shutdown sequence, similar to checkpointer.
	 */
	pqsignal(SIGTERM, SIG_IGN);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SignalHandlerForShutdownRequest);
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/* see PostgresMain() */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		error_context_stack = NULL;
		HOLD_INTERRUPTS();

		/*
		 * We normally shouldn't get errors here. Need to do just enough error
		 * recovery so that we can mark the IO as failed and then exit.
		 */
		LWLockReleaseAll();

		/* TODO: recover from IO errors */

		EmitErrorReport();
		proc_exit(1);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	while (!ShutdownRequestPending)
	{
		WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, -1,
				  WAIT_EVENT_IO_WORKER_MAIN);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}

	proc_exit(0);
}
