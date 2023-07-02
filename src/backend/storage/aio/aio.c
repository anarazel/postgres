/*-------------------------------------------------------------------------
 *
 * aio.c
 *	  Asynchronous I/O subsytem.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/aio.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * Big picture changes needed:
 * - backend local recycleable IOs
 * - retries for AIOs that cannot be retried shared (e.g. XLogFileInit())
 * - docs / comments / cleanup
 * - move more of the shared callback logic into bufmgr.c etc
 * - merging of IOs when submitting individual IOs, not when submitting all pending IOs
 * - Shrink size of PgAioInProgress
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/memnodes.h"
#include "pgstat.h"
#include "storage/aio_internal.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"			/* XXX for io_direct */
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"


#define PGAIO_VERBOSE


/* pgaio helper functions */
static void pgaio_apply_backend_limit(void);
static void pgaio_bounce_buffer_release_internal(PgAioBounceBuffer *bb, bool holding_lock, bool release_resowner);
static void pgaio_io_ref_internal(PgAioInProgress *io, PgAioIoRef *ref);
static void pgaio_transfer_foreign_to_local(void);
static void pgaio_call_local_callbacks(bool in_error);
static void pgaio_io_call_local_callback(PgAioInProgress *io, bool in_error);
static int pgaio_synchronous_submit(void);
static void pgaio_io_wait_ref_int(PgAioIoRef *ref, bool call_shared, bool call_local);
static void pgaio_io_retry_soft_failed(PgAioInProgress *io, uint64 ref_generation);
static void pgaio_wait_for_issued(void);
static inline PgAioInProgress *pgaio_io_from_ref(PgAioIoRef *ref, uint64 *ref_generation);


/* global list of in-progress IO */
PgAioCtl *aio_ctl;

/* current backend's per-backend state */
PgAioPerBackend *my_aio;
int my_aio_id;

/* FIXME: move into PgAioPerBackend / subsume into ->reaped */
static dlist_head local_recycle_requests;

/* GUCs */
int io_method;
int max_aio_in_progress = 32768; /* XXX: Multiple of MaxBackends instead? */
int max_aio_in_flight = 4096;
int max_aio_bounce_buffers = 1024;
int io_max_concurrency = 128;

static int aio_local_callback_depth = 0;

/* Options for io_method. */
const struct config_enum_entry io_method_options[] = {
	{"worker", IOMETHOD_WORKER, false},
#ifdef USE_LIBURING
	{"io_uring", IOMETHOD_IO_URING, false},
#endif
#ifdef USE_POSIX_AIO
	{"posix_aio", IOMETHOD_POSIX_AIO, false},
#endif
	{NULL, 0, false}
};

static const IoMethodOps *pgaio_ops_table[] = {
#ifdef USE_POSIX_AIO
	[IOMETHOD_POSIX_AIO] = &pgaio_posix_aio_ops,
#endif
#ifdef USE_LIBURING
	[IOMETHOD_IO_URING] = &pgaio_uring_ops,
#endif
	[IOMETHOD_WORKER] = &pgaio_worker_ops
};

static const IoMethodOps *pgaio_impl;

void
assign_io_method(int newval, void *extra)
{
	pgaio_impl = pgaio_ops_table[newval];
}

/* --------------------------------------------------------------------------------
 * Initialization and shared memory management.
 * --------------------------------------------------------------------------------
 */

static Size
AioCtlShmemSize(void)
{
	Size		sz;

	/* aio_ctl itself */
	sz = offsetof(PgAioCtl, in_progress_io);

	/* ios */
	sz = add_size(sz, mul_size(max_aio_in_progress, sizeof(PgAioInProgress)));

	return sz;
}

static Size
AioCtlBackendShmemSize(void)
{
	uint32		TotalProcs = MaxBackends + NUM_AUXILIARY_PROCS;

	return mul_size(TotalProcs, sizeof(PgAioPerBackend));
}

static Size
AioBounceShmemSize(void)
{
	Size		sz;

	/* PgAioBounceBuffer itself */
	sz = mul_size(sizeof(PgAioBounceBuffer), max_aio_bounce_buffers);

	/* and the associated buffer */
	sz = add_size(sz,
				  mul_size(BLCKSZ, add_size(max_aio_bounce_buffers, 1)));

	return sz;
}

Size
AioShmemSize(void)
{
	Size		sz = 0;

	sz = add_size(sz, AioCtlShmemSize());
	sz = add_size(sz, AioCtlBackendShmemSize());
	sz = add_size(sz, AioBounceShmemSize());

	if (pgaio_impl->shmem_size)
		sz = add_size(sz, pgaio_impl->shmem_size());

	return sz;
}

void
AioShmemInit(void)
{
	bool		found;
	uint32		TotalProcs = MaxBackends + NUM_AUXILIARY_PROCS;

	aio_ctl = (PgAioCtl *)
		ShmemInitStruct("PgAio", AioCtlShmemSize(), &found);

	if (!found)
	{
		memset(aio_ctl, 0, AioCtlShmemSize());

		dlist_init(&aio_ctl->unused_ios);
		dlist_init(&aio_ctl->reaped_uncompleted);

		/* Initialize IOs. */
		for (int i = 0; i < max_aio_in_progress; i++)
		{
			PgAioInProgress *io = &aio_ctl->in_progress_io[i];

			ConditionVariableInit(&io->cv);
			dlist_push_tail(&aio_ctl->unused_ios, &io->owner_node);
			io->flags = PGAIOIP_UNUSED;
			io->system_referenced = true;
			io->generation = 1;
			io->bb_idx = PGAIO_BB_INVALID;
			io->merge_with_idx = PGAIO_MERGE_INVALID;
		}

		/* Initialize per-backend memory. */
		aio_ctl->backend_state_count = TotalProcs;
		aio_ctl->backend_state = (PgAioPerBackend *)
			ShmemInitStruct("PgAioBackend", AioCtlBackendShmemSize(), &found);
		memset(aio_ctl->backend_state, 0, AioCtlBackendShmemSize());

		for (int procno = 0; procno < TotalProcs; procno++)
		{
			PgAioPerBackend *bs = &aio_ctl->backend_state[procno];

			dlist_init(&bs->unused);
			dlist_init(&bs->outstanding);
			dlist_init(&bs->pending);
			dlist_init(&bs->issued);
			dlist_init(&bs->issued_abandoned);
			pg_atomic_init_u32(&bs->inflight_count, 0);
			dlist_init(&bs->reaped);

			dlist_init(&bs->foreign_completed);
			SpinLockInit(&bs->foreign_completed_lock);
		}

		/* Initialize bounce buffers. */
		{
			char *p;
			char *blocks;

			dlist_init(&aio_ctl->unused_bounce_buffers);
			aio_ctl->bounce_buffers =
				ShmemInitStruct("PgAioBounceBuffers",
								sizeof(PgAioBounceBuffer) * max_aio_bounce_buffers,
								&found);
			Assert(!found);

			p = ShmemInitStruct("PgAioBounceBufferBlocks",
								BLCKSZ * (max_aio_bounce_buffers + 1),
								&found);
			Assert(!found);
			blocks = (char *) TYPEALIGN(BLCKSZ, (uintptr_t) p);

			for (int i = 0; i < max_aio_bounce_buffers; i++)
			{
				PgAioBounceBuffer *bb = &aio_ctl->bounce_buffers[i];

				bb->buffer = blocks + i * BLCKSZ;
				memset(bb->buffer, 0, BLCKSZ);
				pg_atomic_init_u32(&bb->refcount, 0);
				dlist_push_tail(&aio_ctl->unused_bounce_buffers, &bb->node);
				aio_ctl->unused_bounce_buffers_count++;
			}
		}
	}

	/* Initialize IO method specific resources. */
	pgaio_impl->shmem_init(!found);
}

void
pgaio_postmaster_init(void)
{
	/* FIXME: should also be allowed to use AIO */
	dlist_init(&local_recycle_requests);

	// XXX: could create a local queue here.
}

void
pgaio_postmaster_child_init_local(void)
{
	if (pgaio_impl->postmaster_child_init_local)
		pgaio_impl->postmaster_child_init_local();
}

