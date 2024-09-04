/*-------------------------------------------------------------------------
 *
 * method_worker.c
 *    AIO implementation using workers
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
 *	  src/backend/storage/aio/method_worker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "postmaster/auxprocess.h"
#include "postmaster/interrupt.h"
#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/io_worker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/wait_event.h"
#include "utils/ps_status.h"


/* How many workers should each worker wake up if needed? */
#define IO_WORKER_WAKEUP_FANOUT 2


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
	bool		in_use;
} AioWorkerSlot;

typedef struct AioWorkerControl
{
	uint64		idle_worker_mask;
	AioWorkerSlot workers[FLEXIBLE_ARRAY_MEMBER];
} AioWorkerControl;


static size_t pgaio_worker_shmem_size(void);
static void pgaio_worker_shmem_init(bool first_time);
static void pgaio_worker_postmaster_child_init_local(void);

static bool pgaio_worker_needs_synchronous_execution(PgAioHandle *ioh);
static int	pgaio_worker_submit(uint16 num_staged_ios, PgAioHandle **staged_ios);


const IoMethodOps pgaio_worker_ops = {
	.shmem_size = pgaio_worker_shmem_size,
	.shmem_init = pgaio_worker_shmem_init,
	.postmaster_child_init_local = pgaio_worker_postmaster_child_init_local,

	.needs_synchronous_execution = pgaio_worker_needs_synchronous_execution,
	.submit = pgaio_worker_submit,
#if 0
	.wait_one = pgaio_worker_wait_one,
	.retry = pgaio_worker_io_retry,
	.drain = pgaio_worker_drain,
#endif

	.can_scatter_gather_direct = true,
	.can_scatter_gather_buffered = true
};


int			io_workers = 3;
static int	io_worker_queue_size = 64;

static int	MyIoWorkerId;


static AioWorkerSubmissionQueue *io_worker_submission_queue;
static AioWorkerControl *io_worker_control;


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
	bool		found;
	int			size;

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
		{
			io_worker_control->workers[i].latch = NULL;
			io_worker_control->workers[i].in_use = false;
		}
	}
}

static void
pgaio_worker_postmaster_child_init_local(void)
{
}


static int
pgaio_choose_idle_worker(void)
{
	int			worker;

	if (io_worker_control->idle_worker_mask == 0)
		return -1;

	/* Find the lowest bit position, and clear it. */
	worker = pg_rightmost_one_pos64(io_worker_control->idle_worker_mask);
	io_worker_control->idle_worker_mask &= ~(UINT64_C(1) << worker);

	return worker;
}

static bool
pgaio_worker_submission_queue_insert(PgAioHandle *ioh)
{
	AioWorkerSubmissionQueue *queue;
	uint32		new_head;

	queue = io_worker_submission_queue;
	new_head = (queue->head + 1) & (queue->size - 1);
	if (new_head == queue->tail)
	{
		elog(DEBUG1, "full");
		return false;			/* full */
	}

	queue->ios[queue->head] = pgaio_io_get_id(ioh);
	queue->head = new_head;

	return true;
}

static uint32
pgaio_worker_submission_queue_consume(void)
{
	AioWorkerSubmissionQueue *queue;
	uint32		result;

	queue = io_worker_submission_queue;
	if (queue->tail == queue->head)
		return UINT32_MAX;		/* empty */

	result = queue->ios[queue->tail];
	queue->tail = (queue->tail + 1) & (queue->size - 1);

	return result;
}

static uint32
pgaio_worker_submission_queue_depth(void)
{
	uint32		head;
	uint32		tail;

	head = io_worker_submission_queue->head;
	tail = io_worker_submission_queue->tail;

	if (tail > head)
		head += io_worker_submission_queue->size;

	Assert(head >= tail);

	return head - tail;
}

static void
pgaio_worker_submit_internal(int nios, PgAioHandle *ios[])
{
	PgAioHandle *synchronous_ios[PGAIO_SUBMIT_BATCH_SIZE];
	int			nsync = 0;
	Latch	   *wakeup = NULL;
	int			worker;

	Assert(nios <= PGAIO_SUBMIT_BATCH_SIZE);

	LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
	for (int i = 0; i < nios; ++i)
	{
		Assert(!pgaio_worker_needs_synchronous_execution(ios[i]));
		if (!pgaio_worker_submission_queue_insert(ios[i]))
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

			ereport(DEBUG3,
					errmsg("submission for io:%d choosing worker %d, latch %p",
						   pgaio_io_get_id(ios[i]), worker, wakeup),
					errhidestmt(true), errhidecontext(true));
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
			pgaio_io_perform_synchronously(synchronous_ios[i]);
		}
	}
}

