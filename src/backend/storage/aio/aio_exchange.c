/*-------------------------------------------------------------------------
 *
 * aio_exchange.c
 *	  Routines for coordinating which backend will complete an IO.
 *
 * This module is used by implementations where only the backend that submits
 * an IO is allowed to consume the result from the kernel.  In order to avoid
 * deadlocks, backends are allowed to interrupt others to ask for the result.
 *
 * Portions Copyright (c) 2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/aio_exchange.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/aio_internal.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "utils/wait_event.h"

static volatile sig_atomic_t pgaio_exchange_interrupt_pending;
static volatile sig_atomic_t pgaio_exchange_interrupt_holdoff;

static bool
pgaio_exchange_become_completer(PgAioInProgress *io)
{
	return !pg_atomic_exchange_u32(&io->interlock.exchange.have_completer, true);
}

static void
pgaio_exchange_reneg_completer(PgAioInProgress *io)
{
	pg_atomic_write_u32(&io->interlock.exchange.have_completer, false);
	ConditionVariableBroadcast(&io->cv);
}

void
pgaio_exchange_shmem_init(bool first_time)
{
	if (first_time)
	{
		for (int i = 0; i < max_aio_in_progress; i++)
		{
			PgAioInProgress *io = &aio_ctl->in_progress_io[i];

			pg_atomic_init_u32(&io->interlock.exchange.have_completer, false);
			io->interlock.exchange.result = INT_MIN;
		}
	}
}

void
pgaio_exchange_submit_one(PgAioInProgress *io)
{
	for (PgAioInProgress * cur = io;;)
	{
		cur->interlock.exchange.head_idx = pgaio_io_id(io);
		if (cur->merge_with_idx == PGAIO_MERGE_INVALID)
			break;
		cur = &aio_ctl->in_progress_io[cur->merge_with_idx];
	}

	pg_atomic_write_u32(&io->interlock.exchange.have_completer, false);
	io->interlock.exchange.result = INT_MIN;
}

void
pgaio_exchange_process_completion(PgAioInProgress * io,
								  int result,
								  bool in_interrupt_handler)
{
	if (unlikely(in_interrupt_handler))
	{
		/*
		 * Can't process the completion from here, but we can make it
		 * available to other backends with operations that are safe from a
		 * signal handler.
		 */
		Assert(io->submitter_id == my_aio_id);
		Assert(io->interlock.exchange.result == INT_MIN);
		io->interlock.exchange.result = result;
		ConditionVariableSignalFromSignalHandler(&io->cv);
		return;
	}

	io->interlock.exchange.result = INT_MIN;

	pgaio_process_io_completion(io, result);
}

static void
pgaio_exchange_wait_one_local(PgAioInProgress * io, uint64 ref_generation, uint32 wait_event_info)
{
	PgAioInProgress *head_io;
	PgAioIPFlags flags;
	int			result;

	/*
	 * Prevent the interrupt handler from setting any results, so that we can
	 * check for results it's already written without races.
	 */
	pgaio_exchange_disable_interrupt();

	/* Find the head of the merge chain (could be out of date). */
	head_io = &aio_ctl->in_progress_io[io->interlock.exchange.head_idx];

	/* We might have stored the result already, in the interrupt handler. */
	if ((result = head_io->interlock.exchange.result) != INT_MIN)
	{
		if (pgaio_exchange_become_completer(head_io))
		{
			/* We won the right to complete it. */
			if (pgaio_io_recycled(io, ref_generation, &flags) ||
				!(flags & PGAIOIP_INFLIGHT))
				pgaio_exchange_reneg_completer(head_io);
			else
				pgaio_exchange_process_completion(head_io, result, false);
			goto out;
		}
		else
		{
			/* Someone beat us.  Wait for progress. */
			pgaio_exchange_enable_interrupt();

			ConditionVariablePrepareToSleep(&head_io->cv);
			if (!pgaio_io_recycled(io, ref_generation, &flags) &&
				(flags & PGAIOIP_INFLIGHT))
				ConditionVariableSleep(&head_io->cv, wait_event_info);
			ConditionVariableCancelSleep();
			return;
		}
	}

	/* Keep draining until the IO is no longer in flight. */
	for (;;)
	{
		if (pgaio_io_recycled(io, ref_generation, &flags) ||
			!(flags & PGAIOIP_INFLIGHT))
			break;
		pgaio_drain(NULL,
					/* block = */ true,
					/* call_shared = */ false,
					/* call_local = */ false);
	}

 out:
	pgaio_exchange_enable_interrupt();
}