static void
pgaio_postmaster_before_child_exit(int code, Datum arg)
{
	elog(DEBUG2, "aio before shmem exit: start");

	/*
	 * When exitinging in a normal backend there will be no pending IOs due to
	 * pgaio_at_commit()/pgaio_at_abort(). But for aux processes that won't be
	 * the case, so do so explicitly. It's also a useful backstop against
	 * resowner tracking failing due to recursive
	 */
	pgaio_at_error();

	while (!dlist_is_empty(&my_aio->outstanding))
	{
		PgAioInProgress *io = dlist_head_element(PgAioInProgress, owner_node, &my_aio->outstanding);

		elog(WARNING, "leaked outstanding io %zu", io - aio_ctl->in_progress_io);

		pgaio_io_release(io);
	}

	while (!dlist_is_empty(&my_aio->issued))
	{
		PgAioInProgress *io = dlist_head_element(PgAioInProgress, owner_node, &my_aio->issued);

		elog(WARNING, "leaked issued io %zu", io - aio_ctl->in_progress_io);

		pgaio_io_release(io);
	}

	/*
	 * Submit pending requests *after* releasing all IO references. That
	 * ensures that local callback won't get called.
	 */
	pgaio_submit_pending(false);

	/*
	 * Need to wait for in-progress IOs initiated by this backend to
	 * finish. Some operating systems, like linux w/ io_uring, cancel IOs that
	 * are still in progress when exiting. Other's don't provide access to the
	 * results of such IOs.
	 *
	 * XXX This is not sufficient: if we retried an IO that another backend
	 * owns (ie we submitted it to the kernel, even though another backend
	 * submitted it originally), it will not be on our issued lists.
	 *
	 * XXX Temporarily add a per-impl hook for that, so that POSIX AIO can do
	 * this a more reliable way, because otherwise Macs run into trouble (high
	 * retry rate makes it likely that we'll exit with an IO we haven't
	 * handled).  This isn't a permanent solution, I just want to unbreak CI;
	 * once this higher lever code has enough book keeping to wait for all
	 * submitted then we could drop this.
	 */

	if (pgaio_impl->postmaster_before_child_exit)
		pgaio_impl->postmaster_before_child_exit();

	pgaio_wait_for_issued();
	pgaio_complete_ios(true);

	elog(DEBUG2, "aio before shmem exit: end");
}

static void
pgaio_postmaster_child_exit(int code, Datum arg)
{
	/* FIXME: handle unused */
	Assert(my_aio->outstanding_count == 0);
	Assert(dlist_is_empty(&my_aio->outstanding));

	Assert(my_aio->pending_count == 0);
	Assert(dlist_is_empty(&my_aio->pending));

	Assert(my_aio->issued_count == 0);
	Assert(dlist_is_empty(&my_aio->issued));

	Assert(my_aio->issued_abandoned_count == 0);
	Assert(dlist_is_empty(&my_aio->issued_abandoned));

	Assert(pg_atomic_read_u32(&my_aio->inflight_count) == 0);

	Assert(dlist_is_empty(&my_aio->reaped));

	Assert(my_aio->local_completed_count == 0);
	Assert(dlist_is_empty(&my_aio->local_completed));

	Assert(my_aio->foreign_completed_count == 0);
	Assert(dlist_is_empty(&my_aio->foreign_completed));

	my_aio = NULL;
}

void
pgaio_postmaster_child_init(void)
{
	uint32		TotalProcs = MaxBackends + NUM_AUXILIARY_PROCS;

	/* shouldn't be initialized twice */
	Assert(!my_aio);

	if (MyProc == NULL || MyProc->pgprocno >= TotalProcs)
		elog(ERROR, "aio requires a normal PGPROC");

	my_aio_id = MyProc->pgprocno;
	my_aio = &aio_ctl->backend_state[my_aio_id];

	dlist_init(&local_recycle_requests);

	before_shmem_exit(pgaio_postmaster_before_child_exit, 0);
	on_shmem_exit(pgaio_postmaster_child_exit, 0);

	Assert(my_aio->unused_count == 0);
	Assert(my_aio->outstanding_count == 0);
	Assert(my_aio->issued_count == 0);
	Assert(my_aio->issued_abandoned_count == 0);
	Assert(my_aio->pending_count == 0);
	Assert(my_aio->local_completed_count == 0);
	Assert(my_aio->foreign_completed_count == 0);

	/* try to spread out a bit from the start */
	my_aio->last_context = MyProcPid % PGAIO_NUM_CONTEXTS;

	my_aio->executed_total_count = 0;
	my_aio->issued_total_count = 0;
	my_aio->submissions_total_count = 0;
	my_aio->foreign_completed_total_count = 0;
	my_aio->retry_total_count = 0;

#ifdef USE_LIBURING
	if (io_method == IOMETHOD_IO_URING)
	{
		/* no locking needed here, only affects this process */
		for (int i = 0; i < aio_ctl->num_contexts; i++)
			io_uring_ring_dontfork(&aio_ctl->contexts[i].io_uring_ring);
	}
#endif

#ifdef USE_LIBURING
	my_aio->wr.context = NULL;
	my_aio->wr.aio = NULL;
	my_aio->wr.ref_generation = PG_UINT32_MAX;
#endif
}


/* --------------------------------------------------------------------------------
 * Functions dealing with more than one IO.
 * --------------------------------------------------------------------------------
 */

void
pgaio_at_error(void)
{
	/* could have failed within a callback, so reset */
	aio_local_callback_depth = 0;

	pgaio_complete_ios(/* in_error = */ true);
}

void
pgaio_at_abort(void)
{
	/* should have been reset via pgaio_at_error */
	Assert(aio_local_callback_depth == 0);
	Assert(dlist_is_empty(&local_recycle_requests));

	/* should have been released via resowner cleanup */
	Assert(dlist_is_empty(&my_aio->outstanding));
	Assert(dlist_is_empty(&my_aio->issued));

	/*
	 * Need to submit pending IO requests. It could be that the code staging
	 * the IO errored out before getting to that - and this code might already
	 * hold locks required for IO.
	 */
	pgaio_submit_pending(false);
}

void
pgaio_at_subabort(void)
{
	/* should have been reset via pgaio_at_error */
	Assert(aio_local_callback_depth == 0);
	Assert(dlist_is_empty(&local_recycle_requests));

	/* see pgaio_at_abort() */
	pgaio_submit_pending(false);
}

void
pgaio_at_commit(void)
{
	Assert(aio_local_callback_depth == 0);
	Assert(dlist_is_empty(&local_recycle_requests));

	/* should have been released via resowner cleanup */
	Assert(dlist_is_empty(&my_aio->outstanding));
	Assert(dlist_is_empty(&my_aio->issued));

#if 0
	/* see pgaio_at_abort() */
	if (my_aio->pending_count != 0)
	{
		elog(DEBUG1, "unsubmitted IOs %d", my_aio->pending_count);
		pgaio_submit_pending(false);
	}
#endif
}

void
pgaio_at_subcommit(void)
{
	Assert(aio_local_callback_depth == 0);
	Assert(dlist_is_empty(&local_recycle_requests));

#if 0
	/* see pgaio_at_abort() */
	if (my_aio->pending_count != 0)
	{
		elog(DEBUG1, "unsubmitted IOs %d", my_aio->pending_count);
		pgaio_submit_pending(false);
	}
#endif
}

