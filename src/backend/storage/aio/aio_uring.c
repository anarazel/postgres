/*-------------------------------------------------------------------------
 *
 * aio_uring.c
 *	  Routines for Linux io_uring.
 *
 * XXX Write me
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/aio_uring.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <liburing.h>

#include "pgstat.h"
#include "storage/aio_internal.h"
#include "storage/fd.h"
#include "storage/proc.h"
#include "storage/shmem.h"


static void pgaio_uring_sq_from_io(PgAioContext *context, PgAioInProgress *io, struct io_uring_sqe *sqe);
static void pgaio_uring_io_from_cqe(PgAioContext *context, struct io_uring_cqe *cqe);
static void pgaio_uring_iovec_transfer(PgAioContext *context);

/* Entry points for IoMethodOps. */
static size_t pgaio_uring_shmem_size(void);
static void pgaio_uring_shmem_init(bool first_time);
static void pgaio_uring_postmaster_child_init_local(void);
static int pgaio_uring_submit(int max_submit, bool drain);
static void pgaio_uring_wait_one(PgAioContext *context, PgAioInProgress *io, uint64 ref_generation, uint32 wait_event_info);
static void pgaio_uring_io_retry(PgAioInProgress *io);
static int pgaio_uring_drain(PgAioContext *context, bool block, bool call_shared);

/* io_uring local state */
struct io_uring local_ring;

const IoMethodOps pgaio_uring_ops = {
	.shmem_size = pgaio_uring_shmem_size,
	.shmem_init = pgaio_uring_shmem_init,
	.postmaster_child_init_local = pgaio_uring_postmaster_child_init_local,
	.submit = pgaio_uring_submit,
	.retry = pgaio_uring_io_retry,
	.wait_one = pgaio_uring_wait_one,
	.drain = pgaio_uring_drain,
	.can_scatter_gather_direct = true,
	.can_scatter_gather_buffered = true
};

static Size
AioContextShmemSize(void)
{
	return mul_size(PGAIO_NUM_CONTEXTS, sizeof(PgAioContext));
}

static Size
AioContextIovecsShmemSize(void)
{
	return mul_size(PGAIO_NUM_CONTEXTS,
					mul_size(sizeof(PgAioIovec), max_aio_in_flight));
}

static size_t
pgaio_uring_shmem_size(void)
{
	return add_size(AioContextShmemSize(), AioContextIovecsShmemSize());
}

static void
pgaio_uring_shmem_init(bool first_time)
{
	PgAioIovec *iovecs;
	bool found;

	aio_ctl->num_contexts = PGAIO_NUM_CONTEXTS;
	aio_ctl->contexts = ShmemInitStruct("PgAioContexts", AioContextShmemSize(), &found);
	Assert(!found);

	iovecs = (PgAioIovec *)
		ShmemInitStruct("PgAioContextsIovecs", AioContextIovecsShmemSize(), &found);
	Assert(!found);
	memset(iovecs, 0, AioContextIovecsShmemSize());

	for (int contextno = 0; contextno < aio_ctl->num_contexts; contextno++)
	{
		PgAioContext *context = &aio_ctl->contexts[contextno];
		int ret;

		LWLockInitialize(&context->submission_lock, LWTRANCHE_AIO_CONTEXT_SUBMISSION);
		LWLockInitialize(&context->completion_lock, LWTRANCHE_AIO_CONTEXT_COMPLETION);

		slist_init(&context->unused_iovecs);
		slist_init(&context->reaped_iovecs);

		context->iovecs = iovecs;
		iovecs += max_aio_in_flight;

		for (uint32 i = 0; i < max_aio_in_flight; i++)
		{
			slist_push_head(&context->unused_iovecs, &context->iovecs[i].node);
			context->unused_iovecs_count++;
		}

		/*
		 * XXX: Probably worth sharing the WQ between the different rings,
		 * when supported by the kernel. Could also cause additional
		 * contention, I guess?
		 */
		if (!AcquireExternalFD())
			elog(ERROR, "io_uring_queue_init: %m");
		ret = io_uring_queue_init(max_aio_in_flight, &context->io_uring_ring, 0);
		if (ret < 0)
			elog(ERROR, "io_uring_queue_init failed: %s", strerror(-ret));
	}
}