static bool
pgaio_worker_needs_synchronous_execution(PgAioHandle *ioh)
{
	return
		!IsUnderPostmaster
		|| ioh->flags & AHF_REFERENCES_LOCAL
		|| !pgaio_io_can_reopen(ioh);
}

static int
pgaio_worker_submit(uint16 num_staged_ios, PgAioHandle **staged_ios)
{
	for (int i = 0; i < num_staged_ios; i++)
	{
		PgAioHandle *ioh = staged_ios[i];

		pgaio_io_prepare_submit(ioh);
	}

	pgaio_worker_submit_internal(num_staged_ios, staged_ios);

	return num_staged_ios;
}

void
IoWorkerMain(char *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	volatile PgAioHandle *ioh = NULL;
	char		cmd[128];

	MyBackendType = B_IO_WORKER;
	AuxiliaryProcessMainCommon();

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

	/* FIXME: locking */
	MyIoWorkerId = -1;

	LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);

	for (int i = 0; i < io_workers; ++i)
	{
		if (!io_worker_control->workers[i].in_use)
		{
			Assert(io_worker_control->workers[i].latch == NULL);
			io_worker_control->workers[i].in_use = true;
			MyIoWorkerId = i;
			break;
		}
		else
			Assert(io_worker_control->workers[i].latch != NULL);
	}

	if (MyIoWorkerId == -1)
		elog(ERROR, "couldn't find a free worker slot");

	io_worker_control->idle_worker_mask |= (UINT64_C(1) << MyIoWorkerId);
	io_worker_control->workers[MyIoWorkerId].latch = MyLatch;
	LWLockRelease(AioWorkerSubmissionQueueLock);

	sprintf(cmd, "worker: %d", MyIoWorkerId);
	set_ps_display(cmd);

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
		if (ioh != NULL)
		{
#if 0
			/* EINTR is treated as a retryable error */
			pgaio_process_io_completion(unvolatize(PgAioInProgress *, io),
										EINTR);
#endif
		}

		EmitErrorReport();

		/* FIXME: should probably be a before-shmem-exit instead */
		LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
		Assert(io_worker_control->workers[MyIoWorkerId].in_use);
		Assert(io_worker_control->workers[MyIoWorkerId].latch == MyLatch);

		io_worker_control->workers[MyIoWorkerId].in_use = false;
		io_worker_control->workers[MyIoWorkerId].latch = NULL;
		LWLockRelease(AioWorkerSubmissionQueueLock);

		proc_exit(1);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	while (!ShutdownRequestPending)
	{
		uint32		io_index;
		Latch	   *latches[IO_WORKER_WAKEUP_FANOUT];
		int			nlatches = 0;
		int			nwakeups = 0;
		int			worker;

		/* Try to get a job to do. */
		LWLockAcquire(AioWorkerSubmissionQueueLock, LW_EXCLUSIVE);
		if ((io_index = pgaio_worker_submission_queue_consume()) == UINT32_MAX)
		{
			/* Nothing to do.  Mark self idle. */
			/*
			 * XXX: Invent some kind of back pressure to reduce useless
			 * wakeups?
			 */
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
#if 0
			if (nwakeups > 0)
				elog(LOG, "wake %d", nwakeups);
#endif
		}
		LWLockRelease(AioWorkerSubmissionQueueLock);

		for (int i = 0; i < nlatches; ++i)
			SetLatch(latches[i]);

		if (io_index != UINT32_MAX)
		{
			ioh = &aio_ctl->io_handles[io_index];

			ereport(DEBUG3,
					errmsg("worker processing io:%d",
						   pgaio_io_get_id(unvolatize(PgAioHandle *, ioh))),
					errhidestmt(true), errhidecontext(true));

			pgaio_io_reopen(unvolatize(PgAioHandle *, ioh));
			pgaio_io_perform_synchronously(unvolatize(PgAioHandle *, ioh));

			ioh = NULL;
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
	Assert(io_worker_control->workers[MyIoWorkerId].in_use);
	Assert(io_worker_control->workers[MyIoWorkerId].latch == MyLatch);

	io_worker_control->workers[MyIoWorkerId].in_use = false;
	io_worker_control->workers[MyIoWorkerId].latch = NULL;
	LWLockRelease(AioWorkerSubmissionQueueLock);

	proc_exit(0);
}