void
pgaio_complete_ios(bool in_error)
{
	int pending_count_before = my_aio->pending_count;

	Assert(!LWLockHeldByMe(SharedAIOCtlLock));

	/*
	 * Don't want to recurse into proc_exit() or such while calling callbacks
	 * - we need to process the shared (but not local) callbacks.
	 */
	HOLD_INTERRUPTS();

	/* call all callbacks, without holding lock */
	while (!dlist_is_empty(&my_aio->reaped))
	{
		dlist_node *node = dlist_head_node(&my_aio->reaped);
		PgAioInProgress *io = dlist_container(PgAioInProgress, io_node, node);

		Assert(dlist_is_member(&my_aio->reaped, &io->io_node));

		Assert(node != NULL);

		if (!(io->flags & PGAIOIP_SHARED_CALLBACK_CALLED))
		{
			bool finished;

			/*
			 * Set flag before calling callback, otherwise we could easily end
			 * up looping forever.
			 */
			WRITE_ONCE_F(io->flags) |= PGAIOIP_SHARED_CALLBACK_CALLED;

			finished = pgaio_io_call_shared_complete(io);

			dlist_delete_from(&my_aio->reaped, node);

			if (finished)
			{
				/* if a soft failure is done, we can't retry */
				if (io->flags & PGAIOIP_SOFT_FAILURE)
				{
					WRITE_ONCE_F(io->flags) =
						(io->flags & ~PGAIOIP_SOFT_FAILURE) |
						PGAIOIP_HARD_FAILURE;
				}

				dlist_push_tail(&local_recycle_requests, &io->io_node);
			}
			else
			{
				Assert(io->flags & (PGAIOIP_SOFT_FAILURE | PGAIOIP_HARD_FAILURE));

				LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);
				WRITE_ONCE_F(io->flags) =
					(io->flags & ~(PGAIOIP_REAPED | PGAIOIP_IN_PROGRESS)) |
					PGAIOIP_DONE;
				dlist_push_tail(&aio_ctl->reaped_uncompleted, &io->io_node);
				LWLockRelease(SharedAIOCtlLock);

				/* signal state change */
				if (IsUnderPostmaster)
					ConditionVariableBroadcast(&io->cv);
			}
		}
		else
		{
			Assert(in_error);

			dlist_delete_from(&my_aio->reaped, node);

			LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);
			WRITE_ONCE_F(io->flags) =
				(io->flags & ~(PGAIOIP_REAPED | PGAIOIP_IN_PROGRESS)) |
				PGAIOIP_DONE |
				PGAIOIP_HARD_FAILURE;
			dlist_push_tail(&aio_ctl->reaped_uncompleted, &io->io_node);
			LWLockRelease(SharedAIOCtlLock);
		}
	}

	RESUME_INTERRUPTS();

	/* if any IOs weren't fully done, re-submit them */
	if (pending_count_before != my_aio->pending_count)
		pgaio_submit_pending(false);

	/*
	 * Next, under lock, process all the still pending requests. This entails
	 * releasing the "system" reference on the IO and checking which callbacks
	 * need to be called.
	 */
	START_CRIT_SECTION();

	while (!dlist_is_empty(&local_recycle_requests))
	{
		dlist_mutable_iter iter;
		PgAioInProgress* signal_ios[32];
		int to_signal = 0;

		LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);

		dlist_foreach_modify(iter, &local_recycle_requests)
		{
			PgAioInProgress *cur = dlist_container(PgAioInProgress, io_node, iter.cur);

			dlist_delete_from(&local_recycle_requests, iter.cur);
			signal_ios[to_signal++] = cur;

			Assert(cur->system_referenced);
			Assert(cur->flags & PGAIOIP_REAPED);
			Assert(!(cur->flags & PGAIOIP_DONE));
			Assert(!(cur->flags & PGAIOIP_INFLIGHT));
			Assert(!(cur->flags & (PGAIOIP_SOFT_FAILURE)));
			Assert(cur->merge_with_idx == PGAIO_MERGE_INVALID);

			if (cur->user_referenced)
			{
				cur->system_referenced = false;

				if (cur->owner_id != my_aio_id)
				{
					PgAioPerBackend *other = &aio_ctl->backend_state[cur->owner_id];

					SpinLockAcquire(&other->foreign_completed_lock);

					dlist_push_tail(&other->foreign_completed, &cur->io_node);
					other->foreign_completed_count++;
					other->foreign_completed_total_count++;

					pg_write_barrier();

					WRITE_ONCE_F(cur->flags) =
						(cur->flags & ~(PGAIOIP_REAPED | PGAIOIP_IN_PROGRESS)) |
						PGAIOIP_DONE |
						PGAIOIP_FOREIGN_DONE;

					SpinLockRelease(&other->foreign_completed_lock);
				}
				else
				{
					WRITE_ONCE_F(cur->flags) =
						(cur->flags & ~(PGAIOIP_REAPED | PGAIOIP_IN_PROGRESS)) |
						PGAIOIP_DONE;

					dlist_push_tail(&my_aio->local_completed, &cur->io_node);
					my_aio->local_completed_count++;
				}
			}
			else
			{
				PgAioPerBackend *other = &aio_ctl->backend_state[cur->owner_id];

#ifdef PGAIO_VERBOSE
				ereport(DEBUG4,
						errmsg("removing aio %zu/%llu from issued_abandoned complete_ios",
							   cur - aio_ctl->in_progress_io,
							   (long long unsigned) cur->generation),
						errhidecontext(1),
						errhidestmt(1));
#endif

				dlist_delete_from(&other->issued_abandoned, &cur->owner_node);
				Assert(other->issued_abandoned_count > 0);
				other->issued_abandoned_count--;

				cur->generation++;
				pg_write_barrier();

				cur->flags = PGAIOIP_UNUSED;

				if (cur->bb_idx != PGAIO_BB_INVALID)
				{
					pgaio_bounce_buffer_release_internal(&aio_ctl->bounce_buffers[cur->bb_idx],
														 /* holding_lock = */ true,
														 /* release_resowner = */ false);
					cur->bb_idx = PGAIO_BB_INVALID;
				}

				cur->op = PGAIO_OP_INVALID;
				cur->scb = PGAIO_SCB_INVALID;
				cur->owner_id = INVALID_PGPROCNO;
				cur->result = 0;
				cur->system_referenced = true;
				cur->on_completion_local = NULL;

				dlist_push_head(&aio_ctl->unused_ios, &cur->owner_node);
				aio_ctl->used_count--;
			}

			if (to_signal >= lengthof(signal_ios))
				break;
		}
		LWLockRelease(SharedAIOCtlLock);

		if (IsUnderPostmaster)
		{
			for (int i = 0; i < to_signal; i++)
			{
				ConditionVariableBroadcast(&signal_ios[i]->cv);
			}
		}
	}

	END_CRIT_SECTION();
}

/*
 * Broadcast on a set of IOs (and merged IOs).
 *
 * We need to be careful about other backends resetting merge_with_idx.  We
 * still could end up broadcasting on IOs that we don't care about, but that's
 * harmless.
 */
void
pgaio_broadcast_ios(PgAioInProgress **ios, int nios)
{
	for (int i = 0; i < nios; i++)
	{
		PgAioInProgress *cur = ios[i];

		while (true)
		{
			uint32 next_idx;

			ConditionVariableBroadcast(&cur->cv);

			next_idx = cur->merge_with_idx;
			pg_compiler_barrier();
			if (next_idx == PGAIO_MERGE_INVALID)
				break;
			cur = &aio_ctl->in_progress_io[next_idx];
		}
	}
}

/*
 * Call all pending local callbacks.
 */
static void
pgaio_call_local_callbacks(bool in_error)
{
	if (my_aio->local_completed_count != 0)
	{
		/*
		 * Don't call local callbacks in a critical section. If a specific IO
		 * is required to finish within the critical section, and that IO uses
		 * a local callback, pgaio_io_wait_ref_int() will call that callback
		 * regardless of the callback depth.
		 */
		if (CritSectionCount != 0)
			return;

		if (aio_local_callback_depth > 0)
			return;

		while (!dlist_is_empty(&my_aio->local_completed))
		{
			dlist_node *node = dlist_pop_head_node(&my_aio->local_completed);
			PgAioInProgress *io = dlist_container(PgAioInProgress, io_node, node);

			pgaio_io_call_local_callback(io, in_error);
		}
	}
}

/*
 * Receive completions in ring.
 */
int
pgaio_drain(PgAioContext *context, bool block, bool call_shared, bool call_local)
{
	int ndrained = 0;

	if (pgaio_impl->drain)
		ndrained = pgaio_impl->drain(context, block, call_shared);

	if (call_shared)
	{
		pgaio_complete_ios(false);
		pgaio_transfer_foreign_to_local();
	}
	if (call_local)
	{
		Assert(call_shared);
		pgaio_call_local_callbacks(false);
	}
	return ndrained;
}

/*
 * aio_exchange.c needs to be able to request a very limited kind of 'drain'
 * that receives results from the kernel but does no other processing.
 */
void
pgaio_drain_in_interrupt_handler(void)
{
	pgaio_impl->drain_in_interrupt_handler();
}

static void
pgaio_transfer_foreign_to_local(void)
{
	/*
	 * Transfer all the foreign completions into the local queue.
	 */
	if (my_aio->foreign_completed_count != 0)
	{
		SpinLockAcquire(&my_aio->foreign_completed_lock);

		while (!dlist_is_empty(&my_aio->foreign_completed))
		{
			dlist_node *node = dlist_pop_head_node(&my_aio->foreign_completed);
			PgAioInProgress *io = dlist_container(PgAioInProgress, io_node, node);

			Assert(!(io->flags & PGAIOIP_LOCAL_CALLBACK_CALLED));

			dlist_push_tail(&my_aio->local_completed, &io->io_node);
			io->flags &= ~PGAIOIP_FOREIGN_DONE;
			my_aio->foreign_completed_count--;
			my_aio->local_completed_count++;
		}
		SpinLockRelease(&my_aio->foreign_completed_lock);
	}
}

static int
pgaio_synchronous_submit(void)
{
	int nsubmitted = 0;

	while (!dlist_is_empty(&my_aio->pending))
	{
		dlist_node *node;
		PgAioInProgress *io;

		node = dlist_head_node(&my_aio->pending);
		io = dlist_container(PgAioInProgress, io_node, node);

		pgaio_io_prepare_submit(io, io->ring);
		pgaio_do_synchronously(io);

		++nsubmitted;
	}

	return nsubmitted;
}