static void
pgaio_uring_postmaster_child_init_local(void)
{
	int ret;

	ret = io_uring_queue_init(32, &local_ring, 0);
	if (ret < 0)
		elog(ERROR, "io_uring_queue_init failed: %s", strerror(-ret));
}

static PgAioContext *
pgaio_acquire_context(void)
{
	PgAioContext *context;
	int init_last_context = my_aio->last_context;

	/*
	 * First try to acquire a context without blocking on the lock. We start
	 * with the last context we successfully used, which should lead to
	 * backends spreading to different contexts over time.
	 */
	for (int i = 0; i < aio_ctl->num_contexts; i++)
	{
		context = &aio_ctl->contexts[my_aio->last_context];

		if (LWLockConditionalAcquire(&context->submission_lock, LW_EXCLUSIVE))
		{
			return context;
		}

		if (++my_aio->last_context == aio_ctl->num_contexts)
			my_aio->last_context = 0;
	}

	/*
	 * Couldn't acquire any without blocking. Block on the last + 1.;
	 */
	if (++my_aio->last_context == aio_ctl->num_contexts)
		my_aio->last_context = 0;
	context = &aio_ctl->contexts[my_aio->last_context];

	elog(DEBUG2, "blocking acquiring io context %d, started on %d",
		 my_aio->last_context, init_last_context);

	LWLockAcquire(&context->submission_lock, LW_EXCLUSIVE);

	return context;
}

static int
pgaio_uring_submit(int max_submit, bool drain)
{
	PgAioInProgress *ios[PGAIO_SUBMIT_BATCH_SIZE];
	struct io_uring_sqe *sqe[PGAIO_SUBMIT_BATCH_SIZE];
	PgAioContext *context;
	int nios = 0;

	context = pgaio_acquire_context();

	Assert(max_submit != 0 && max_submit <= my_aio->pending_count);

	while (!dlist_is_empty(&my_aio->pending))
	{
		dlist_node *node;
		PgAioInProgress *io;

		if (nios == max_submit)
			break;

		/*
		 * XXX: Should there be a per-ring limit? If so, we'd probably best
		 * apply it here.
		 */

		sqe[nios] = io_uring_get_sqe(&context->io_uring_ring);

		if (!sqe[nios])
		{
			Assert(nios != 0);
			elog(WARNING, "io_uring_get_sqe() returned NULL?");
			break;
		}

		node = dlist_head_node(&my_aio->pending);
		io = dlist_container(PgAioInProgress, io_node, node);
		ios[nios] = io;

		pgaio_io_prepare_submit(io, context - aio_ctl->contexts);

		pgaio_uring_sq_from_io(context, ios[nios], sqe[nios]);

		nios++;
	}

	Assert(nios > 0);

	{
		int ret;

		pg_atomic_add_fetch_u32(&my_aio->inflight_count, nios);
		my_aio->submissions_total_count++;

again:
		pgstat_report_wait_start(WAIT_EVENT_AIO_SUBMIT);
		ret = io_uring_submit(&context->io_uring_ring);
		pgstat_report_wait_end();

		if (ret == -EINTR)
		{
			elog(DEBUG3, "submit EINTR, nios: %d", nios);
			goto again;
		}
		if (ret < 0)
			elog(PANIC, "failed: %d/%s",
				 ret, strerror(-ret));
		else if (ret != nios)
		{
			/* likely unreachable, but if it is, we would need to re-submit */
			elog(PANIC, "submitted only %d of %d",
				 ret, nios);
		}
	}

	LWLockRelease(&context->submission_lock);

	/*
	 * Others might have been waiting for this IO. Because it wasn't
	 * marked as in-flight until now, they might be waiting for the
	 * CV. Wake'em up.
	 */
	pgaio_broadcast_ios(ios, nios);

	/* callbacks will be called later, by pgaio_submit_pending_internal() */
	if (drain)
		pgaio_drain(context,
					/* block = */ false,
					/* call_shared = */ false,
					/* call_local = */ false);

	return nios;
}

