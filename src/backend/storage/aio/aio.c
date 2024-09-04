/*-------------------------------------------------------------------------
 *
 * aio.c
 *    Asynchronous I/O subsytem.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/bufmgr.h"
#include "utils/resowner.h"
#include "utils/wait_event_types.h"



static void pgaio_io_reclaim(PgAioHandle *ioh);
static void pgaio_io_resowner_register(PgAioHandle *ioh);
static void pgaio_io_wait_for_free(void);
static PgAioHandle *pgaio_io_from_ref(PgAioHandleRef *ior, uint64 *ref_generation);

static void pgaio_bounce_buffer_wait_for_free(void);



/* Options for io_method. */
const struct config_enum_entry io_method_options[] = {
	{"sync", IOMETHOD_SYNC, false},
	{NULL, 0, false}
};

int			io_method = DEFAULT_IO_METHOD;
int			io_max_concurrency = -1;
int			io_bounce_buffers = -1;


/* global control for AIO */
PgAioCtl   *aio_ctl;

/* current backend's per-backend state */
PgAioPerBackend *my_aio;


static const IoMethodOps *pgaio_ops_table[] = {
	[IOMETHOD_SYNC] = &pgaio_sync_ops,
};


const IoMethodOps *pgaio_impl;



/* --------------------------------------------------------------------------------
 * "Core" IO Api
 * --------------------------------------------------------------------------------
 */

/*
 * AFIXME: rewrite
 *
 * Shared completion callbacks can be executed by any backend (otherwise there
 * would be deadlocks). Therefore they cannot update state for the issuer of
 * the IO. That can be done with issuer callbacks.
 *
 * Note that issuer callbacks are effectively executed in a critical
 * section. This is necessary as we need to be able to execute IO in critical
 * sections (consider e.g. WAL logging) and to be able to execute IOs we need
 * to acquire an IO, which in turn requires executing issuer callbacks. An
 * alternative scheme could be to defer local callback execution until a later
 * point, but that gets complicated quickly.
 *
 * Therefore the typical pattern is to use an issuer callback to set some
 * flags in backend local memory, which can then be used to error out at a
 * later time.
 *
 * NB: The issuer callback is cleared when the resowner owning the IO goes out
 * of scope.
 */
PgAioHandle *
pgaio_io_get(struct ResourceOwnerData *resowner, PgAioReturn *ret)
{
	PgAioHandle *h;

	while (true)
	{
		h = pgaio_io_get_nb(resowner, ret);

		if (h != NULL)
			return h;

		/*
		 * Evidently all handles by this backend are in use. Just wait for
		 * some to complete.
		 */
		pgaio_io_wait_for_free();
	}
}

PgAioHandle *
pgaio_io_get_nb(struct ResourceOwnerData *resowner, PgAioReturn *ret)
{
	if (my_aio->num_staged_ios >= PGAIO_SUBMIT_BATCH_SIZE)
	{
		Assert(my_aio->num_staged_ios == PGAIO_SUBMIT_BATCH_SIZE);
		pgaio_submit_staged();
	}

	if (my_aio->handed_out_io)
	{
		ereport(ERROR,
				errmsg("API violation: Only one IO can be handed out"));
	}

	if (!dclist_is_empty(&my_aio->idle_ios))
	{
		dlist_node *ion = dclist_pop_head_node(&my_aio->idle_ios);
		PgAioHandle *ioh = dclist_container(PgAioHandle, node, ion);

		Assert(ioh->state == AHS_IDLE);
		Assert(ioh->owner_procno == MyProcNumber);

		ioh->state = AHS_HANDED_OUT;
		my_aio->handed_out_io = ioh;

		if (resowner)
			pgaio_io_resowner_register(ioh);

		if (ret)
			ioh->report_return = ret;

		return ioh;
	}

	return NULL;
}