static void
pgaio_submit_pending_internal(bool drain, bool call_shared, bool call_local, bool will_wait)
{
	int total_submitted = 0;
	uint32 orig_total;

	if (!aio_ctl || !my_aio)
		return;

	if (my_aio->pending_count == 0)
	{
		Assert(dlist_is_empty(&my_aio->pending));
		return;
	}

	HOLD_INTERRUPTS();

	orig_total = my_aio->pending_count;

#define COMBINE_ENABLED

#ifdef COMBINE_ENABLED
#if 0
	ereport(LOG, errmsg("before combine"),
			errhidestmt(true),
			errhidecontext(true));
	pgaio_print_list(&my_aio->pending, NULL, offsetof(PgAioInProgress, io_node));
#endif
	if (my_aio->pending_count > 1 && PGAIO_MAX_COMBINE > 1)
		pgaio_combine_pending();

#if 0
	ereport(LOG, errmsg("after combine"),
			errhidestmt(true),
			errhidecontext(true));
	pgaio_print_list(&my_aio->pending, NULL, offsetof(PgAioInProgress, io_node));
#endif
#endif /* COMBINE_ENABLED */

	/*
	 * FIXME: currently the pgaio_synchronous_submit() path is not compatible
	 * with IOMETHOD_IO_URING: There's e.g. a danger we'd wait for io_uring
	 * events, which could stall (or trigger asserts), as
	 * pgaio_io_wait_ref_int() right now has no way of detecting that case.
	 */
	if (io_method == IOMETHOD_IO_URING)
		will_wait = false;

	/*
	 * Loop until all pending IOs are submitted. Throttle max in-flight before
	 * calling into the IO implementation specific routine, so this code can
	 * be shared.
	 */
	while (!dlist_is_empty(&my_aio->pending))
	{
		int max_submit;
		int did_submit;
		int inflight_limit;
		int pending_count;

		Assert(my_aio->pending_count > 0);
		pgaio_apply_backend_limit();

		pending_count = my_aio->pending_count;
		Assert(pending_count > 0);
		if (pending_count == 0)
			break;

		inflight_limit = io_max_concurrency - pg_atomic_read_u32(&my_aio->inflight_count);
		max_submit = Min(pending_count, PGAIO_SUBMIT_BATCH_SIZE);
		max_submit = Min(max_submit, inflight_limit);
		Assert(max_submit > 0 && max_submit <= pending_count);

		START_CRIT_SECTION();
		if (my_aio->pending_count == 1 && will_wait)
			did_submit = pgaio_synchronous_submit();
		else
			did_submit = pgaio_impl->submit(max_submit, drain);
		total_submitted += did_submit;
		Assert(did_submit > 0 && did_submit <= max_submit);
		END_CRIT_SECTION();
	}

	my_aio->executed_total_count += orig_total;
	my_aio->issued_total_count += total_submitted;

#ifdef PGAIO_VERBOSE
	ereport(DEBUG3,
			errmsg("submitted %d (orig %d), %d in flight",
				   total_submitted, orig_total,
				   pg_atomic_read_u32(&my_aio->inflight_count)),
			errhidestmt(true),
			errhidecontext(true));
#endif

	RESUME_INTERRUPTS();

	if (call_shared)
		pgaio_complete_ios(false);

	if (call_local)
	{
		pgaio_transfer_foreign_to_local();
		pgaio_call_local_callbacks(/* in_error = */ false);
	}
}

void pg_noinline
pgaio_submit_pending(bool drain)
{
	Assert(my_aio);

	/*
	 * We allow shared callbacks to be called even when not draining, as we
	 * might be forced to drain due to backend IO concurrency limits.
	 */
	pgaio_submit_pending_internal(drain,
								  /* call_shared */ true,
								  /* call_local */ drain,
								  /* will_wait */ false);
}

void
pgaio_limit_pending(bool drain, int limit_to)
{
	if (limit_to <= my_aio->pending_count)
		pgaio_submit_pending(drain);
}

void
pgaio_closing_possibly_referenced(void)
{
	if (!my_aio)
		return;

	/* the callback could trigger further IO or such ATM */
	pgaio_submit_pending_internal(false,
								  /* call_shared */ false,
								  /* call_local */ false,
								  /* will_wait */ false);
}

/*
 * Should be called before closing any fd that might have asynchronous I/O
 * operations in progress.  Some implementations may need to drain the
 * descriptor first.
 */
void
pgaio_closing_fd(int fd)
{
	if (pgaio_impl->closing_fd)
		pgaio_impl->closing_fd(fd);
}

static void
pgaio_apply_backend_limit(void)
{
	uint32 current_inflight = pg_atomic_read_u32(&my_aio->inflight_count);

	while (current_inflight >= io_max_concurrency)
	{
		PgAioInProgress *io;

		/*
		 * XXX: Should we be a bit fairer and check the "oldest" in-flight IO
		 * between issued and issued_abandoned?
		 */

		if (my_aio->issued_count > 0)
		{
			dlist_iter iter;

			Assert(!dlist_is_empty(&my_aio->issued));

			dlist_foreach(iter, &my_aio->issued)
			{
				io = dlist_container(PgAioInProgress, owner_node, iter.cur);

				if (io->flags & PGAIOIP_INFLIGHT)
				{
					PgAioIoRef ref;

					ereport(DEBUG2,
							errmsg("applying per-backend limit to issued IO %zu/%llu (current %d in %d, target %d)",
								   io - aio_ctl->in_progress_io,
								   (unsigned long long) io->generation,
								   my_aio->issued_count + my_aio->issued_abandoned_count,
								   current_inflight,
								   io_max_concurrency),
							errhidestmt(true),
							errhidecontext(true));

					pgaio_io_ref(io, &ref);
					pgaio_io_wait_ref_int(&ref, /* call_shared = */ false, /* call_local = */ false);
					current_inflight = pg_atomic_read_u32(&my_aio->inflight_count);
					break;
				}
			}
		}

		if (current_inflight < io_max_concurrency)
			break;

		if (my_aio->issued_abandoned_count > 0)
		{
			dlist_iter iter;
			PgAioIoRef ref;

			/*
			 * ->issued_abandoned_count is only maintained once shared
			 * callbacks have been invoked. So do so, as otherwise we could
			 * end up looping here endlessly, as those IOs already finished.
			 */
			pgaio_complete_ios(false);
			pgaio_transfer_foreign_to_local();

			io = NULL;

			LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);
			dlist_foreach(iter, &my_aio->issued_abandoned)
			{
				io = dlist_container(PgAioInProgress, owner_node, iter.cur);

				if (io->flags & PGAIOIP_INFLIGHT)
				{
					pgaio_io_ref_internal(io, &ref);
					break;
				}
				else
					io = NULL;
			}
			LWLockRelease(SharedAIOCtlLock);

			if (io == NULL)
				continue;

			ereport(DEBUG2,
					errmsg("applying per-backend limit to issued_abandoned IO %zu/%llu (current %d in %d, target %d)",
						   io - aio_ctl->in_progress_io,
						   (unsigned long long) io->generation,
						   my_aio->issued_count + my_aio->issued_abandoned_count,
						   current_inflight,
						   io_max_concurrency),
					errhidestmt(true),
					errhidecontext(true));

			pgaio_io_wait_ref_int(&ref,  /* call_shared = */ false, /* call_local = */ false);
		}

		current_inflight = pg_atomic_read_u32(&my_aio->inflight_count);
	}
}

/*
 * Wait for all IOs issued by this process to complete.
 */
static void
pgaio_wait_for_issued(void)
{
	dlist_iter iter;

	dlist_foreach(iter, &my_aio->issued)
	{
		PgAioInProgress *io = dlist_container(PgAioInProgress, owner_node, iter.cur);

		if (io->flags & PGAIOIP_INFLIGHT)
		{
			PgAioIoRef ref;

			pgaio_io_ref_internal(io, &ref);
			pgaio_io_print(io, NULL);
			pgaio_io_wait_ref(&ref, false);
		}
	}

	/* XXX explain dirty read of issued_abandoned */
	while (!dlist_is_empty(&my_aio->issued_abandoned))
	{
		PgAioInProgress *io = NULL;
		PgAioIoRef ref;

		LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);
		if (!dlist_is_empty(&my_aio->issued_abandoned))
		{
			io = dlist_head_element(PgAioInProgress, owner_node, &my_aio->issued_abandoned);
			pgaio_io_ref_internal(io, &ref);
		}
		LWLockRelease(SharedAIOCtlLock);

		if (!io)
			break;

		pgaio_io_print(io, NULL);
		pgaio_io_wait_ref(&ref, false);
		pgaio_complete_ios(false);
		pgaio_transfer_foreign_to_local();
	}
}


/* --------------------------------------------------------------------------------
 * Functions primarily dealing with one IO.
 * --------------------------------------------------------------------------------
 */

PgAioInProgress *
pgaio_io_get(void)
{
	dlist_node *elem;
	PgAioInProgress *io;

	Assert(!LWLockHeldByMe(SharedAIOCtlLock));

	Assert(CurrentResourceOwner != NULL);

	// FIXME: relax?
	Assert(my_aio->pending_count < PGAIO_SUBMIT_BATCH_SIZE);

	/* FIXME: wait for an IO to complete if full */

	LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);

	while (unlikely(dlist_is_empty(&aio_ctl->unused_ios)))
	{
		LWLockRelease(SharedAIOCtlLock);
		elog(WARNING, "needed to drain while getting IO (used %d inflight %d)",
			 aio_ctl->used_count, pg_atomic_read_u32(&my_aio->inflight_count));

		/*
		 * FIXME: should we wait for IO instead?
		 *
		 * Also, need to protect against too many ios handed out but not used.
		 */
		for (int i = 0; i < aio_ctl->num_contexts; i++)
			pgaio_drain(&aio_ctl->contexts[i],
						/* block = */ true,
						/* call_shared = */ true,
						/* call_local = */ true);

		LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);
	}

	elem = dlist_pop_head_node(&aio_ctl->unused_ios);
	aio_ctl->used_count++;

	LWLockRelease(SharedAIOCtlLock);

	io = dlist_container(PgAioInProgress, owner_node, elem);

#ifdef PGAIO_VERBOSE
	ereport(DEBUG3, errmsg("acquired io %zu/%llu",
						   io - aio_ctl->in_progress_io,
						   (long long unsigned) io->generation),
			errhidecontext(1),
			errhidestmt(1));
