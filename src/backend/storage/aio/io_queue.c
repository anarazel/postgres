/*-------------------------------------------------------------------------
 *
 * io_queue.c
 *	  AIO - Mechanism for tracking many IOs
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/io_queue.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/ilist.h"
#include "storage/aio.h"
#include "storage/io_queue.h"
#include "utils/resowner.h"



typedef struct TrackedIO
{
	PgAioWaitRef iow;
	dlist_node	node;
} TrackedIO;

struct IOQueue
{
	int			depth;
	int			unsubmitted;

	bool		has_reserved;

	dclist_head idle;
	dclist_head in_progress;

	TrackedIO	tracked_ios[FLEXIBLE_ARRAY_MEMBER];
};


IOQueue *
io_queue_create(int depth, int flags)
{
	size_t		sz;
	IOQueue    *ioq;

	sz = offsetof(IOQueue, tracked_ios)
		+ sizeof(TrackedIO) * depth;

	ioq = palloc0(sz);

	ioq->depth = 0;

	for (int i = 0; i < depth; i++)
	{
		TrackedIO  *tio = &ioq->tracked_ios[i];

		pgaio_wref_clear(&tio->iow);
		dclist_push_tail(&ioq->idle, &tio->node);
	}

	return ioq;
}

void
io_queue_wait_one(IOQueue *ioq)
{
	/* submit all pending IO before waiting */
	pgaio_submit_staged();

	while (!dclist_is_empty(&ioq->in_progress))
	{
		/* FIXME: Should we really pop here already? */
		dlist_node *node = dclist_pop_head_node(&ioq->in_progress);
		TrackedIO  *tio = dclist_container(TrackedIO, node, node);

		pgaio_wref_wait(&tio->iow);
		dclist_push_head(&ioq->idle, &tio->node);
	}
}

void
io_queue_reserve(IOQueue *ioq)
{
	if (ioq->has_reserved)
		return;

	if (dclist_is_empty(&ioq->idle))
		io_queue_wait_one(ioq);

	Assert(!dclist_is_empty(&ioq->idle));

	ioq->has_reserved = true;
}

PgAioHandle *
io_queue_acquire_io(IOQueue *ioq)
{
	PgAioHandle *ioh;

	io_queue_reserve(ioq);

	Assert(!dclist_is_empty(&ioq->idle));

	if (!io_queue_is_empty(ioq))
	{
		ioh = pgaio_io_acquire_nb(CurrentResourceOwner, NULL);
		if (ioh == NULL)
		{
			/*
			 * Need to wait for all IOs, blocking might not be legal in the
			 * context.
			 *
			 * XXX: This doesn't make a whole lot of sense, we're also
			 * blocking here. What was I smoking when I wrote the above?
			 */
			io_queue_wait_all(ioq);
			ioh = pgaio_io_acquire(CurrentResourceOwner, NULL);
		}
	}
	else
	{
		ioh = pgaio_io_acquire(CurrentResourceOwner, NULL);
	}

	return ioh;
}

void
io_queue_track(IOQueue *ioq, const struct PgAioWaitRef *iow)
{
	dlist_node *node;
	TrackedIO  *tio;

	Assert(ioq->has_reserved);
	ioq->has_reserved = false;

	Assert(!dclist_is_empty(&ioq->idle));

	node = dclist_pop_head_node(&ioq->idle);
	tio = dclist_container(TrackedIO, node, node);

	tio->iow = *iow;

	dclist_push_tail(&ioq->in_progress, &tio->node);

	ioq->unsubmitted++;

	/*
	 * XXX: Should have some smarter logic here. We don't want to wait too
	 * long to submit, that'll mean we're more likely to block. But we also
	 * don't want to have the overhead of submitting every IO individually.
	 */
	if (ioq->unsubmitted >= 4)
	{
		pgaio_submit_staged();
		ioq->unsubmitted = 0;
	}
}

void
io_queue_wait_all(IOQueue *ioq)
{
	/* submit all pending IO before waiting */
	pgaio_submit_staged();

	while (!dclist_is_empty(&ioq->in_progress))
	{
		/* wait for the last IO to minimize unnecessary wakeups */
		dlist_node *node = dclist_tail_node(&ioq->in_progress);
		TrackedIO  *tio = dclist_container(TrackedIO, node, node);

		if (!pgaio_wref_check_done(&tio->iow))
		{
			ereport(DEBUG3,
					errmsg("io_queue_wait_all for io:%d",
						   pgaio_wref_get_id(&tio->iow)),
					errhidestmt(true),
					errhidecontext(true));

			pgaio_wref_wait(&tio->iow);
		}

		dclist_delete_from(&ioq->in_progress, &tio->node);
		dclist_push_head(&ioq->idle, &tio->node);
	}
}

bool
io_queue_is_empty(IOQueue *ioq)
{
	return dclist_is_empty(&ioq->in_progress);
}

void
io_queue_free(IOQueue *ioq)
{
	io_queue_wait_all(ioq);

	pfree(ioq);
}
