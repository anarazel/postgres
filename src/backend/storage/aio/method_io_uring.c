/*-------------------------------------------------------------------------
 *
 * method_io_uring.c
 *    AIO implementation using io_uring on Linux
 *
 * XXX Write me
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/method_io_uring.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef USE_LIBURING

#include <liburing.h>

#include "pgstat.h"
#include "port/pg_iovec.h"
#include "storage/aio_internal.h"
#include "storage/fd.h"
#include "storage/proc.h"
#include "storage/shmem.h"


/* Entry points for IoMethodOps. */
static size_t pgaio_uring_shmem_size(void);
static void pgaio_uring_postmaster_init(void);
static void pgaio_uring_shmem_init(bool first_time);
static void pgaio_uring_postmaster_child_init(void);
static void pgaio_uring_postmaster_child_init_local(void);

static int	pgaio_uring_submit(uint16 num_staged_ios, PgAioHandle **staged_ios);
static void pgaio_uring_wait_one(PgAioHandle *ioh, uint64 ref_generation);

static void pgaio_uring_sq_from_io(PgAioHandle *ioh, struct io_uring_sqe *sqe);


const IoMethodOps pgaio_uring_ops = {
	.shmem_size = pgaio_uring_shmem_size,
	.shmem_init = pgaio_uring_shmem_init,
	.postmaster_init = pgaio_uring_postmaster_init,
	.postmaster_child_init = pgaio_uring_postmaster_child_init,
	.postmaster_child_init_local = pgaio_uring_postmaster_child_init_local,

	.submit = pgaio_uring_submit,
	.wait_one = pgaio_uring_wait_one,
#if 0
	.retry = pgaio_uring_io_retry,
	.wait_one = pgaio_uring_wait_one,
	.drain = pgaio_uring_drain,
#endif
	.can_scatter_gather_direct = true,
	.can_scatter_gather_buffered = true
};

typedef struct PgAioUringContext
{
	LWLock		completion_lock;

	struct io_uring io_uring_ring;
	/* XXX: probably worth padding to a cacheline boundary here */
} PgAioUringContext;


static PgAioUringContext *aio_uring_contexts;
static PgAioUringContext *my_shared_uring_context;

/* io_uring local state */
static struct io_uring local_ring;



static Size
AioContextShmemSize(void)
{
	uint32		TotalProcs = MaxBackends + NUM_AUXILIARY_PROCS - MAX_IO_WORKERS;

	return mul_size(TotalProcs, sizeof(PgAioUringContext));
}

static size_t
pgaio_uring_shmem_size(void)
{
	return AioContextShmemSize();
}

static void
pgaio_uring_shmem_init(bool first_time)
{
	uint32		TotalProcs = MaxBackends + NUM_AUXILIARY_PROCS - MAX_IO_WORKERS;
	bool		found;

	aio_uring_contexts = (PgAioUringContext *)
		ShmemInitStruct("AioUring", pgaio_uring_shmem_size(), &found);

	if (found)
		return;

	for (int contextno = 0; contextno < TotalProcs; contextno++)
	{
		PgAioUringContext *context = &aio_uring_contexts[contextno];
		int			ret;

		/*
		 * XXX: Probably worth sharing the WQ between the different rings,
		 * when supported by the kernel. Could also cause additional
		 * contention, I guess?
		 */
#if 0
		if (!AcquireExternalFD())
			elog(ERROR, "No external FD available");
#endif
		ret = io_uring_queue_init(io_max_concurrency, &context->io_uring_ring, 0);
		if (ret < 0)
			elog(ERROR, "io_uring_queue_init failed: %s", strerror(-ret));

		LWLockInitialize(&context->completion_lock, LWTRANCHE_AIO_URING_COMPLETION);
	}
}

static void
pgaio_uring_postmaster_init(void)
{
	uint32		TotalProcs =
		MaxBackends + NUM_AUXILIARY_PROCS - MAX_IO_WORKERS;

	for (int i = 0; i < TotalProcs; i++)
		ReserveExternalFD();
}

static void
pgaio_uring_postmaster_child_init(void)
{
	my_shared_uring_context = &aio_uring_contexts[MyProcNumber];
}

static void
pgaio_uring_postmaster_child_init_local(void)
{
	int			ret;

	ret = io_uring_queue_init(32, &local_ring, 0);
	if (ret < 0)
		elog(ERROR, "io_uring_queue_init failed: %s", strerror(-ret));
}

static int
pgaio_uring_submit(uint16 num_staged_ios, PgAioHandle **staged_ios)
{
	struct io_uring *uring_instance = &my_shared_uring_context->io_uring_ring;

	Assert(num_staged_ios <= PGAIO_SUBMIT_BATCH_SIZE);

	for (int i = 0; i < num_staged_ios; i++)
	{
		PgAioHandle *ioh = staged_ios[i];
		struct io_uring_sqe *sqe;

		sqe = io_uring_get_sqe(uring_instance);

		pgaio_io_prepare_submit(ioh);
		pgaio_uring_sq_from_io(ioh, sqe);
	}

	while (true)
	{
		int			ret;

		pgstat_report_wait_start(WAIT_EVENT_AIO_SUBMIT);
		ret = io_uring_submit(uring_instance);
		pgstat_report_wait_end();

		if (ret == -EINTR)
		{
			elog(DEBUG3, "submit EINTR, nios: %d", num_staged_ios);
			continue;
		}
		if (ret < 0)
			elog(PANIC, "failed: %d/%s",
				 ret, strerror(-ret));
		else if (ret != num_staged_ios)
		{
			/* likely unreachable, but if it is, we would need to re-submit */
			elog(PANIC, "submitted only %d of %d",
				 ret, num_staged_ios);
		}
		else
		{
			elog(DEBUG3, "submit nios: %d", num_staged_ios);
		}
		break;
	}

	return num_staged_ios;
}