static void
pgaio_uring_io_retry(PgAioInProgress *io)
{
	PgAioContext *context;
	struct io_uring_sqe *sqe;

	/*
	 * Need to use the same context as last time - other backends could be
	 * trying to read this IO's completion, potentially wait on the wrong
	 * context. Outside of retries ->generation incrementing protects against
	 * that, but we can't do that for retries.
	 */
	context = &aio_ctl->contexts[io->ring];

	LWLockAcquire(&context->submission_lock, LW_EXCLUSIVE);

	sqe = io_uring_get_sqe(&context->io_uring_ring);
	pgaio_uring_sq_from_io(context, io, sqe);

	WRITE_ONCE_F(io->flags) |= PGAIOIP_INFLIGHT;
	pg_atomic_fetch_add_u32(&aio_ctl->backend_state[io->owner_id].inflight_count, 1);

	while (true)
	{
		int ret;

		pgstat_report_wait_start(WAIT_EVENT_AIO_SUBMIT);
		ret = io_uring_submit(&context->io_uring_ring);
		pgstat_report_wait_end();

		if (likely(ret == 1))
			break;
		else if (ret != -EINTR)
			elog(PANIC, "failed: %d/%s",
				 ret, strerror(-ret));
	}

	LWLockRelease(&context->submission_lock);

	/* see pgaio_uring_submit() */
	ConditionVariableBroadcast(&io->cv);
}

static int
pgaio_uring_drain_locked(PgAioContext *context)
{
	uint32 processed = 0;
	int ready;

	Assert(LWLockHeldByMe(&context->completion_lock));

	/*
	 * Don't drain more events than available right now. Otherwise it's
	 * plausible that one backend could get stuck, for a while, receiving CQEs
	 * without actually processing them.
	 */
	ready = io_uring_cq_ready(&context->io_uring_ring);

	while (ready > 0)
	{
		struct io_uring_cqe *reaped_cqes[PGAIO_MAX_LOCAL_REAPED];
		uint32 processed_one;

		START_CRIT_SECTION();

		ereport(DEBUG3,
				errmsg("context [%zu]: drain %d ready",
					   context - aio_ctl->contexts,
					   ready),
				errhidestmt(true),
				errhidecontext(true));

		processed_one =
			io_uring_peek_batch_cqe(&context->io_uring_ring,
									reaped_cqes,
									Min(PGAIO_MAX_LOCAL_REAPED, ready));
		Assert(processed_one <= ready);

		ready -= processed_one;
		processed += processed_one;

		for (int i = 0; i < processed_one; i++)
		{
			struct io_uring_cqe *cqe = reaped_cqes[i];

			pgaio_uring_io_from_cqe(context, cqe);

			io_uring_cqe_seen(&context->io_uring_ring, cqe);
		}

		END_CRIT_SECTION();
	}

	if (context->reaped_iovecs_count > context->unused_iovecs_count &&
		LWLockConditionalAcquire(&context->submission_lock, LW_EXCLUSIVE))
	{
		ereport(DEBUG4,
				errmsg("plenty reaped iovecs (%d), transferring",
					   context->reaped_iovecs_count),
				errhidestmt(true),
				errhidecontext(true));

		pgaio_uring_iovec_transfer(context);
		LWLockRelease(&context->submission_lock);
	}

	return processed;
}

static LWLockWaitCheckRes
pgaio_uring_completion_check_wait(LWLock *lock, LWLockMode mode, uint64_t cb_data)
{
	PgAioUringWaitRef *wr = (PgAioUringWaitRef*) (uintptr_t) cb_data;

	if (wr->aio == NULL)
	{
		return LW_WAIT_DONE;
	}
	else
	{
		PgAioIPFlags flags;

		if (pgaio_io_recycled(wr->aio, wr->aio->generation, &flags) ||
			!(flags & PGAIOIP_INFLIGHT))
		{
			return LW_WAIT_DONE;
		}
		else
		{
			return LW_WAIT_NEEDS_LOCK;
		}
	}
}