void
pgaio_io_release(PgAioHandle *ioh)
{
	if (ioh == my_aio->handed_out_io)
	{
		Assert(ioh->state == AHS_HANDED_OUT);
		Assert(ioh->resowner);

		my_aio->handed_out_io = NULL;
		pgaio_io_reclaim(ioh);
	}
	else
	{
		elog(ERROR, "release in unexpected state");
	}
}

void
pgaio_io_release_resowner(dlist_node *ioh_node, bool on_error)
{
	PgAioHandle *ioh = dlist_container(PgAioHandle, resowner_node, ioh_node);

	Assert(ioh->resowner);

	ResourceOwnerForgetAioHandle(ioh->resowner, &ioh->resowner_node);
	ioh->resowner = NULL;

	switch (ioh->state)
	{
		case AHS_IDLE:
			elog(ERROR, "unexpected");
			break;
		case AHS_HANDED_OUT:
			Assert(ioh == my_aio->handed_out_io || my_aio->handed_out_io == NULL);

			if (ioh == my_aio->handed_out_io)
			{
				my_aio->handed_out_io = NULL;
				if (!on_error)
					elog(WARNING, "leaked AIO handle");
			}

			pgaio_io_reclaim(ioh);
			break;
		case AHS_DEFINED:
		case AHS_PREPARED:
			/* XXX: Should we warn about this when is_commit? */
			pgaio_submit_staged();
			break;
		case AHS_IN_FLIGHT:
		case AHS_REAPED:
		case AHS_COMPLETED_SHARED:
			/* this is expected to happen */
			break;
		case AHS_COMPLETED_LOCAL:
			/* XXX: unclear if this ought to be possible? */
			pgaio_io_reclaim(ioh);
			break;
	}

	/*
	 * Need to unregister the reporting of the IO's result, the memory it's
	 * referencing likely has gone away.
	 */
	if (ioh->report_return)
		ioh->report_return = NULL;
}

int
pgaio_io_get_iovec(PgAioHandle *ioh, struct iovec **iov)
{
	Assert(ioh->state == AHS_HANDED_OUT);

	*iov = &aio_ctl->iovecs[ioh->iovec_off];

	/* AFIXME: Needs to be the value at startup time */
	return io_combine_limit;
}

PgAioSubjectData *
pgaio_io_get_subject_data(PgAioHandle *ioh)
{
	return &ioh->scb_data;
}

PgAioOpData *
pgaio_io_get_op_data(PgAioHandle *ioh)
{
	return &ioh->op_data;
}

ProcNumber
pgaio_io_get_owner(PgAioHandle *ioh)
{
	return ioh->owner_procno;
}

bool
pgaio_io_has_subject(PgAioHandle *ioh)
{
	return ioh->subject != ASI_INVALID;
}

void
pgaio_io_set_flag(PgAioHandle *ioh, PgAioHandleFlags flag)
{
	Assert(ioh->state == AHS_HANDED_OUT);

	ioh->flags |= flag;
}

void
pgaio_io_set_io_data_32(PgAioHandle *ioh, uint32 *data, uint8 len)
{
	Assert(ioh->state == AHS_HANDED_OUT);

	for (int i = 0; i < len; i++)
		aio_ctl->iovecs_data[ioh->iovec_off + i] = data[i];
	ioh->iovec_data_len = len;
}

uint64 *
pgaio_io_get_io_data(PgAioHandle *ioh, uint8 *len)
{
	Assert(ioh->iovec_data_len > 0);

	*len = ioh->iovec_data_len;

	return &aio_ctl->iovecs_data[ioh->iovec_off];
}

void
pgaio_io_set_subject(PgAioHandle *ioh, PgAioSubjectID subjid)
{
	Assert(ioh->state == AHS_HANDED_OUT);

	ioh->subject = subjid;

	elog(DEBUG3, "io:%d, op %s, subject %s, set subject",
		 pgaio_io_get_id(ioh),
		 pgaio_io_get_op_name(ioh),
		 pgaio_io_get_subject_name(ioh));
}

