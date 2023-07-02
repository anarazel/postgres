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
 * So that the submitter can make just one system call when submitting a batch
 * of IOs, wakeups "fan out"; each woken backend can wake two more.  XXX This
 * could be improved by using futexes instead of latches to wake N waiters.
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
#include "port/pg_bitutils.h"
#include "postmaster/interrupt.h"
#include "storage/aio_internal.h"
#include "storage/condition_variable.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "tcop/tcopprot.h"


/* GUCs */
int io_worker_queue_size;
int io_workers;

int MyIoWorkerId;

/* How many workers should each worker wake up if needed? */
#define IO_WORKER_WAKEUP_FANOUT 2

/* Entry points for IoMethodOps. */
static size_t pgaio_worker_shmem_size(void);
static void pgaio_worker_shmem_init(bool first_time);
static int pgaio_worker_submit(int max_submit, bool drain);
static void pgaio_worker_io_retry(PgAioInProgress *io);

typedef struct AioWorkerSubmissionQueue
{
	uint32		size;
	uint32		mask;
	uint32		head;
	uint32		tail;
	uint32		ios[FLEXIBLE_ARRAY_MEMBER];
} AioWorkerSubmissionQueue;

typedef struct AioWorkerSlot
{
	Latch	   *latch;
} AioWorkerSlot;

typedef struct AioWorkerControl
{
	uint64		idle_worker_mask;
	AioWorkerSlot workers[FLEXIBLE_ARRAY_MEMBER];
} AioWorkerControl;

StaticAssertDecl(sizeof(((AioWorkerControl *)0)->idle_worker_mask) * CHAR_BIT >= MAX_IO_WORKERS,
				 "idle_worker_mask not wide enough");

static AioWorkerSubmissionQueue *io_worker_submission_queue;
static AioWorkerControl *io_worker_control;

const IoMethodOps pgaio_worker_ops = {
	.shmem_size = pgaio_worker_shmem_size,
	.shmem_init = pgaio_worker_shmem_init,
	.submit = pgaio_worker_submit,
	.retry = pgaio_worker_io_retry,

	/*
	 * We may not have true scatter/gather on this platform (see fallback
	 * emulation in pg_preadv()/pg_pwritev() which does a loop of single
	 * segment IOs), but there may still be some advantage to keeping
	 * sequential regions within the same process in buffered mode due to
	 * kernel readahead, so we'll say yes in that case.  That does not apply
	 * to direct I/O, so we'll only say yes here if we have real support.
	 *
	 * XXX For Windows in worker mode with direct IO on, we could teach our
	 * pg_readv(), pg_writev() functions to use scatter gather and wait.
	 */
#if defined(HAVE_PREADV) && defined(HAVE_PWRITEV)
	.can_scatter_gather_direct = true,
#endif
	.can_scatter_gather_buffered = true
};

static size_t
pgaio_worker_shmem_size(void)
{
	return
		offsetof(AioWorkerSubmissionQueue, ios) +
		sizeof(uint32) * io_worker_queue_size +
		offsetof(AioWorkerControl, workers) +
		sizeof(AioWorkerSlot) * io_workers;
}