static LWLockWaitCheckRes
pgaio_uring_completion_check_wake(LWLock *lock, LWLockMode mode, struct PGPROC *wakee, uint64_t cb_data)
{
	PgAioUringWaitRef *wr = (PgAioUringWaitRef*) (uintptr_t) wakee->lwWaitData;

	if (!wr || wr->aio == NULL)
		return LW_WAIT_NEEDS_LOCK;
	else
	{
		PgAioIPFlags flags;

		if (pgaio_io_recycled(wr->aio, wr->aio->generation, &flags) ||
			!(flags & PGAIOIP_INFLIGHT))
		{
			return LW_WAIT_DONE;
		}
		else
		{
			return LW_WAIT_NEEDS_LOCK;
		}
	}
}

static int
pgaio_uring_drain(PgAioContext *context, bool block, bool call_shared)
{
	uint32 processed = 0;

	Assert(!LWLockHeldByMe(&context->completion_lock));

	while (io_uring_cq_ready(&context->io_uring_ring))
	{
		if (block)
		{
			Assert(my_aio->wr.context == NULL);
			Assert(my_aio->wr.aio == NULL);
			Assert(my_aio->wr.ref_generation == UINT32_MAX);

			my_aio->wr.context = context;

			if (!LWLockAcquireEx(&context->completion_lock, LW_EXCLUSIVE,
								 pgaio_uring_completion_check_wait,
								 (uint64) (uintptr_t) &my_aio->wr))
			{
				my_aio->wr.context = NULL;
				continue;
			}
		}
		else
		{
			if (!LWLockConditionalAcquire(&context->completion_lock, LW_EXCLUSIVE))
				break;
		}

		processed = pgaio_uring_drain_locked(context);

		/*
		 * If allowed, call shared callbacks under lock - that prevent
		 * other backends to first have to wait on the completion_lock for
		 * pgaio_uring_wait_one, then again below pgaio_drain(), and then
		 * again on the condition variable for the AIO.
		 */
		if (call_shared)
			pgaio_complete_ios(false);

		LWLockReleaseEx(&context->completion_lock,
						pgaio_uring_completion_check_wake, 0);
		my_aio->wr.context = NULL;
		break;
	}

	return processed;
}