void
pgaio_io_get_ref(PgAioHandle *ioh, PgAioHandleRef *ior)
{
	Assert(ioh->state == AHS_HANDED_OUT ||
		   ioh->state == AHS_DEFINED ||
		   ioh->state == AHS_PREPARED);
	Assert(ioh->generation != 0);

	ior->aio_index = ioh - aio_ctl->io_handles;
	ior->generation_upper = (uint32) (ioh->generation >> 32);
	ior->generation_lower = (uint32) ioh->generation;
}

void
pgaio_io_ref_clear(PgAioHandleRef *ior)
{
	ior->aio_index = PG_UINT32_MAX;
}

bool
pgaio_io_ref_valid(PgAioHandleRef *ior)
{
	return ior->aio_index != PG_UINT32_MAX;
}

int
pgaio_io_ref_get_id(PgAioHandleRef *ior)
{
	Assert(pgaio_io_ref_valid(ior));
	return ior->aio_index;
}

bool
pgaio_io_was_recycled(PgAioHandle *ioh, uint64 ref_generation, PgAioHandleState *state)
{
	*state = ioh->state;
	pg_read_barrier();

	return ioh->generation != ref_generation;
}

void
pgaio_io_ref_wait(PgAioHandleRef *ior)
{
	uint64		ref_generation;
	PgAioHandleState state;
	bool		am_owner;
	PgAioHandle *ioh;

	ioh = pgaio_io_from_ref(ior, &ref_generation);

	am_owner = ioh->owner_procno == MyProcNumber;


	if (pgaio_io_was_recycled(ioh, ref_generation, &state))
		return;

	if (am_owner)
	{
		if (state == AHS_DEFINED || state == AHS_PREPARED)
		{
			/* XXX: Arguably this should be prevented by callers? */
			pgaio_submit_staged();
		}
		else if (state != AHS_IN_FLIGHT && state != AHS_REAPED && state != AHS_COMPLETED_SHARED && state != AHS_COMPLETED_LOCAL)
		{
			elog(PANIC, "waiting for own IO in wrong state: %d",
				 state);
		}

		/*
		 * Somebody else completed the IO, need to execute issuer callback, so
		 * reclaim eagerly.
		 */
		if (state == AHS_COMPLETED_LOCAL)
		{
			pgaio_io_reclaim(ioh);

			return;
		}
	}

	while (true)
	{
		if (pgaio_io_was_recycled(ioh, ref_generation, &state))
			return;

		switch (state)
		{
			case AHS_IDLE:
			case AHS_HANDED_OUT:
				elog(ERROR, "IO in wrong state: %d", state);
				break;

			case AHS_IN_FLIGHT:
				if (pgaio_impl->wait_one)
				{
					pgaio_impl->wait_one(ioh, ref_generation);
					continue;
				}
				/* fallthrough */

				/* waiting for owner to submit */
			case AHS_PREPARED:
			case AHS_DEFINED:
				/* waiting for reaper to complete */
				/* fallthrough */
			case AHS_REAPED:
				/* shouldn't be able to hit this otherwise */
				Assert(IsUnderPostmaster);
				/* ensure we're going to get woken up */
				ConditionVariablePrepareToSleep(&ioh->cv);

				while (!pgaio_io_was_recycled(ioh, ref_generation, &state))
				{
					if (state != AHS_REAPED && state != AHS_DEFINED &&
						state != AHS_IN_FLIGHT)
						break;
					ConditionVariableSleep(&ioh->cv, WAIT_EVENT_AIO_COMPLETION);
				}

				ConditionVariableCancelSleep();
				break;

			case AHS_COMPLETED_SHARED:
				/* see above */
				if (am_owner)
					pgaio_io_reclaim(ioh);
				return;
			case AHS_COMPLETED_LOCAL:
				return;
		}
	}
}