#endif

	Assert(io->op == PGAIO_OP_INVALID);
	Assert(io->scb == PGAIO_SCB_INVALID);
	Assert(io->flags == PGAIOIP_UNUSED);
	Assert(io->system_referenced);
	Assert(io->on_completion_local == NULL);
	Assert(io->merge_with_idx == PGAIO_MERGE_INVALID);

	io->user_referenced = true;
	io->system_referenced = false;

	WRITE_ONCE_F(io->flags) = PGAIOIP_IDLE;

	io->owner_id = my_aio_id;
	io->submitter_id = my_aio_id;	/* XXX or invalid, with defences elsewhere? */

	dlist_push_tail(&my_aio->outstanding, &io->owner_node);
	my_aio->outstanding_count++;

	ResourceOwnerRememberAioIP(CurrentResourceOwner, &io->resowner_node);

	return io;
}

void
pgaio_io_release(PgAioInProgress *io)
{
	Assert(io->user_referenced);
	Assert(io->owner_id == my_aio_id);

	ResourceOwnerForgetAioIP(CurrentResourceOwner, &io->resowner_node);

	LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);

	io->user_referenced = false;

	if (io->flags & (PGAIOIP_IDLE |
					 PGAIOIP_PREP |
					 PGAIOIP_PENDING |
					 PGAIOIP_LOCAL_CALLBACK_CALLED))
	{
		Assert(!(io->flags & PGAIOIP_INFLIGHT));

		Assert(my_aio->outstanding_count > 0);
		dlist_delete_from(&my_aio->outstanding, &io->owner_node);
		my_aio->outstanding_count--;

#ifdef PGAIO_VERBOSE
		ereport(DEBUG4, errmsg("releasing plain user reference to %zu",
							   io - aio_ctl->in_progress_io),
				errhidecontext(1),
				errhidestmt(1));
#endif
	}
	else
	{
		dlist_delete_from(&my_aio->issued, &io->owner_node);
		my_aio->issued_count--;

		if (io->system_referenced)
		{
#ifdef PGAIO_VERBOSE
			ereport(DEBUG4, errmsg("putting aio %zu onto issued_abandoned during release",
								   io - aio_ctl->in_progress_io),
					errhidecontext(1),
					errhidestmt(1));
#endif

			dlist_push_tail(&my_aio->issued_abandoned, &io->owner_node);
			my_aio->issued_abandoned_count++;
		}
		else
		{
			Assert(io->flags & (PGAIOIP_DONE | PGAIOIP_SHARED_CALLBACK_CALLED));

#ifdef PGAIO_VERBOSE
			ereport(DEBUG4, errmsg("not putting aio %zu onto issued_abandoned during release",
								   io - aio_ctl->in_progress_io),
					errhidecontext(1),
					errhidestmt(1));
#endif
		}
	}

	if (!io->system_referenced)
	{
		Assert(!(io->flags & PGAIOIP_INFLIGHT));
		Assert(io->flags & (PGAIOIP_IDLE |
							PGAIOIP_PREP |
							PGAIOIP_DONE));

		if (io->flags & PGAIOIP_DONE)
		{
			if (io->flags & PGAIOIP_FOREIGN_DONE)
			{
				SpinLockAcquire(&my_aio->foreign_completed_lock);
				Assert(io->flags & PGAIOIP_FOREIGN_DONE);
				dlist_delete_from(&my_aio->foreign_completed, &io->io_node);
				my_aio->foreign_completed_count--;
				SpinLockRelease(&my_aio->foreign_completed_lock);
			}
			else if (!(io->flags & PGAIOIP_LOCAL_CALLBACK_CALLED))
			{
				dlist_delete_from(&my_aio->local_completed, &io->io_node);
				my_aio->local_completed_count--;
				io->on_completion_local = NULL;
			}

		}

		io->generation++;
		pg_write_barrier();

		io->flags = PGAIOIP_UNUSED;
		io->op = PGAIO_OP_INVALID;
		io->scb = PGAIO_SCB_INVALID;
		io->owner_id = INVALID_PGPROCNO;
		io->result = 0;
		io->system_referenced = true;
		io->on_completion_local = NULL;

		Assert(io->merge_with_idx == PGAIO_MERGE_INVALID);

		if (io->bb_idx != PGAIO_BB_INVALID)
		{
			pgaio_bounce_buffer_release_internal(&aio_ctl->bounce_buffers[io->bb_idx],
												 /* holding_lock = */ true,
												 /* release_resowner = */ false);
			io->bb_idx = PGAIO_BB_INVALID;
		}

		dlist_push_head(&aio_ctl->unused_ios, &io->owner_node);
		aio_ctl->used_count--;
	}

	LWLockRelease(SharedAIOCtlLock);
}

void
pgaio_io_release_resowner(dlist_node *aio_node)
{
	PgAioInProgress *io = dlist_container(PgAioInProgress, resowner_node, aio_node);

	pgaio_io_release(io);
}

void
pgaio_io_resowner_leak(struct dlist_node *aio_node)
{
	PgAioInProgress *io = dlist_container(PgAioInProgress, resowner_node, aio_node);
	StringInfoData s;

	initStringInfo(&s);

	pgaio_io_print(io, &s);

	elog(WARNING, "aio reference leak: %s",
		 s.data);

	pfree(s.data);
}

void
pgaio_io_recycle(PgAioInProgress *io)
{
	Assert(io->flags & (PGAIOIP_IDLE | PGAIOIP_DONE));
	Assert(io->user_referenced);
	Assert(io->owner_id == my_aio_id);
	Assert(!io->system_referenced);
	Assert(io->merge_with_idx == PGAIO_MERGE_INVALID);

	if (io->bb_idx != PGAIO_BB_INVALID)
	{
		pgaio_bounce_buffer_release_internal(&aio_ctl->bounce_buffers[io->bb_idx],
											 /* holding_lock = */ false,
											 /* release_resowner = */ false);
		io->bb_idx = PGAIO_BB_INVALID;
	}

	if (io->flags & PGAIOIP_DONE)
	{
		/* request needs to actually be done, including local callbacks */
		Assert(!(io->flags & PGAIOIP_FOREIGN_DONE));
		Assert(io->flags & PGAIOIP_LOCAL_CALLBACK_CALLED);

		io->generation++;
		pg_write_barrier();

		io->flags &= ~PGAIOIP_DONE;
		io->flags |= PGAIOIP_IDLE;

		io->op = PGAIO_OP_INVALID;
		io->scb = PGAIO_SCB_INVALID;
	}

	io->flags &= ~(PGAIOIP_SHARED_CALLBACK_CALLED |
				   PGAIOIP_LOCAL_CALLBACK_CALLED |
				   PGAIOIP_RETRY |
				   PGAIOIP_HARD_FAILURE |
				   PGAIOIP_SOFT_FAILURE);
	Assert(io->flags == PGAIOIP_IDLE);
	io->result = 0;
	io->on_completion_local = NULL;
}

/*
 * Prepare idle IO to be initialized with an IO operation. After this either
 * pgaio_io_unprepare() has to be called (if the IO actually couldn't be
 * submitted), or the IO has to be staged with pgaio_io_stage().
 */
void
pgaio_io_prepare(PgAioInProgress *io, PgAioOp op)
{
	/* true for now, but not necessarily in the future */
	Assert(io->flags == PGAIOIP_IDLE);
	Assert(io->user_referenced);
	Assert(io->merge_with_idx == PGAIO_MERGE_INVALID);
	Assert(io->op == PGAIO_OP_INVALID);
	Assert(io->scb == PGAIO_SCB_INVALID);

	if (my_aio->pending_count + 1 >= PGAIO_SUBMIT_BATCH_SIZE)
	{
#ifdef PGAIO_VERBOSE
		ereport(DEBUG3,
				errmsg("submitting during prep for %zu due to %u inflight",
					   io - aio_ctl->in_progress_io,
					   my_aio->pending_count),
				errhidecontext(1),
				errhidestmt(1));
#endif
		pgaio_submit_pending(false);
	}

	Assert(my_aio->pending_count < PGAIO_SUBMIT_BATCH_SIZE);

	/* for this module */
	io->system_referenced = true;
	io->op = op;
	Assert(io->owner_id == my_aio_id);

	WRITE_ONCE_F(io->flags) = PGAIOIP_PREP;
}

void
pgaio_io_unprepare(PgAioInProgress *io, PgAioOp op)
{
	Assert(io->op == op);
	Assert(io->owner_id == my_aio_id);
	Assert(io->flags == PGAIOIP_PREP);
	Assert(io->system_referenced);

	io->op = PGAIO_OP_INVALID;
	io->system_referenced = false;

	WRITE_ONCE_F(io->flags) = PGAIOIP_IDLE;
}

void
pgaio_io_stage(PgAioInProgress *io, PgAioSharedCallback scb)
{
	Assert(my_aio->pending_count < PGAIO_SUBMIT_BATCH_SIZE);
	Assert(io->flags == PGAIOIP_PREP);
	Assert(io->user_referenced);
	Assert(io->op != PGAIO_OP_INVALID);
	Assert(io->scb == PGAIO_SCB_INVALID);
	Assert(pgaio_shared_callback_op(scb) == io->op);

	io->scb = scb;

	WRITE_ONCE_F(io->flags) = (io->flags & ~PGAIOIP_PREP) | PGAIOIP_IN_PROGRESS | PGAIOIP_PENDING;
	dlist_push_tail(&my_aio->pending, &io->io_node);
	my_aio->pending_count++;

#ifdef PGAIO_VERBOSE
	if (message_level_is_interesting(DEBUG3))
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(ErrorContext);
		StringInfoData s;

		/*
		 * This code isn't ever allowed to error out, but the debugging
		 * output is super useful...
		 */
		START_CRIT_SECTION();

		initStringInfo(&s);

		pgaio_io_print(io, &s);

		ereport(DEBUG3,
				errmsg("staged %s",
					   s.data),
				errhidestmt(true),
				errhidecontext(true));
		pfree(s.data);
		MemoryContextSwitchTo(oldcontext);

		END_CRIT_SECTION();
	}