static void
pgaio_worker_shmem_init(bool first_time)
{
	bool found;
	int size;

	/* Round size up to next power of two so we can make a mask. */
	size = pg_nextpower2_32(io_worker_queue_size);

	io_worker_submission_queue =
		ShmemInitStruct("AioWorkerSubmissionQueue",
						offsetof(AioWorkerSubmissionQueue, ios) +
						sizeof(uint32) * size,
						&found);
	if (!found)
	{
		io_worker_submission_queue->size = size;
		io_worker_submission_queue->head = 0;
		io_worker_submission_queue->tail = 0;
	}

	io_worker_control =
		ShmemInitStruct("AioWorkerControl",
						offsetof(AioWorkerControl, workers) +
						sizeof(AioWorkerSlot) * io_workers,
						&found);
	if (!found)
	{
		io_worker_control->idle_worker_mask = 0;
		for (int i = 0; i < io_workers; ++i)
			io_worker_control->workers[i].latch = NULL;
	}
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

static int
pgaio_choose_idle_worker(void)
{
	int worker;

	if (io_worker_control->idle_worker_mask == 0)
		return -1;

	/* Find the lowest bit position, and clear it. */
	worker = pg_rightmost_one_pos64(io_worker_control->idle_worker_mask);
	io_worker_control->idle_worker_mask &= ~(UINT64_C(1) << worker);

	return worker;
};

static bool
pgaio_worker_submission_queue_insert(PgAioInProgress *io)
{
	AioWorkerSubmissionQueue *queue;
	uint32 new_head;

	queue = io_worker_submission_queue;
	new_head = (queue->head + 1) & (queue->size - 1);
	if (new_head == queue->tail)
		return false;		/* full */

	queue->ios[queue->head] = pgaio_io_id(io);
	queue->head = new_head;

	return true;
}

static uint32
pgaio_worker_submission_queue_consume(void)
{
	AioWorkerSubmissionQueue *queue;
	uint32 result;

	queue = io_worker_submission_queue;
	if (queue->tail == queue->head)
		return UINT32_MAX;	/* empty */

	result = queue->ios[queue->tail];
	queue->tail = (queue->tail + 1) & (queue->size - 1);

	return result;
}

static uint32
pgaio_worker_submission_queue_depth(void)
{
	uint32 head;
	uint32 tail;

	head = io_worker_submission_queue->head;
	tail = io_worker_submission_queue->tail;

	if (tail > head)
		head += io_worker_submission_queue->size;

	Assert(head >= tail);

	return head - tail;
}

static void
pgaio_worker_submit_internal(PgAioInProgress *ios[], int nios)
{
	PgAioInProgress *synchronous_ios[PGAIO_SUBMIT_BATCH_SIZE];
	int nsync = 0;
	Latch *wakeup = NULL;
	int worker;

	Assert(nios <= PGAIO_SUBMIT_BATCH_SIZE);

	LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
	for (int i = 0; i < nios; ++i)
	{
		if (pgaio_worker_need_synchronous(ios[i]) ||
			!pgaio_worker_submission_queue_insert(ios[i]))
		{
			/*
			 * We'll do it synchronously, but only after we've sent as many as
			 * we can to workers, to maximize concurrency.
			 */
			synchronous_ios[nsync++] = ios[i];
			continue;
		}

		if (wakeup == NULL)
		{
			/* Choose an idle worker to wake up if we haven't already. */
			worker = pgaio_choose_idle_worker();
			if (worker >= 0)
				wakeup = io_worker_control->workers[worker].latch;
		}
	}
	LWLockRelease(AioWorkerSubmissionQueueLock);

	if (wakeup)
		SetLatch(wakeup);

	/* Run whatever is left synchronously. */
	if (nsync > 0)
	{
		for (int i = 0; i < nsync; ++i)
		{
			pgaio_do_synchronously(synchronous_ios[i]);
			pgaio_complete_ios(false);
		}
	}
}

static int
pgaio_worker_submit(int max_submit, bool drain)
{
	PgAioInProgress *ios[PGAIO_SUBMIT_BATCH_SIZE];
	int nios = 0;

	while (!dlist_is_empty(&my_aio->pending) && nios < max_submit)
	{
		dlist_node *node;
		PgAioInProgress *io;

		node = dlist_head_node(&my_aio->pending);
		io = dlist_container(PgAioInProgress, io_node, node);

		pgaio_io_prepare_submit(io, io->ring);

		Assert(nios < PGAIO_SUBMIT_BATCH_SIZE);

		ios[nios++] = io;
	}

	pgaio_worker_submit_internal(ios, nios);

	return nios;
}

static void
pgaio_worker_io_retry(PgAioInProgress *io)
{
	WRITE_ONCE_F(io->flags) |= PGAIOIP_INFLIGHT;

	pgaio_worker_submit_internal(&io, 1);
}

void
IoWorkerMain(void)
{
	volatile PgAioInProgress *io = NULL;
	sigjmp_buf	local_sigjmp_buf;

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
	pqsignal(SIGUSR2, SignalHandlerForShutdownRequest);
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
	io_worker_control->idle_worker_mask |= (UINT64_C(1) << MyIoWorkerId);
	io_worker_control->workers[MyIoWorkerId].latch = MyLatch;
	LWLockRelease(AioWorkerSubmissionQueueLock);

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

		if (io != NULL)
		{
			/* EINTR is treated as a retryable error */
			pgaio_process_io_completion(unvolatize(PgAioInProgress *, io),
										EINTR);
		}

		EmitErrorReport();
		proc_exit(1);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	while (!ShutdownRequestPending)
	{
		uint32 io_index;
		Latch *latches[IO_WORKER_WAKEUP_FANOUT];
		int nlatches = 0;
		int nwakeups = 0;
		int worker;

		/* Try to get a job to do. */
		LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
		if ((io_index = pgaio_worker_submission_queue_consume()) == UINT32_MAX)
		{
			/* Nothing to do.  Mark self idle. */
			/* XXX: Invent some kind of back pressure to reduce useless wakeups? */
			io_worker_control->idle_worker_mask |= (UINT64_C(1) << MyIoWorkerId);
		}
		else
		{
			/* Got one.  Clear idle flag. */
			io_worker_control->idle_worker_mask &= ~(UINT64_C(1) << MyIoWorkerId);

			/* See if we can wake up some peers. */
			nwakeups = Min(pgaio_worker_submission_queue_depth(),
						   IO_WORKER_WAKEUP_FANOUT);
			for (int i = 0; i < nwakeups; ++i)
			{
				if ((worker = pgaio_choose_idle_worker()) < 0)
					break;
				latches[nlatches++] = io_worker_control->workers[worker].latch;
			}
		}
		LWLockRelease(AioWorkerSubmissionQueueLock);

		for (int i = 0; i < nlatches; ++i)
			SetLatch(latches[i]);

		if (io_index != UINT32_MAX)
		{
			io = &aio_ctl->in_progress_io[io_index];

			pgaio_io_call_shared_open(unvolatize(PgAioInProgress *, io));
			pgaio_do_synchronously(unvolatize(PgAioInProgress *, io));
			pgaio_complete_ios(false);
			io = NULL;
		}
		else
		{
			WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, -1,
					  WAIT_EVENT_IO_WORKER_MAIN);
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}

	LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
	io_worker_control->idle_worker_mask &= ~(UINT64_C(1) << MyIoWorkerId);
	io_worker_control->workers[MyIoWorkerId].latch = NULL;
	LWLockRelease(AioWorkerSubmissionQueueLock);
}