#define PGAIO_MAX_LOCAL_REAPED 16

static void
pgaio_uring_drain_locked(PgAioUringContext *context)
{
	int			ready;
	int			orig_ready;

	/*
	 * Don't drain more events than available right now. Otherwise it's
	 * plausible that one backend could get stuck, for a while, receiving CQEs
	 * without actually processing them.
	 */
	orig_ready = ready = io_uring_cq_ready(&context->io_uring_ring);

	while (ready > 0)
	{
		struct io_uring_cqe *reaped_cqes[PGAIO_MAX_LOCAL_REAPED];
		uint32		reaped;

		START_CRIT_SECTION();
		reaped =
			io_uring_peek_batch_cqe(&context->io_uring_ring,
									reaped_cqes,
									Min(PGAIO_MAX_LOCAL_REAPED, ready));
		Assert(reaped <= ready);

		ready -= reaped;

		for (int i = 0; i < reaped; i++)
		{
			struct io_uring_cqe *cqe = reaped_cqes[i];
			PgAioHandle *ioh;

			ioh = io_uring_cqe_get_data(cqe);
			io_uring_cqe_seen(&context->io_uring_ring, cqe);

			pgaio_io_process_completion(ioh, cqe->res);
		}

		END_CRIT_SECTION();

		ereport(DEBUG3,
				errmsg("drained %d/%d, now expecting %d",
					   reaped, orig_ready, io_uring_cq_ready(&context->io_uring_ring)),
				errhidestmt(true),
				errhidecontext(true));

	}
}

static void
pgaio_uring_wait_one(PgAioHandle *ioh, uint64 ref_generation)
{
	PgAioHandleState state;
	ProcNumber	owner_procno = ioh->owner_procno;
	PgAioUringContext *owner_context = &aio_uring_contexts[owner_procno];
	bool		expect_cqe;
	int			waited = 0;

	/*
	 * We ought to have a smarter locking scheme, nearly all the time the
	 * backend owning the ring will reap the completions, making the locking
	 * unnecessarily expensive.
	 */
	LWLockAcquire(&owner_context->completion_lock, LW_EXCLUSIVE);

	while (true)
	{
		ereport(DEBUG3,
				errmsg("wait_one for io:%d in state %s, cycle %d",
					   pgaio_io_get_id(ioh), pgaio_io_get_state_name(ioh), waited),
				errhidestmt(true),
				errhidecontext(true));

		if (pgaio_io_was_recycled(ioh, ref_generation, &state) ||
			state != AHS_IN_FLIGHT)
		{
			break;
		}
		else if (io_uring_cq_ready(&owner_context->io_uring_ring))
		{
			expect_cqe = true;
		}
		else
		{
			int			ret;
			struct io_uring_cqe *cqes;

			pgstat_report_wait_start(WAIT_EVENT_AIO_DRAIN);
			ret = io_uring_wait_cqes(&owner_context->io_uring_ring, &cqes, 1, NULL, NULL);
			pgstat_report_wait_end();

			if (ret == -EINTR)
			{
				continue;
			}
			else if (ret != 0)
			{
				elog(PANIC, "unexpected: %d/%s: %m", ret, strerror(-ret));
			}
			else
			{
				Assert(cqes != NULL);
				expect_cqe = true;
				waited++;
			}
		}

		if (expect_cqe)
		{
			pgaio_uring_drain_locked(owner_context);
		}
	}

	LWLockRelease(&owner_context->completion_lock);

	ereport(DEBUG3,
			errmsg("wait_one with %d sleeps",
				   waited),
			errhidestmt(true),
			errhidecontext(true));
}

static void
pgaio_uring_sq_from_io(PgAioHandle *ioh, struct io_uring_sqe *sqe)
{
	struct iovec *iov;

	switch (ioh->op)
	{
		case PGAIO_OP_READ:
			iov = &aio_ctl->iovecs[ioh->iovec_off];
			if (ioh->op_data.read.iov_length == 1)
			{
				io_uring_prep_read(sqe,
								   ioh->op_data.read.fd,
								   iov->iov_base,
								   iov->iov_len,
								   ioh->op_data.read.offset);
			}
			else
			{
				io_uring_prep_readv(sqe,
									ioh->op_data.read.fd,
									iov,
									ioh->op_data.read.iov_length,
									ioh->op_data.read.offset);

			}
			break;

		case PGAIO_OP_WRITE:
			iov = &aio_ctl->iovecs[ioh->iovec_off];
			if (ioh->op_data.write.iov_length == 1)
			{
				io_uring_prep_write(sqe,
									ioh->op_data.write.fd,
									iov->iov_base,
									iov->iov_len,
									ioh->op_data.write.offset);
			}
			else
			{
				io_uring_prep_writev(sqe,
									 ioh->op_data.write.fd,
									 iov,
									 ioh->op_data.write.iov_length,
									 ioh->op_data.write.offset);
			}
			break;

		default:
			elog(ERROR, "not implemented");
	}

	io_uring_sqe_set_data(sqe, ioh);
}

#endif							/* USE_LIBURING */