#endif
}

void
pgaio_io_prepare_submit(PgAioInProgress *io, uint32 ring)
{
	PgAioInProgress *cur;

	cur = io;

	while (true)
	{
		Assert(cur->flags & PGAIOIP_PENDING);
		Assert(!(cur->flags & PGAIOIP_PREP));
		Assert(!(cur->flags & PGAIOIP_IDLE));
		Assert(my_aio_id == cur->owner_id);

		cur->ring = ring;
		cur->submitter_id = my_aio_id;

		pg_write_barrier();

		WRITE_ONCE_F(cur->flags) =
			(cur->flags & ~PGAIOIP_PENDING) | PGAIOIP_INFLIGHT;

		dlist_delete_from(&my_aio->pending, &cur->io_node);
		my_aio->pending_count--;

		if (cur->user_referenced)
		{
			Assert(my_aio->outstanding_count > 0);
			dlist_delete_from(&my_aio->outstanding, &cur->owner_node);
			my_aio->outstanding_count--;

			dlist_push_tail(&my_aio->issued, &cur->owner_node);
			my_aio->issued_count++;
		}
		else
		{
#ifdef PGAIO_VERBOSE
			ereport(DEBUG4,
					errmsg("[%d] putting aio %zu/%llu onto issued_abandoned during submit",
						   cur->ring,
						   cur - aio_ctl->in_progress_io,
						   (long long unsigned) cur->generation),
					errhidecontext(1),
					errhidestmt(1));
#endif

			LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);
			dlist_push_tail(&my_aio->issued_abandoned, &cur->owner_node);
			my_aio->issued_abandoned_count++;
			LWLockRelease(SharedAIOCtlLock);
		}

		ereport(DEBUG5,
				errmsg("[%d] readied %zu/%llu for submit",
					   cur->ring,
					   cur - aio_ctl->in_progress_io,
					   (unsigned long long ) cur->generation),
				errhidecontext(1),
				errhidestmt(1));

		if (cur->merge_with_idx == PGAIO_MERGE_INVALID)
			break;
		cur = &aio_ctl->in_progress_io[cur->merge_with_idx];
	}
}

bool
pgaio_io_success(PgAioInProgress *io)
{
	Assert(io->user_referenced);
	Assert(io->flags & PGAIOIP_DONE);

	if (io->flags & (PGAIOIP_HARD_FAILURE | PGAIOIP_SOFT_FAILURE))
		return false;

	/* FIXME: is this possible? */
	if (!(io->flags & PGAIOIP_SHARED_CALLBACK_CALLED))
		return false;

	return true;
}

int
pgaio_io_result(PgAioInProgress *io)
{
	Assert(io->user_referenced);
	Assert(io->flags & PGAIOIP_DONE);

	/*
	 * FIXME: This will currently not return correct information for partial
	 * writes that were retried.
	 */
	return io->result;
}

bool
pgaio_io_done(PgAioInProgress *io)
{
	Assert(io->user_referenced);
	Assert(!(io->flags & PGAIOIP_UNUSED));

	if (io->flags & PGAIOIP_SOFT_FAILURE)
		return false;

	if (io->flags & (PGAIOIP_IDLE | PGAIOIP_HARD_FAILURE))
		return true;

	if (io->flags & PGAIOIP_DONE)
	{
		if (io->owner_id == my_aio_id &&
			!(io->flags & PGAIOIP_LOCAL_CALLBACK_CALLED))
			return false;
		return true;
	}

	return false;
}

bool
pgaio_io_pending(PgAioInProgress *io)
{
	Assert(io->user_referenced);
	Assert(!(io->flags & PGAIOIP_UNUSED));

	return io->flags & PGAIOIP_PENDING;
}

uint32
pgaio_io_id(PgAioInProgress *io)
{
	return io - aio_ctl->in_progress_io;
}

uint64
pgaio_io_generation(PgAioInProgress *io)
{
	return io->generation;
}

static void
pgaio_io_ref_internal(PgAioInProgress *io, PgAioIoRef *ref)
{
	Assert(io->flags & (PGAIOIP_IDLE | PGAIOIP_PREP | PGAIOIP_IN_PROGRESS | PGAIOIP_DONE));

	ref->aio_index = io - aio_ctl->in_progress_io;
	ref->generation_upper = (uint32) (io->generation >> 32);
	ref->generation_lower = (uint32) io->generation;
	Assert(io->generation != 0);
}

void
pgaio_io_ref(PgAioInProgress *io, PgAioIoRef *ref)
{
	Assert(io->user_referenced);
	pgaio_io_ref_internal(io, ref);
}

static inline PgAioInProgress *
pgaio_io_from_ref(PgAioIoRef *ref, uint64 *ref_generation)
{
	PgAioInProgress *io;

	Assert(ref->aio_index < max_aio_in_progress);

	io = &aio_ctl->in_progress_io[ref->aio_index];
	*ref_generation = ((uint64) ref->generation_upper) << 32 |
		ref->generation_lower;

	Assert(*ref_generation != 0);

	return io;
}

/*
 * Register a completion callback that is executed locally in the backend that
 * initiated the IO, even if the the completion of the IO has been reaped by
 * another process (which executed the shared callback, unlocking buffers
 * etc).  This is mainly useful for AIO using code to promptly react to
 * individual IOs finishing, without having to individually check each of the
 * IOs.
 */
void
pgaio_io_on_completion_local(PgAioInProgress *io, PgAioOnCompletionLocalContext *ocb)
{
	Assert(io->flags & (PGAIOIP_IDLE | PGAIOIP_PENDING));
	Assert(io->on_completion_local == NULL);

	io->on_completion_local = ocb;
}

void
pgaio_io_wait_ref(PgAioIoRef *ref, bool call_local)
{
	pgaio_io_wait_ref_int(ref, /* call_shared = */ true, call_local);
}

static void
pgaio_io_wait_ref_int(PgAioIoRef *ref, bool call_shared, bool call_local)
{
	uint64 ref_generation;
	PgAioInProgress *io;
	uint32 done_flags = PGAIOIP_DONE;
	PgAioIPFlags flags;
	bool am_owner;

	/*
	 * If we just wait for the IO to finish, we can only wait for the
	 * corresponding state.
	 */
	if (!call_shared)
	{
		Assert(!call_local);
		done_flags |= PGAIOIP_REAPED;
	}

	io = pgaio_io_from_ref(ref, &ref_generation);

	am_owner = io->owner_id == my_aio_id;

	if (pgaio_io_recycled(io, ref_generation, &flags))
		return;

	if (am_owner && (flags & PGAIOIP_PENDING))
	{
		Assert(call_shared);
		pgaio_submit_pending_internal(true, call_shared, call_local,
									  /* will_wait = */ true);
	}

	Assert(!am_owner || !(flags & PGAIOIP_IDLE));

	Assert(!(flags & (PGAIOIP_UNUSED)));

wait_ref_again:

	while (true)
	{
		if (pgaio_io_recycled(io, ref_generation, &flags))
			return;

		if (flags & done_flags)
			goto wait_ref_out;

		Assert(!(flags & (PGAIOIP_UNUSED)));

		if (flags & PGAIOIP_INFLIGHT)		/* XXX && pgaio_impl->XXX ?*/
		{
			pgaio_drain(&aio_ctl->contexts[io->ring],
						/* block = */ false,
						call_shared, call_local);

			if (pgaio_io_recycled(io, ref_generation, &flags))
				return;

			if (flags & done_flags)
				goto wait_ref_out;
		}

		if (my_aio->pending_count > 0 && call_shared)
		{
			/*
			 * If we otherwise would have to sleep submit all pending
			 * requests, to avoid others having to wait for us to submit
			 * them. Don't want to do so when not needing to sleep, as
			 * submitting IOs in smaller increments can be less efficient.
			 */
			pgaio_submit_pending_internal(call_shared, call_shared, call_local,
										  /* will_wait = */ false);
		}
		else if (pgaio_impl->wait_one && (flags & PGAIOIP_INFLIGHT))
		{
			/* note that this is allowed to spuriously return */
			pgaio_impl->wait_one(&aio_ctl->contexts[io->ring],
								 io,
								 ref_generation,
								 WAIT_EVENT_AIO_IO_COMPLETE_ANY);
		}
		else
		{
			if (call_shared)
				pgaio_complete_ios(false);

			/* ensure we're going to get woken up */
			if (IsUnderPostmaster)
				ConditionVariablePrepareToSleep(&io->cv);

			if (!pgaio_io_recycled(io, ref_generation, &flags) &&
				!(flags & done_flags))
			{
				/* shouldn't be reachable without concurrency */
				/*
				 * XXX: Assertion should be one level up, but right now we can
				 * get here with posix aio, even in single user mode. Probably
				 * need a proper branch to deal with REAPED but not yet
				 * consumed completions?
				 */
				Assert(IsUnderPostmaster);

				ConditionVariableSleep(&io->cv, WAIT_EVENT_AIO_IO_COMPLETE_ONE);
			}

			if (IsUnderPostmaster)
				ConditionVariableCancelSleepEx(true);
		}
	}

wait_ref_out:

	if (pgaio_io_recycled(io, ref_generation, &flags))
		return;

	/*
	 * If somebody else is retrying the IO, just wait from scratch.
	 */
	if (!(flags & done_flags))
	{
		Assert(flags & PGAIOIP_RETRY);
		goto wait_ref_again;
	}

	if (!call_shared)
	{
		Assert(flags & (PGAIOIP_REAPED | PGAIOIP_SHARED_CALLBACK_CALLED | PGAIOIP_DONE));
		return;
	}

	Assert(flags & PGAIOIP_DONE);

	/* can retry soft failures, but not hard ones */
	if (unlikely(flags & PGAIOIP_SOFT_FAILURE))
	{
		pgaio_io_retry_soft_failed(io, ref_generation);
		pgaio_io_wait_ref_int(ref, call_shared, call_local);

		return;
	}

	if (am_owner && call_local && !(flags & PGAIOIP_LOCAL_CALLBACK_CALLED))
	{
		if (flags & PGAIOIP_FOREIGN_DONE)
			pgaio_transfer_foreign_to_local();

		Assert(!(io->flags & PGAIOIP_FOREIGN_DONE));

		pgaio_io_call_local_callback(io, false);
	}
}