bool
pgaio_io_ref_check_done(PgAioHandleRef *ior)
{
	uint64		ref_generation;
	PgAioHandleState state;
	bool		am_owner;
	PgAioHandle *ioh;

	ioh = pgaio_io_from_ref(ior, &ref_generation);

	if (pgaio_io_was_recycled(ioh, ref_generation, &state))
		return true;


	if (state == AHS_IDLE)
		return true;

	am_owner = ioh->owner_procno == MyProcNumber;

	if (state == AHS_COMPLETED_SHARED || state == AHS_COMPLETED_LOCAL)
	{
		if (am_owner)
			pgaio_io_reclaim(ioh);
		return true;
	}

	return false;
}

int
pgaio_io_get_id(PgAioHandle *ioh)
{
	Assert(ioh >= aio_ctl->io_handles &&
		   ioh <= (aio_ctl->io_handles + aio_ctl->io_handle_count));
	return ioh - aio_ctl->io_handles;
}

const char *
pgaio_io_get_state_name(PgAioHandle *ioh)
{
	switch (ioh->state)
	{
		case AHS_IDLE:
			return "idle";
		case AHS_HANDED_OUT:
			return "handed_out";
		case AHS_DEFINED:
			return "DEFINED";
		case AHS_PREPARED:
			return "PREPARED";
		case AHS_IN_FLIGHT:
			return "IN_FLIGHT";
		case AHS_REAPED:
			return "REAPED";
		case AHS_COMPLETED_SHARED:
			return "COMPLETED_SHARED";
		case AHS_COMPLETED_LOCAL:
			return "COMPLETED_LOCAL";
	}
	pg_unreachable();
}

/*
 * Internal, should only be called from pgaio_io_prep_*().
 */
void
pgaio_io_prepare(PgAioHandle *ioh, PgAioOp op)
{
	Assert(ioh->state == AHS_HANDED_OUT);
	Assert(pgaio_io_has_subject(ioh));

	ioh->op = op;
	ioh->state = AHS_DEFINED;
	ioh->result = 0;

	/* allow a new IO to be staged */
	my_aio->handed_out_io = NULL;

	pgaio_io_prepare_subject(ioh);

	ioh->state = AHS_PREPARED;

	elog(DEBUG3, "io:%d: prepared %s",
		 pgaio_io_get_id(ioh), pgaio_io_get_op_name(ioh));

	if (!pgaio_io_needs_synchronous_execution(ioh))
	{
		my_aio->staged_ios[my_aio->num_staged_ios++] = ioh;
		Assert(my_aio->num_staged_ios <= PGAIO_SUBMIT_BATCH_SIZE);
	}
	else
	{
		pgaio_io_prepare_submit(ioh);
		pgaio_io_perform_synchronously(ioh);
	}
}

/*
 * Handle IO getting completed by a method.
 */
void
pgaio_io_process_completion(PgAioHandle *ioh, int result)
{
	Assert(ioh->state == AHS_IN_FLIGHT);

	ioh->result = result;

	pg_write_barrier();

	/* FIXME: should be done in separate function */
	ioh->state = AHS_REAPED;

	pgaio_io_process_completion_subject(ioh);

	/* ensure results of completion are visible before the new state */
	pg_write_barrier();

	ioh->state = AHS_COMPLETED_SHARED;

	/* condition variable broadcast ensures state is visible before wakeup */
	ConditionVariableBroadcast(&ioh->cv);

	if (ioh->owner_procno == MyProcNumber)
		pgaio_io_reclaim(ioh);
}

bool
pgaio_io_needs_synchronous_execution(PgAioHandle *ioh)
{
	if (pgaio_impl->needs_synchronous_execution)
		return pgaio_impl->needs_synchronous_execution(ioh);
	return false;
}

/*
 * Handle IO being processed by IO method.
 */
void
pgaio_io_prepare_submit(PgAioHandle *ioh)
{
	ioh->state = AHS_IN_FLIGHT;
	pg_write_barrier();
}

static PgAioHandle *
pgaio_io_from_ref(PgAioHandleRef *ior, uint64 *ref_generation)
{
	PgAioHandle *ioh;

	Assert(ior->aio_index < aio_ctl->io_handle_count);

	ioh = &aio_ctl->io_handles[ior->aio_index];

	*ref_generation = ((uint64) ior->generation_upper) << 32 |
		ior->generation_lower;

	Assert(*ref_generation != 0);

	return ioh;
}