static void
pgaio_uring_wait_one(PgAioContext *context, PgAioInProgress *io, uint64 ref_generation, uint32 wait_event_info)
{
	PgAioIPFlags flags;
	int loops = 0;

	Assert(my_aio->wr.context == NULL);
	Assert(my_aio->wr.aio == NULL);
	Assert(my_aio->wr.ref_generation == UINT32_MAX);

	my_aio->wr.context = context;
	my_aio->wr.aio = io;
	my_aio->wr.ref_generation = ref_generation;

uring_wait_one_again:

	if (LWLockAcquireEx(&context->completion_lock, LW_EXCLUSIVE,
						pgaio_uring_completion_check_wait,
						(uint64) (uintptr_t) &my_aio->wr))
	{
		bool expect_cqe = false;

		ereport(DEBUG3,
				errmsg("[%d] got the lock, before waiting for %zu/%llu, %d ready",
					   (int)(context - aio_ctl->contexts),
					   io - aio_ctl->in_progress_io,
					   (long long unsigned) io->generation,
					   io_uring_cq_ready(&context->io_uring_ring)),
				errhidestmt(true),
				errhidecontext(true));

		if (io_uring_cq_ready(&context->io_uring_ring))
		{
			expect_cqe = true;

			ereport(DEBUG3,
					errmsg("[%d] new completions under lock, without wait (for %zu/%llu), %d ready",
						   (int)(context - aio_ctl->contexts),
						   io - aio_ctl->in_progress_io,
						   (long long unsigned) io->generation,
						   io_uring_cq_ready(&context->io_uring_ring)),
					errhidestmt(true),
					errhidecontext(true));

		}
		else if (pgaio_io_recycled(io, ref_generation, &flags) ||
				 !(flags & PGAIOIP_INFLIGHT))
		{
			/*
			 * Cannot set expect_cqe = true, because in the recycled case
			 * there may not be any completion.
			 */
			ereport(DEBUG3,
					errmsg("[%d] recycled or completed under lock, without wait (for %zu/%llu), %d ready",
						   (int)(context - aio_ctl->contexts),
						   io - aio_ctl->in_progress_io,
						   (long long unsigned) io->generation,
						   io_uring_cq_ready(&context->io_uring_ring)),
					errhidestmt(true),
					errhidecontext(true));
		}
		else
		{
			int ret;
			struct io_uring_cqe *cqes;

			/*
			 * XXX: Temporary, non-assert, sanity checks, some of these are
			 * hard to hit in assert builds and lead to rare and hard to debug
			 * hangs.
			 */
			if (io->generation != ref_generation ||
				&aio_ctl->contexts[io->ring] != context)
			{
				ereport(PANIC,
						errmsg("generation/context changed while locked: ref_gen: %llu, real_gen: %llu, orig ring: %d, cur ring: %d",
							   (long long unsigned) ref_generation,
							   (long long unsigned) io->generation,
							   (int)(context - aio_ctl->contexts),
							   io->ring),
						errhidestmt(true),
						errhidecontext(true));
			}

			pgstat_report_wait_start(wait_event_info);
			ret = io_uring_wait_cqes(&context->io_uring_ring, &cqes, 1, NULL, NULL);
			pgstat_report_wait_end();

			if (ret == -EINTR)
			{
				Assert(cqes == NULL);
				elog(DEBUG2, "got interrupted: %d, %m", ret);
			}
			else if (ret != 0)
			{
				Assert(cqes == NULL);
				elog(PANIC, "unexpected: %d/%s: %m", ret, strerror(-ret));
			}
			else
			{
				Assert(cqes != NULL);
				expect_cqe = true;
			}

			/* see XXX above */
			if (io->generation != ref_generation ||
				&aio_ctl->contexts[io->ring] != context)
			{
				ereport(PANIC,
						errmsg("generation/context changed while locked, after wait: ref_gen: %llu, real_gen: %llu, orig ring: %d, cur ring: %d",
							   (long long unsigned) ref_generation,
							   (long long unsigned) io->generation,
							   (int)(context - aio_ctl->contexts),
							   io->ring),
						errhidestmt(true),
						errhidecontext(true));
			}

			ereport(DEBUG3,
					errmsg("[%d] after waiting for %zu/%llu, %d ready",
						   (int)(context - aio_ctl->contexts),
						   io - aio_ctl->in_progress_io,
						   (long long unsigned) io->generation,
						   io_uring_cq_ready(&context->io_uring_ring)),
					errhidestmt(true),
					errhidecontext(true));
		}

		/*
		 * Drain and call shared callbacks under lock while we already have it
		 * - that avoids others to first have to wait on the completion_lock
		 * for pgaio_uring_wait_one, then again in pgaio_drain(), and then
		 * again on the condition variable for the AIO.
		 */
		if (io_uring_cq_ready(&context->io_uring_ring))
		{
			pgaio_uring_drain_locked(context);
			pgaio_complete_ios(false);
		}
		else if (expect_cqe)
			elog(PANIC, "expected completion(s), but none ready");

		LWLockReleaseEx(&context->completion_lock,
						pgaio_uring_completion_check_wake, 0);
	}
	else
	{
		ereport(DEBUG3,
				errmsg("[%d] somebody else got the lock while waiting for %zu/%llu, %d ready",
					   (int)(context - aio_ctl->contexts),
					   io - aio_ctl->in_progress_io,
					   (long long unsigned) io->generation,
					   io_uring_cq_ready(&context->io_uring_ring)),
				errhidestmt(true),
				errhidecontext(true));
	}

	/*
	 * If the IO is still INFLIGHT (e.g. we just got completions for other
	 * IOs, or didn't get the lock), try again. We could return to
	 * pgaio_io_wait_ref_int(), but it's a bit more efficient to loop here.
	 */
	if (!pgaio_io_recycled(io, ref_generation, &flags) &&
		(flags & PGAIOIP_INFLIGHT))
	{
		loops++;
		ereport(DEBUG3,
				errmsg("[%d] will need to wait on %zu/%llu again, %d loops",
					   (int)(context - aio_ctl->contexts),
					   io - aio_ctl->in_progress_io,
					   (long long unsigned) io->generation,
					   loops),
				errhidestmt(true),
				errhidecontext(true));
		goto uring_wait_one_again;
	}

	my_aio->wr.context = NULL;
	my_aio->wr.aio = NULL;
	my_aio->wr.ref_generation = PG_UINT32_MAX;
}