bool
pgaio_io_check_ref(PgAioIoRef *ref)
{
	uint64 ref_generation;
	PgAioInProgress *io;
	PgAioIPFlags flags;
	PgAioContext *context;

	io = pgaio_io_from_ref(ref, &ref_generation);

	if (pgaio_io_recycled(io, ref_generation, &flags))
		return true;

	if (flags & PGAIOIP_PENDING)
		return false;

	if (flags & PGAIOIP_SOFT_FAILURE)
		return false;

	if (flags & PGAIOIP_DONE)
		return true;

	context = &aio_ctl->contexts[io->ring];
	Assert(!(flags & (PGAIOIP_UNUSED)));

	/* recheck after determining the current context */
	if (pgaio_io_recycled(io, ref_generation, &flags))
		return true;

	if (flags & PGAIOIP_INFLIGHT)
		pgaio_drain(context,
					/* block = */ false,
					/* call_shared = */ true,
					/* call_local = */ false);

	if (pgaio_io_recycled(io, ref_generation, &flags))
		return true;

	if (flags & (PGAIOIP_SOFT_FAILURE | PGAIOIP_RETRY))
		return false;

	if (flags & PGAIOIP_DONE)
		return true;

	return false;
}


/*
 */
void
pgaio_io_wait(PgAioInProgress *io)
{
	PgAioIoRef ref;

	Assert(io->user_referenced && io->owner_id == my_aio_id);

	pgaio_io_ref(io, &ref);
	pgaio_io_wait_ref(&ref, /* call_local = */ true);
}

static void
pgaio_io_call_local_callback(PgAioInProgress *io, bool in_error)
{
	Assert(!(io->flags & PGAIOIP_LOCAL_CALLBACK_CALLED));
	Assert(io->user_referenced);

	Assert(my_aio->local_completed_count > 0);
	dlist_delete_from(&my_aio->local_completed, &io->io_node);
	Assert(my_aio->local_completed_count > 0);
	my_aio->local_completed_count--;

	dlist_delete_from(&my_aio->issued, &io->owner_node);
	Assert(my_aio->issued_count > 0);
	my_aio->issued_count--;
	dlist_push_tail(&my_aio->outstanding, &io->owner_node);
	my_aio->outstanding_count++;

	io->flags |= PGAIOIP_LOCAL_CALLBACK_CALLED;

	if (!io->on_completion_local)
		return;

	if (!in_error)
	{
		check_stack_depth();
		aio_local_callback_depth++;
		io->on_completion_local->callback(io->on_completion_local, io);
		Assert(aio_local_callback_depth > 0);
		aio_local_callback_depth--;
	}
}

static void
pgaio_io_retry_common(PgAioInProgress *io)
{
	/* we currently don't merge IOs during retries */
	Assert(!(io->flags & PGAIOIP_DONE));
	Assert(io->merge_with_idx == PGAIO_MERGE_INVALID);

	/*
	 * Need to enforce limit during retries too, otherwise submissions in
	 * other backends could lead us to exhaust resources.
	 */
	pgaio_apply_backend_limit();

#ifdef PGAIO_VERBOSE
	if (message_level_is_interesting(DEBUG2))
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(ErrorContext);
		StringInfoData s;

		initStringInfo(&s);

		pgaio_io_print(io, &s);

		ereport(DEBUG2,
				errmsg("retrying %s",
					   s.data),
				errhidestmt(true),
				errhidecontext(true));
		pfree(s.data);
		MemoryContextSwitchTo(oldcontext);
	}
#endif

	my_aio->retry_total_count++;

	io->submitter_id = my_aio_id;

	START_CRIT_SECTION();
	pgaio_impl->retry(io);
	END_CRIT_SECTION();
}

static void
pgaio_io_retry_soft_failed(PgAioInProgress *io, uint64 ref_generation)
{
	bool need_retry;
	PgAioIPFlags flags;

	LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);

	/* could concurrently have been unset / retried */
	if (!pgaio_io_recycled(io, ref_generation, &flags) &&
		(flags & PGAIOIP_DONE) &&
		(flags & (PGAIOIP_HARD_FAILURE | PGAIOIP_SOFT_FAILURE)))
	{
		Assert(!(io->flags & PGAIOIP_FOREIGN_DONE));
		Assert(!(io->flags & PGAIOIP_HARD_FAILURE));
		Assert(!(io->flags & PGAIOIP_REAPED));
		Assert(io->flags & PGAIOIP_DONE);

		dlist_delete(&io->io_node);

		WRITE_ONCE_F(io->flags) =
			(flags & ~(PGAIOIP_DONE |
					   PGAIOIP_FOREIGN_DONE |
					   PGAIOIP_SHARED_CALLBACK_CALLED |
					   PGAIOIP_LOCAL_CALLBACK_CALLED |
					   PGAIOIP_HARD_FAILURE |
					   PGAIOIP_SOFT_FAILURE)) |
			PGAIOIP_IN_PROGRESS |
			PGAIOIP_RETRY;

		need_retry = true;
	}
	else
	{
		need_retry = false;
	}
	LWLockRelease(SharedAIOCtlLock);

	if (!need_retry)
	{
		ereport(DEBUG2, errmsg("was about to retry %zd/%llu, but somebody else did already",
							   io - aio_ctl->in_progress_io,
							   (long long unsigned) ref_generation),
				errhidestmt(true),
				errhidecontext(true));
		return;
	}

	/* the io is still in-progress */
	Assert(io->system_referenced);

	pgaio_io_call_shared_retry(io);

	pgaio_io_retry_common(io);
}

/*
 * Retry failed IO. Needs to have been submitted by this backend, and
 * referenced resources, like fds, still need to be valid.
 */
void
pgaio_io_retry(PgAioInProgress *io)
{
	Assert(io->user_referenced);
	Assert(!io->system_referenced);
	Assert(io->owner_id == my_aio_id);
	Assert(io->flags & PGAIOIP_DONE);
	Assert(io->flags & PGAIOIP_LOCAL_CALLBACK_CALLED);
	Assert(io->flags & PGAIOIP_HARD_FAILURE);

	/*
	 * The IO has completed already, so we need to mark it as in-flight
	 * again.
	 */
	Assert(my_aio->outstanding_count > 0);
	dlist_delete_from(&my_aio->outstanding, &io->owner_node);
	my_aio->outstanding_count--;
	io->system_referenced = true;

	dlist_push_tail(&my_aio->issued, &io->owner_node);
	my_aio->issued_count++;

	WRITE_ONCE_F(io->flags) =
		(io->flags & ~(PGAIOIP_DONE |
					   PGAIOIP_FOREIGN_DONE |
					   PGAIOIP_SHARED_CALLBACK_CALLED |
					   PGAIOIP_LOCAL_CALLBACK_CALLED |
					   PGAIOIP_HARD_FAILURE |
					   PGAIOIP_SOFT_FAILURE)) |
		PGAIOIP_IN_PROGRESS |
		PGAIOIP_RETRY;

	/* can reuse original fd */

	pgaio_io_retry_common(io);
}


/* --------------------------------------------------------------------------------
 * Printing / Debugging output related routines.
 * --------------------------------------------------------------------------------
 */