static void
pgaio_io_resowner_register(PgAioHandle *ioh)
{
	Assert(!ioh->resowner);
	Assert(CurrentResourceOwner);

	ResourceOwnerRememberAioHandle(CurrentResourceOwner, &ioh->resowner_node);
	ioh->resowner = CurrentResourceOwner;
}

static void
pgaio_io_reclaim(PgAioHandle *ioh)
{
	/* This is only ok if it's our IO */
	Assert(ioh->owner_procno == MyProcNumber);

	ereport(DEBUG3,
			errmsg("reclaiming io:%d, state: %s, op %s, subject %s, result: %d, distilled_result: AFIXME, report to: %p",
				   pgaio_io_get_id(ioh),
				   pgaio_io_get_state_name(ioh),
				   pgaio_io_get_op_name(ioh),
				   pgaio_io_get_subject_name(ioh),
				   ioh->result,
				   ioh->report_return
				   ),
			errhidestmt(true), errhidecontext(true));

	if (ioh->report_return)
	{
		if (ioh->state != AHS_HANDED_OUT)
		{
			ioh->report_return->result = ioh->distilled_result;
			ioh->report_return->subject_data = ioh->scb_data;
		}
	}

	/* reclaim all associated bounce buffers */
	if (!slist_is_empty(&ioh->bounce_buffers))
	{
		slist_mutable_iter it;

		slist_foreach_modify(it, &ioh->bounce_buffers)
		{
			PgAioBounceBuffer *bb = slist_container(PgAioBounceBuffer, node, it.cur);

			slist_delete_current(&it);

			slist_push_head(&my_aio->idle_bbs, &bb->node);
		}
	}

	if (ioh->resowner)
	{
		ResourceOwnerForgetAioHandle(ioh->resowner, &ioh->resowner_node);
		ioh->resowner = NULL;
	}

	Assert(!ioh->resowner);

	ioh->num_shared_callbacks = 0;
	ioh->iovec_data_len = 0;
	ioh->report_return = NULL;
	ioh->flags = 0;

	pg_write_barrier();
	ioh->generation++;
	pg_write_barrier();
	ioh->state = AHS_IDLE;
	pg_write_barrier();

	dclist_push_tail(&my_aio->idle_ios, &ioh->node);
}

static void
pgaio_io_wait_for_free(void)
{
	bool		found_handed_out = false;
	int			reclaimed = 0;
	static uint32 lastpos = 0;

	elog(DEBUG2,
		 "waiting for self: %d pending",
		 my_aio->num_staged_ios);

	/*
	 * First check if any of our IOs actually have completed - when using
	 * worker, that'll often be the case. We could do so as part of the loop
	 * below, but that'd potentially lead us to wait for some IO submitted
	 * before.
	 */
	for (int i = 0; i < io_max_concurrency; i++)
	{
		PgAioHandle *ioh = &aio_ctl->io_handles[my_aio->io_handle_off + i];

		if (ioh->state == AHS_COMPLETED_SHARED)
		{
			pgaio_io_reclaim(ioh);
			reclaimed++;
		}
	}

	if (reclaimed > 0)
		return;

	if (my_aio->num_staged_ios > 0)
	{
		elog(DEBUG2, "submitting while acquiring free io");
		pgaio_submit_staged();
	}

	for (uint32 i = lastpos; i < lastpos + io_max_concurrency; i++)
	{
		uint32		thisoff = my_aio->io_handle_off + (i % io_max_concurrency);
		PgAioHandle *ioh = &aio_ctl->io_handles[thisoff];

		switch (ioh->state)
		{
			case AHS_IDLE:

				/*
				 * While one might think that pgaio_io_get_nb() should have
				 * succeeded, this is reachable because the IO could have
				 * completed during the submission above.
				 */
				return;
			case AHS_DEFINED:	/* should have been submitted above */
			case AHS_PREPARED:
			case AHS_COMPLETED_LOCAL:
				elog(ERROR, "shouldn't get here with io:%d in state %d",
					 pgaio_io_get_id(ioh), ioh->state);
				break;
			case AHS_HANDED_OUT:
				if (found_handed_out)
					elog(ERROR, "more than one handed out IO");
				found_handed_out = true;
				continue;
			case AHS_REAPED:
			case AHS_IN_FLIGHT:
				{
					PgAioHandleRef ior;

					ior.aio_index = ioh - aio_ctl->io_handles;
					ior.generation_upper = (uint32) (ioh->generation >> 32);
					ior.generation_lower = (uint32) ioh->generation;

					pgaio_io_ref_wait(&ior);
					elog(DEBUG2, "waited for io:%d",
						 pgaio_io_get_id(ioh));
					lastpos = i;
					return;
				}
				break;
			case AHS_COMPLETED_SHARED:
				/* reclaim */
				pgaio_io_reclaim(ioh);
				lastpos = i;
				return;
		}
	}

	elog(PANIC, "could not reclaim any handles");
}