static void
pgaio_exchange_wait_one_foreign(PgAioInProgress * io, uint64 ref_generation, uint32 wait_event_info)
{
	uint32 head_idx;
	PgAioInProgress *head_io;
	PgAioIPFlags flags;

	/* Find the head of the merge chain (could be out of date if gen advanced). */
	head_idx = io->interlock.exchange.head_idx;
	head_io = &aio_ctl->in_progress_io[head_idx];

	/*
	 * Wait for the submitter to tell us the result, or to process completions
	 * itself and wake us up.  Interrupt it periodically, to make sure that it
	 * eventually does that even if it's blocked (perhaps on a lock we hold).
	 * Start with 10ms and back off until we reach 1 second.
	 */
	ConditionVariablePrepareToSleep(&head_io->cv);
	for (int backoff = 10;; backoff = Min(backoff * 2, 1000))
	{
		int			result;

		if (pgaio_io_recycled(io, ref_generation, &flags) ||
			!(flags & PGAIOIP_INFLIGHT))
			break;

		/* See if the submitter has put the result in shared memory. */
		result = head_io->interlock.exchange.result;
		if (result != INT_MIN)
		{
			if (pgaio_exchange_become_completer(head_io))
			{
				/* We won the right to complete it. */
				if (pgaio_io_recycled(io, ref_generation, &flags) ||
					!(flags & PGAIOIP_INFLIGHT))
					pgaio_exchange_reneg_completer(head_io);
				else
					pgaio_exchange_process_completion(head_io, result, false);
				break;
			}
		}
		else
		{
			/*
			 * Ask the submitter to drain.
			 *
			 * XXX It might be nice to coordinate so that multiple waiters
			 * don't become a thundering herd.
			 *
			 * XXX Expose whether the submitter has interrupts disabled, and
			 * do a double-checked flag dance so that we can avoid
			 * interrupting the submitter if it's blocked in drain()
			 * (hopefully a common case).
			 */
			SendProcSignal(ProcGlobal->allProcs[head_io->submitter_id].pid,
						   PROCSIG_AIO_INTERRUPT,
						   InvalidBackendId);
		}

		ConditionVariableTimedSleep(&io->cv, backoff, wait_event_info);
	}
	ConditionVariableCancelSleep();
}

void
pgaio_exchange_wait_one(PgAioContext *context,
						PgAioInProgress * io,
						uint64 ref_generation,
						uint32 wait_event_info)
{
	/*
	 * XXX Would it be possible for us to see an out of date submitter_id,
	 * after the IO was retried by another backend?  Should we clear our own
	 * submitter_id when completing to avoid that?
	 */

	if (io->submitter_id == my_aio_id)
		pgaio_exchange_wait_one_local(io, ref_generation, wait_event_info);
	else
		pgaio_exchange_wait_one_foreign(io, ref_generation, wait_event_info);
}

/*
 * Disable interrupt processing.
 */
void
pgaio_exchange_disable_interrupt(void)
{
	pgaio_exchange_interrupt_holdoff++;
}

/*
 * Re-enable interrupt processing, and handle any interrupts we missed.
 */
void
pgaio_exchange_enable_interrupt(void)
{
	Assert(pgaio_exchange_interrupt_holdoff > 0);
	if (--pgaio_exchange_interrupt_holdoff == 0)
	{
		while (pgaio_exchange_interrupt_pending)
		{
			pgaio_exchange_interrupt_pending = false;
			pgaio_exchange_interrupt_holdoff++;
			pgaio_drain_in_interrupt_handler();
			pgaio_exchange_interrupt_holdoff--;
		}
	}
}

/*
 * Handler for PROCSIG_AIO_INTERRUPT.  Called in signal handler when another
 * backend is blocked waiting for an IO that this backend submitted.
 */
void
HandleAioInterrupt(void)
{
	/*
	 * If exchange interrupts are currently disabled, just remember that the
	 * interrupt arrived.  It'll be processed when they're reenabled.
	 */
	if (pgaio_exchange_interrupt_holdoff > 0)
	{
		pgaio_exchange_interrupt_pending = true;
		return;
	}
	pgaio_drain_in_interrupt_handler();
}