void
pgaio_print_queues(void)
{
	StringInfoData s;
	uint32 inflight_backend = 0;
	uint32 *inflight_context;
	MemoryContext old_context;

	old_context = MemoryContextSwitchTo(ErrorContext);
	initStringInfo(&s);

	for (int procno = 0; procno < aio_ctl->backend_state_count; procno++)
	{
		PgAioPerBackend *bs = &aio_ctl->backend_state[procno];

		inflight_backend += pg_atomic_read_u32(&bs->inflight_count);
	}

	inflight_context = palloc0(sizeof(uint32) * aio_ctl->backend_state_count);
	for (int i = 0; i < max_aio_in_progress; i++)
	{
		PgAioInProgress *io = &aio_ctl->in_progress_io[i];

		if (!(io->flags & PGAIOIP_INFLIGHT))
			continue;
		inflight_context[io->ring]++;
	}

	appendStringInfo(&s, "inflight backend: %d", inflight_backend);

#ifdef USE_LIBURING
	for (int contextno = 0; contextno < aio_ctl->num_contexts; contextno++)
	{
		PgAioContext *context = &aio_ctl->contexts[contextno];

		appendStringInfo(&s, "\n\tqueue[%d]: sq ready/space: %d/%d, ready: %d, we think inflight: %d",
						 contextno,
						 io_uring_sq_ready(&context->io_uring_ring),
						 io_uring_sq_space_left(&context->io_uring_ring),
						 io_uring_cq_ready(&context->io_uring_ring),
						 inflight_context[contextno]);
	}
#endif

	ereport(LOG, errmsg_internal("%s", s.data),
			errhidestmt(true),
			errhidecontext(true));

	pfree(s.data);
	MemoryContextSwitchTo(old_context);
}

void
pgaio_io_flag_string(PgAioIPFlags flags, StringInfo s)
{
	bool first = true;

#define STRINGIFY_FLAG(f) if (flags & f) {  appendStringInfoString(s, first ? CppAsString(f) : " | " CppAsString(f)); first = false;}

	STRINGIFY_FLAG(PGAIOIP_UNUSED);
	STRINGIFY_FLAG(PGAIOIP_IDLE);
	STRINGIFY_FLAG(PGAIOIP_PREP);
	STRINGIFY_FLAG(PGAIOIP_IN_PROGRESS);
	STRINGIFY_FLAG(PGAIOIP_PENDING);
	STRINGIFY_FLAG(PGAIOIP_INFLIGHT);
	STRINGIFY_FLAG(PGAIOIP_REAPED);
	STRINGIFY_FLAG(PGAIOIP_SHARED_CALLBACK_CALLED);
	STRINGIFY_FLAG(PGAIOIP_LOCAL_CALLBACK_CALLED);

	STRINGIFY_FLAG(PGAIOIP_DONE);
	STRINGIFY_FLAG(PGAIOIP_FOREIGN_DONE);

	STRINGIFY_FLAG(PGAIOIP_RETRY);
	STRINGIFY_FLAG(PGAIOIP_HARD_FAILURE);
	STRINGIFY_FLAG(PGAIOIP_SOFT_FAILURE);

#undef STRINGIFY_FLAG
}

static void
pgaio_io_print_one(PgAioInProgress *io, StringInfo s)
{
	appendStringInfo(s, "aio %zu/%llu: op: %s, scb: %s, result: %d, ring: %d, owner: %d, flags: ",
					 io - aio_ctl->in_progress_io,
					 (long long unsigned) io->generation,
					 pgaio_io_operation_string(io->op),
					 pgaio_io_shared_callback_string(io->scb),
					 io->result,
					 io->ring,
					 io->owner_id);
	pgaio_io_flag_string(io->flags, s);
	appendStringInfo(s, ", user/system_referenced: %d/%d (",
					 io->user_referenced,
					 io->system_referenced);
	pgaio_io_call_shared_desc(io, s);
	appendStringInfoString(s, ")");
}

void
pgaio_io_ref_print(PgAioIoRef *ref, StringInfo s)
{
	PgAioInProgress *io;

	io = &aio_ctl->in_progress_io[ref->aio_index];

	pgaio_io_print(io, s);
}

void
pgaio_io_print(PgAioInProgress *io, StringInfo s)
{
	bool alloc = false;
	MemoryContext old_context;

	if (s == NULL)
	{
		old_context = MemoryContextSwitchTo(ErrorContext);
		s = makeStringInfo();
		alloc = true;
	}

	pgaio_io_print_one(io, s);

	if (io->merge_with_idx != PGAIO_MERGE_INVALID)
	{
		PgAioInProgress *cur = io;
		int nummerge = 0;

		appendStringInfoString(s, "\n  merge with:");

		while (cur->merge_with_idx != PGAIO_MERGE_INVALID)
		{
			PgAioInProgress *next;

			nummerge++;
			appendStringInfo(s, "\n    %d: ", nummerge);

			next = &aio_ctl->in_progress_io[cur->merge_with_idx];
			pgaio_io_print_one(next, s);

			cur = next;
		}
	}

	if (alloc)
	{
		ereport(LOG,
				errmsg("%s", s->data),
				errhidestmt(true),
				errhidecontext(true));
		pfree(s->data);
		pfree(s);
		MemoryContextReset(ErrorContext);
		MemoryContextSwitchTo(old_context);
	}
}

void
pgaio_print_list(dlist_head *head, StringInfo s, size_t offset)
{
	bool alloc = false;
	dlist_iter iter;
	bool first = true;
	MemoryContext old_context;

	if (s == NULL)
	{
		old_context = MemoryContextSwitchTo(ErrorContext);
		s = makeStringInfo();
		alloc = true;
	}

	dlist_foreach(iter, head)
	{
		PgAioInProgress *io = ((PgAioInProgress *) ((char *) (iter.cur) - offset));

		if (!first)
			appendStringInfo(s, "\n");
		first = false;

		pgaio_io_print(io, s);
	}

	if (alloc)
	{
		ereport(LOG,
				errmsg("%s", s->data),
				errhidestmt(true),
				errhidecontext(true));
		pfree(s->data);
		pfree(s);
		MemoryContextSwitchTo(old_context);
		MemoryContextReset(ErrorContext);
	}
}


/* --------------------------------------------------------------------------------
 * Bounce Buffer management.
 * --------------------------------------------------------------------------------
 */

PgAioBounceBuffer *
pgaio_bounce_buffer_get(void)
{
	PgAioBounceBuffer *bb = NULL;

	ResourceOwnerEnlargeAioBB(CurrentResourceOwner);

	while (true)
	{
		LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);
		if (!dlist_is_empty(&aio_ctl->unused_bounce_buffers))
		{
			dlist_node *node = dlist_pop_head_node(&aio_ctl->unused_bounce_buffers);

			aio_ctl->unused_bounce_buffers_count--;
			bb = dlist_container(PgAioBounceBuffer, node, node);

			Assert(pg_atomic_read_u32(&bb->refcount) == 0);
		}
		LWLockRelease(SharedAIOCtlLock);

		if (!bb)
		{
			elog(WARNING, "needed to drain while getting BB (inflight %d)",
				 pg_atomic_read_u32(&my_aio->inflight_count));

			for (int i = 0; i < aio_ctl->num_contexts; i++)
				pgaio_drain(&aio_ctl->contexts[i],
							/* block = */ true,
							/* call_shared = */ true,
							/* call_local = */ true);
		}
		else
			break;
	}

	pg_atomic_write_u32(&bb->refcount, 1);

	ResourceOwnerRememberAioBB(CurrentResourceOwner, bb);

	return bb;
}

static void
pgaio_bounce_buffer_release_internal(PgAioBounceBuffer *bb, bool holding_lock, bool release_resowner)
{
	Assert(holding_lock == LWLockHeldByMe(SharedAIOCtlLock));
	Assert(bb != NULL);

	if (release_resowner)
		ResourceOwnerForgetAioBB(CurrentResourceOwner, bb);

	if (pg_atomic_sub_fetch_u32(&bb->refcount, 1) != 0)
		return;

	if (!holding_lock)
		LWLockAcquire(SharedAIOCtlLock, LW_EXCLUSIVE);
	dlist_push_tail(&aio_ctl->unused_bounce_buffers, &bb->node);
	aio_ctl->unused_bounce_buffers_count++;
	if (!holding_lock)
		LWLockRelease(SharedAIOCtlLock);
}

void
pgaio_bounce_buffer_release(PgAioBounceBuffer *bb)
{
	pgaio_bounce_buffer_release_internal(bb,
										 /* holding_lock = */ false,
										 /* release_resowner */ true);
}

char *
pgaio_bounce_buffer_buffer(PgAioBounceBuffer *bb)
{
	return bb->buffer;
}

void
pgaio_assoc_bounce_buffer(PgAioInProgress *io, PgAioBounceBuffer *bb)
{
	Assert(bb != NULL);
	Assert(io->bb_idx == PGAIO_BB_INVALID);
	Assert(io->flags == PGAIOIP_IDLE);
	Assert(io->user_referenced);
	Assert(pg_atomic_read_u32(&bb->refcount) > 0);

	io->bb_idx = bb - aio_ctl->bounce_buffers;
	pg_atomic_fetch_add_u32(&bb->refcount, 1);
}

bool
pgaio_can_scatter_gather(void)
{
	/* XXX Caller should only use this for data files!  FIXME */
	if (io_direct_flags & IO_DIRECT_DATA)
		return pgaio_impl->can_scatter_gather_direct;
	return pgaio_impl->can_scatter_gather_buffered;
}