/* --------------------------------------------------------------------------------
 * Bounce Buffers
 * --------------------------------------------------------------------------------
 */

PgAioBounceBuffer *
pgaio_bounce_buffer_get(void)
{
	PgAioBounceBuffer *bb = NULL;
	slist_node *node;

	if (my_aio->handed_out_bb != NULL)
		elog(ERROR, "can only hand out one BB");

	/*
	 * FIXME It probably is not correct to have bounce buffers be per backend,
	 * they use too much memory.
	 */
	if (slist_is_empty(&my_aio->idle_bbs))
	{
		pgaio_bounce_buffer_wait_for_free();
	}

	node = slist_pop_head_node(&my_aio->idle_bbs);
	bb = slist_container(PgAioBounceBuffer, node, node);

	my_aio->handed_out_bb = bb;

	bb->resowner = CurrentResourceOwner;
	ResourceOwnerRememberAioBounceBuffer(bb->resowner, &bb->resowner_node);

	return bb;
}

void
pgaio_io_assoc_bounce_buffer(PgAioHandle *ioh, PgAioBounceBuffer *bb)
{
	if (my_aio->handed_out_bb != bb)
		elog(ERROR, "can only assign handed out BB");
	my_aio->handed_out_bb = NULL;

	/*
	 * There can be many bounce buffers assigned in case of vectorized IOs.
	 */
	slist_push_head(&ioh->bounce_buffers, &bb->node);

	/* once associated with an IO, the IO has ownership */
	ResourceOwnerForgetAioBounceBuffer(bb->resowner, &bb->resowner_node);
	bb->resowner = NULL;
}

uint32
pgaio_bounce_buffer_id(PgAioBounceBuffer *bb)
{
	return bb - aio_ctl->bounce_buffers;
}

void
pgaio_bounce_buffer_release(PgAioBounceBuffer *bb)
{
	if (my_aio->handed_out_bb != bb)
		elog(ERROR, "can only release handed out BB");

	slist_push_head(&my_aio->idle_bbs, &bb->node);
	my_aio->handed_out_bb = NULL;

	ResourceOwnerForgetAioBounceBuffer(bb->resowner, &bb->resowner_node);
	bb->resowner = NULL;
}

void
pgaio_bounce_buffer_release_resowner(dlist_node *bb_node, bool on_error)
{
	PgAioBounceBuffer *bb = dlist_container(PgAioBounceBuffer, resowner_node, bb_node);

	Assert(bb->resowner);

	if (!on_error)
		elog(WARNING, "leaked AIO bounce buffer");

	pgaio_bounce_buffer_release(bb);
}

char *
pgaio_bounce_buffer_buffer(PgAioBounceBuffer *bb)
{
	return bb->buffer;
}