static void
pgaio_uring_io_from_cqe(PgAioContext *context, struct io_uring_cqe *cqe)
{
	uint32 prev_inflight_count PG_USED_FOR_ASSERTS_ONLY;
	PgAioInProgress *io;

	io = io_uring_cqe_get_data(cqe);
	Assert(io != NULL);

	if (io->used_iovec != -1)
	{
		PgAioIovec *iovec = &context->iovecs[io->used_iovec];

		io->used_iovec = -1;
		slist_push_head(&context->reaped_iovecs, &iovec->node);
		context->reaped_iovecs_count++;
	}

	/* XXX: this doesn't need to be under the lock */
	prev_inflight_count = pg_atomic_fetch_sub_u32(&aio_ctl->backend_state[io->owner_id].inflight_count, 1);
	Assert(prev_inflight_count > 0);

	pgaio_process_io_completion(io, cqe->res);
}

static void
pgaio_uring_iovec_transfer(PgAioContext *context)
{
	Assert(LWLockHeldByMe(&context->submission_lock));
	Assert(LWLockHeldByMe(&context->completion_lock));

	while (!slist_is_empty(&context->reaped_iovecs))
	{
		slist_push_head(&context->unused_iovecs, slist_pop_head_node(&context->reaped_iovecs));
	}

	context->unused_iovecs_count += context->reaped_iovecs_count;
	context->reaped_iovecs_count = 0;
}

static struct PgAioIovec *
pgaio_uring_iovec_get(PgAioContext *context, PgAioInProgress *io)
{
	slist_node *node;
	PgAioIovec *iov;

	if (context->unused_iovecs_count == 0)
	{
		ereport(DEBUG2,
				errmsg("out of unused iovecs, transferring %d reaped ones",
					   context->reaped_iovecs_count),
				errhidestmt(true),
				errhidecontext(true));
		LWLockAcquire(&context->completion_lock, LW_EXCLUSIVE);
		Assert(context->reaped_iovecs_count > 0);
		pgaio_uring_iovec_transfer(context);
		LWLockRelease(&context->completion_lock);
		Assert(context->unused_iovecs_count > 0);
	}

	context->unused_iovecs_count--;
	node = slist_pop_head_node(&context->unused_iovecs);
	iov = slist_container(PgAioIovec, node, node);

	io->used_iovec = iov - context->iovecs;

	return iov;
}

static void
pgaio_uring_sq_from_io(PgAioContext *context, PgAioInProgress *io, struct io_uring_sqe *sqe)
{
	PgAioIovec *iovec;
	int iovcnt = 0;

	io->used_iovec = -1;

	switch (io->op)
	{
		case PGAIO_OP_READ:
			iovec = pgaio_uring_iovec_get(context, io);
			iovcnt = pgaio_fill_iov(iovec->iovec, io);

			io_uring_prep_readv(sqe,
								io->op_data.read.fd,
								iovec->iovec,
								iovcnt,
								io->op_data.read.offset
								+ io->op_data.read.already_done);
			break;

		case PGAIO_OP_WRITE:
			iovec = pgaio_uring_iovec_get(context, io);
			iovcnt = pgaio_fill_iov(iovec->iovec, io);

			io_uring_prep_writev(sqe,
								 io->op_data.write.fd,
								 iovec->iovec,
								 iovcnt,
								 io->op_data.write.offset
								 + io->op_data.write.already_done);
			break;

		case PGAIO_OP_FSYNC:
			io_uring_prep_fsync(sqe,
								io->op_data.fsync.fd,
								io->op_data.fsync.datasync ? IORING_FSYNC_DATASYNC : 0);
			break;

		case PGAIO_OP_FLUSH_RANGE:
			io_uring_prep_rw(IORING_OP_SYNC_FILE_RANGE,
							 sqe,
							 io->op_data.flush_range.fd,
							 NULL,
							 io->op_data.flush_range.nbytes,
							 io->op_data.flush_range.offset);
			sqe->sync_range_flags = SYNC_FILE_RANGE_WRITE;
			break;

		case PGAIO_OP_NOP:
			elog(ERROR, "not yet");
			break;

		case PGAIO_OP_INVALID:
			elog(ERROR, "invalid");
	}

	io_uring_sqe_set_data(sqe, io);
}
