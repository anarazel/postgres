/*-------------------------------------------------------------------------
 *
 * aio_worker.c
 *	  Routines for worker-based asynchronous I/O.
 *
 * Worker processes consume IOs from a shared memory submission queue, run
 * traditional synchronous system calls, and perform the shared completion
 * handling immediately.  Client code submits most requests by pushing IOs
 * into the submission queue, and waits (if necessary) using condition
 * variables.  Some IOs cannot be performed in another process due to lack of
 * infrastructure for reopening the file, and must processed synchronously by
 * the client code when submitted.
 *
 * This method of AIO is available in all builds on all operating systems, and
 * is the default.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/aio_worker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "storage/aio_internal.h"
#include "storage/condition_variable.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"


/* GUCs */
int io_worker_queue_size;
int io_workers;

int MyIoWorkerId;


Size
AioWorkerShmemSize(void)
{
	return squeue32_estimate(io_worker_queue_size);
}

void
AioWorkerShmemInit(void)
{
	bool found;

	aio_ctl->worker_submission_queue = (squeue32 *)
		ShmemInitStruct("AioWorkerSubmissionQueue", AioWorkerShmemSize(),
						&found);
	Assert(!found);
	squeue32_init(aio_ctl->worker_submission_queue, io_worker_queue_size);
	ConditionVariableInit(&aio_ctl->worker_submission_queue_not_empty);
}

static bool
pgaio_worker_need_synchronous(PgAioInProgress *io)
{
	/* Single user mode doesn't have any AIO workers. */
	if (!IsUnderPostmaster)
		return true;

	/* Not all IOs support the file being (re)opened by a worker. */
	return !pgaio_io_has_shared_open(io);
}

static void
pgaio_worker_submit_one(PgAioInProgress *io)
{
	uint32 io_index;

	io_index = io - aio_ctl->in_progress_io;

	if (pgaio_worker_need_synchronous(io))
	{
		/* Perform the IO synchronously in this process. */
		pgaio_do_synchronously(io);
	}
	else
	{
		/*
		 * Push it on the submission queue and wake a worker, but if the
		 * queue is full then handle it synchronously rather than waiting.
		 * XXX Is this fair enough?  XXX Write a smarter work distributor.
		 */
		if (squeue32_enqueue(aio_ctl->worker_submission_queue, io_index))
			ConditionVariableSignal(&aio_ctl->worker_submission_queue_not_empty);
		else
			pgaio_do_synchronously(io);
	}
	pgaio_complete_ios(false);
}

int
pgaio_worker_submit(int max_submit, bool drain)
{
	int nios = 0;

	while (!dlist_is_empty(&my_aio->pending) && nios < max_submit)
	{
		dlist_node *node;
		PgAioInProgress *io;

		node = dlist_head_node(&my_aio->pending);
		io = dlist_container(PgAioInProgress, io_node, node);

		pgaio_io_prepare_submit(io, io->ring);

		pgaio_worker_submit_one(io);
		++nios;
	}

	return nios;
}

void
pgaio_worker_io_retry(PgAioInProgress *io)
{
	WRITE_ONCE_F(io->flags) |= PGAIOIP_INFLIGHT;

	pgaio_worker_submit_one(io);
}

void
IoWorkerMain(void)
{
	/* TODO review all signals */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, die); /* to allow manually triggering worker restart */
	/*
	 * Ignore SIGTERM, will get explicit shutdown via SIGUSR2 later in the
	 * shutdown sequence, similar to checkpointer.
	 */
	pqsignal(SIGTERM, SIG_IGN);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, die);
	PG_SETMASK(&UnBlockSig);

	for (;;)
	{
		uint32 io_index;

		if (squeue32_dequeue(aio_ctl->worker_submission_queue, &io_index))
		{
			PgAioInProgress *io = &aio_ctl->in_progress_io[io_index];

			/* Do IO and completions.  This'll signal anyone waiting. */
			/*
			 * XXX I think we might need this to be in a critical section, so
			 * that we can't lose track of an IO without taking the system
			 * down.  But we're not allowed to, because the IO handler might
			 * reach smgropen() which allocates.
			 */
			ConditionVariableCancelSleep();

			pgaio_io_call_shared_open(io);
			pgaio_do_synchronously(io);
			pgaio_complete_ios(false);
		}
		else
		{
			/* Nothing in the queue.  Go to sleep. */
			ConditionVariableSleep(&aio_ctl->worker_submission_queue_not_empty,
								   WAIT_EVENT_IO_WORKER_MAIN);
		}
	}
}