static void
pgaio_bounce_buffer_wait_for_free(void)
{
	static uint32 lastpos = 0;

	if (my_aio->num_staged_ios > 0)
	{
		elog(DEBUG2, "submitting while acquiring free bb");
		pgaio_submit_staged();
	}

	for (uint32 i = lastpos; i < lastpos + io_max_concurrency; i++)
	{
		uint32		thisoff = my_aio->io_handle_off + (i % io_max_concurrency);
		PgAioHandle *ioh = &aio_ctl->io_handles[thisoff];

		switch (ioh->state)
		{
			case AHS_IDLE:
			case AHS_HANDED_OUT:
				continue;
			case AHS_DEFINED:	/* should have been submitted above */
			case AHS_PREPARED:
				elog(ERROR, "shouldn't get here with io:%d in state %d",
					 pgaio_io_get_id(ioh), ioh->state);
				break;
			case AHS_REAPED:
			case AHS_IN_FLIGHT:
				if (!slist_is_empty(&ioh->bounce_buffers))
				{
					PgAioHandleRef ior;

					ior.aio_index = ioh - aio_ctl->io_handles;
					ior.generation_upper = (uint32) (ioh->generation >> 32);
					ior.generation_lower = (uint32) ioh->generation;

					pgaio_io_ref_wait(&ior);
					elog(DEBUG2, "waited for io:%d to reclaim BB",
						 pgaio_io_get_id(ioh));

					if (slist_is_empty(&my_aio->idle_bbs))
						elog(WARNING, "empty after wait");

					if (!slist_is_empty(&my_aio->idle_bbs))
					{
						lastpos = i;
						return;
					}
				}
				break;
			case AHS_COMPLETED_SHARED:
			case AHS_COMPLETED_LOCAL:
				/* reclaim */
				pgaio_io_reclaim(ioh);

				if (!slist_is_empty(&my_aio->idle_bbs))
				{
					lastpos = i;
					return;
				}
				break;
		}
	}

	/*
	 * The submission above could have caused the IO to complete at any time.
	 */
	if (slist_is_empty(&my_aio->idle_bbs))
		elog(PANIC, "no more bbs");
}



/* --------------------------------------------------------------------------------
 * Actions on multiple IOs.
 * --------------------------------------------------------------------------------
 */

void
pgaio_submit_staged(void)
{
	int			total_submitted = 0;
	int			did_submit;

	if (my_aio->num_staged_ios == 0)
		return;


	START_CRIT_SECTION();

	did_submit = pgaio_impl->submit(my_aio->num_staged_ios, my_aio->staged_ios);

	END_CRIT_SECTION();

	total_submitted += did_submit;

	Assert(total_submitted == did_submit);

	my_aio->num_staged_ios = 0;

#ifdef PGAIO_VERBOSE
	ereport(DEBUG2,
			errmsg("submitted %d",
				   total_submitted),
			errhidestmt(true),
			errhidecontext(true));
#endif
}

bool
pgaio_have_staged(void)
{
	return my_aio->num_staged_ios > 0;
}



/* --------------------------------------------------------------------------------
 * Other
 * --------------------------------------------------------------------------------
 */

/*
 * Need to submit staged but not yet submitted IOs using the fd, otherwise
 * the IO would end up targeting something bogus.
 */
void
pgaio_closing_fd(int fd)
{
	/*
	 * Might be called before AIO is initialized or in a subprocess that
	 * doesn't use AIO.
	 */
	if (!my_aio)
		return;

	/*
	 * For now just submit all staged IOs - we could be more selective, but
	 * it's probably not worth it.
	 */
	pgaio_submit_staged();
}

void
pgaio_at_xact_end(bool is_subxact, bool is_commit)
{
	Assert(!my_aio->handed_out_io);
	Assert(!my_aio->handed_out_bb);
}

/*
 * Similar to pgaio_at_xact_end(..., is_commit = false), but for cases where
 * errors happen outside of transactions.
 */
void
pgaio_at_error(void)
{
	Assert(!my_aio->handed_out_io);
	Assert(!my_aio->handed_out_bb);
}


void
assign_io_method(int newval, void *extra)
{
	pgaio_impl = pgaio_ops_table[newval];
}
