/*-------------------------------------------------------------------------
 *
 * buf_reserve.c
 *	  buffer reservation routines
 *
 * We always try to keep the free reserve filled to the current target,
 * refilling it from the clean list. If the clean list is sufficiently below
 * the target size, we find victim buffers and, if necessary, issue IO to
 * write out the buffers.
 *
 * Todo:
 * - docs
 * - incremental increasing of list sizes
 * - use of aio callbacks to avoid needing to scan the entire list / busy-checking
 * - sort lists of waiting-for-lsn-to-be-flushed buffers
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_reserve.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/ilist.h"
#include "miscadmin.h"
#include "storage/aio.h"
#include "storage/buf.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner_private.h"


typedef struct CleanBufferEntry
{
	Buffer buffer;
	XLogRecPtr lsn;
	bool in_walflush;
	bool in_writeback;
	bool from_ring;
	PgAioIoRef aio_ref;

	dlist_node node;
} CleanBufferEntry;

typedef struct BackendBufReserve
{
	int free_target_size;
	int free_max_size;

	int clean_target_size;
	int clean_max_size;

	int clean_batch_size;

	/* free buffers */
	int free_cur_size;
	dlist_head free_list;

	/* clean buffers */
	int clean_clean_size;
	dlist_head clean_clean_list;

	/* buffers currently in writeback */
	int clean_writeback_size;
	dlist_head clean_writeback_list;

	/* buffers currently waiting for XLOG flushes */
	int clean_walflush_size;
	dlist_head clean_walflush_list;

} BackendBufReserve;

static BackendBufReserve bbr = {
	.free_cur_size = 0,
	.free_list = DLIST_STATIC_INIT(bbr.free_list),

	.clean_clean_size = 0,
	.clean_clean_list = DLIST_STATIC_INIT(bbr.clean_clean_list),

	.clean_writeback_size = 0,
	.clean_writeback_list = DLIST_STATIC_INIT(bbr.clean_writeback_list),

	.clean_walflush_size = 0,
	.clean_walflush_list = DLIST_STATIC_INIT(bbr.clean_walflush_list)
};

static void
BufReserveFillFree(IOContext io_context)
{
	while (true)
	{
		if (bbr.free_cur_size >= bbr.free_target_size)
			break;

		if (bbr.clean_clean_size > 0)
		{
			dlist_node *node = dlist_pop_head_node(&bbr.clean_clean_list);
			CleanBufferEntry *entry = dlist_container(CleanBufferEntry, node, node);

			Assert(!entry->in_writeback);
			Assert(!entry->in_walflush);
			Assert(bbr.clean_clean_size > 0);
			bbr.clean_clean_size--;

			if (TryReuseBuffer(entry->buffer, io_context))
			{
				ereport(DEBUG2,
						errmsg("fill free: %d reusable", entry->buffer),
						errhidestmt(true),
						errhidecontext(true));

				dlist_push_tail(&bbr.free_list, &entry->node);
				bbr.free_cur_size++;

				/*
				 * When a BufferAccessStrategy is in use, blocks evicted from shared
				 * buffers are counted as IOOP_EVICT in the corresponding context
				 * (e.g. IOCONTEXT_BULKWRITE). Shared buffers are evicted by a
				 * strategy in two cases: 1) while initially claiming buffers for the
				 * strategy ring 2) to replace an existing strategy ring buffer
				 * because it is pinned or in use and cannot be reused.
				 *
				 * Blocks evicted from buffers already in the strategy ring are
				 * counted as IOOP_REUSE in the corresponding strategy context.
				 *
				 * At this point, we can accurately count evictions and reuses,
				 * because we have successfully claimed the valid buffer. Previously,
				 * we may have been forced to release the buffer due to concurrent
				 * pinners or erroring out.
				 *
				 * FIXME: Reimplement IOOP_REUSE properly
				 */
				pgstat_count_io_op(IOOBJECT_RELATION, io_context,
								   io_context != IOCONTEXT_NORMAL && entry->from_ring ?
								   IOOP_REUSE : IOOP_EVICT);

			}
			else
			{
				ereport(DEBUG2,
						errmsg("fill free: %d not reusable", entry->buffer),
						errhidestmt(true),
						errhidecontext(true));
			}
		}
		else
			break;
	}
}

static void
BufReserveFillClean(BufferAccessStrategy strategy, IOContext io_context)
{
	int refill_count;
	bool staged_buffer_writes = false;

	refill_count = bbr.clean_target_size - (bbr.clean_clean_size + bbr.clean_writeback_size);

	if (refill_count >= (bbr.clean_batch_size))
		refill_count = bbr.clean_batch_size;

	Assert(refill_count > 0);

	for (int i = 0; i < refill_count; i++)
	{
		CleanBufferEntry *clean;

		clean = MemoryContextAllocZero(TopMemoryContext, sizeof(CleanBufferEntry));

		/*
		 * FIXME: Nothing prevents the same buffer from being returned again,
		 * ending up multiple times on the clean list.
		 */
		clean->buffer = AsyncGetVictimBuffer(strategy, &clean->lsn,
											 &clean->aio_ref, &clean->from_ring,
											 io_context);

		if (clean->lsn != InvalidXLogRecPtr)
		{
			ereport(DEBUG2,
					errmsg("fill clean: %d needs flush %X/%X",
						   clean->buffer, (uint32) (clean->lsn >> 32), (uint32) clean->lsn),
					errhidestmt(true),
					errhidecontext(true));
			pgaio_io_ref_clear(&clean->aio_ref);
			clean->in_walflush = true;
			clean->in_writeback = false;
			bbr.clean_walflush_size++;
			dlist_push_tail(&bbr.clean_walflush_list, &clean->node);
		}
		else if (pgaio_io_ref_valid(&clean->aio_ref))
		{
			ereport(DEBUG2,
					errmsg("fill clean: %d was dirty", clean->buffer),
					errhidestmt(true),
					errhidecontext(true));

			clean->in_walflush = false;
			clean->in_writeback = true;
			bbr.clean_writeback_size++;
			staged_buffer_writes = true;
			dlist_push_tail(&bbr.clean_writeback_list, &clean->node);
		}
		else
		{
			ereport(DEBUG2,
					errmsg("fill clean: %d already clean", clean->buffer),
					errhidestmt(true),
					errhidecontext(true));

			pgaio_io_ref_clear(&clean->aio_ref);
			clean->in_walflush = false;
			clean->in_writeback = false;
			bbr.clean_clean_size++;
			dlist_push_tail(&bbr.clean_clean_list, &clean->node);
		}

		ReleaseBuffer(clean->buffer);
	}

	/*
	 * If we staged a buffer write we need to submit that write, the buffer is
	 * locked for that IO.
	 */
	if (staged_buffer_writes)
		pgaio_submit_pending(false);
	else
		pgaio_limit_pending(false, bbr.clean_batch_size / 2);
}

static void
BufReserveCheckClean(IOContext io_context)
{
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, &bbr.clean_walflush_list)
	{
		CleanBufferEntry *entry = dlist_container(CleanBufferEntry, node, iter.cur);

		Assert(entry->in_walflush);
		Assert(entry->lsn != InvalidXLogRecPtr);

		if (!XLogNeedsFlush(entry->lsn))
		{
			bool valid;

			ereport(DEBUG2,
					errmsg("check: %d doesn't need flush: %X/%X",
						   entry->buffer,
						   (uint32) (entry->lsn >> 32), (uint32) entry->lsn),
					errhidestmt(true),
					errhidecontext(true));

			entry->in_walflush = false;
			Assert(bbr.clean_walflush_size > 0);
			bbr.clean_walflush_size--;
			dlist_delete_from(&bbr.clean_walflush_list, &entry->node);

			valid = AsyncFlushVictim(entry->buffer, &entry->lsn, &entry->aio_ref,
									 io_context);

			if (valid)
			{
				if (pgaio_io_ref_valid(&entry->aio_ref))
				{
					ereport(DEBUG2,
							errmsg("check clean: %d was dirty", entry->buffer),
							errhidestmt(true),
							errhidecontext(true));

					entry->in_walflush = false;
					entry->in_writeback = true;
					bbr.clean_writeback_size++;
					dlist_push_tail(&bbr.clean_writeback_list, &entry->node);
				}
				else
				{
					ereport(DEBUG2,
							errmsg("check clean: %d already clean", entry->buffer),
							errhidestmt(true),
							errhidecontext(true));

					pgaio_io_ref_clear(&entry->aio_ref);
					entry->in_walflush = false;
					entry->in_writeback = false;
					bbr.clean_clean_size++;
					dlist_push_tail(&bbr.clean_clean_list, &entry->node);
				}
			}
			else
			{
				ereport(DEBUG2,
						errmsg("check clean: %d not usable", entry->buffer),
						errhidestmt(true),
						errhidecontext(true));

				pfree(entry);
			}
		}
	}

	dlist_foreach_modify(iter, &bbr.clean_writeback_list)
	{
		CleanBufferEntry *entry = dlist_container(CleanBufferEntry, node, iter.cur);

		Assert(entry->in_writeback);
		Assert(!entry->in_walflush);

		if (pgaio_io_check_ref(&entry->aio_ref))
		{
			ereport(DEBUG2,
					errmsg("check: %d is done", entry->buffer),
					errhidestmt(true),
					errhidecontext(true));
			entry->in_writeback = false;
			dlist_delete_from(&bbr.clean_writeback_list, &entry->node);
			Assert(bbr.clean_writeback_size > 0);
			bbr.clean_writeback_size--;

			dlist_push_tail(&bbr.clean_clean_list, &entry->node);
			bbr.clean_clean_size++;
		}
	}
}

static void
BufReserveWaitOneClean(IOContext io_context)
{
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, &bbr.clean_writeback_list)
	{
		CleanBufferEntry *entry = dlist_container(CleanBufferEntry, node, iter.cur);

		Assert(entry->in_writeback);
		Assert(!entry->in_walflush);

		ereport(DEBUG1,
				errmsg("WaitOne: buf %d: waiting for IO (free: %d clean: %d writeback: %d flush: %d)",
					   entry->buffer,
					   bbr.free_cur_size, bbr.clean_clean_size, bbr.clean_writeback_size, bbr.clean_walflush_size),
				errhidestmt(true),
				errhidecontext(true));

		pgaio_io_wait_ref(&entry->aio_ref, false);

		entry->in_writeback = false;

		dlist_delete_from(&bbr.clean_writeback_list, &entry->node);
		Assert(bbr.clean_writeback_size > 0);
		bbr.clean_writeback_size--;

		dlist_push_tail(&bbr.clean_clean_list, &entry->node);
		bbr.clean_clean_size++;

		return;
	}

	dlist_foreach_modify(iter, &bbr.clean_walflush_list)
	{
		CleanBufferEntry *entry = dlist_container(CleanBufferEntry, node, iter.cur);

		Assert(entry->in_walflush);
		Assert(!entry->in_writeback);

		ereport(DEBUG1,
				errmsg("WaitOne: buf %d: waiting for flush of %X/%X (free: %d clean: %d writeback: %d flush: %d)",
					   entry->buffer,
					   (uint32) (entry->lsn >> 32), (uint32) entry->lsn,
					   bbr.free_cur_size, bbr.clean_clean_size, bbr.clean_writeback_size, bbr.clean_walflush_size),
				errhidestmt(true),
				errhidecontext(true));
		XLogFlush(entry->lsn);
		BufReserveCheckClean(io_context);
		return;
	}
}

static bool
BufReserveCleanNeedsRefill(void)
{
	int clean_size = bbr.clean_clean_size +
		bbr.clean_writeback_size +
		bbr.clean_walflush_size;

	int clean_start_issue = bbr.clean_target_size -
		bbr.clean_batch_size;

	return clean_size <= clean_start_issue;
}

BufferDesc *
BufReserveGetFree(BufferAccessStrategy strategy, bool block, IOContext io_context)
{
	BufferDesc *ret = NULL;
	volatile static int recursion = 0;

	Assert(recursion == 0);
	recursion++;

	PG_TRY();
	{

	while (true)
	{
		CHECK_FOR_INTERRUPTS();

		if (bbr.free_cur_size > 0)
		{
			CleanBufferEntry *entry;

			entry = dlist_head_element(CleanBufferEntry, node, &bbr.free_list);
			ret = GetBufferDescriptor(entry->buffer - 1);

			OwnUnusedBuffer(entry->buffer);

			dlist_delete_from(&bbr.free_list, &entry->node);
			bbr.free_cur_size--;

			pfree(entry);
		}

		if (bbr.free_cur_size < bbr.free_target_size)
		{
			BufReserveCheckClean(io_context);
			BufReserveFillFree(io_context);
		}

		if (BufReserveCleanNeedsRefill())
		{
			BufReserveCheckClean(io_context);
			BufReserveFillClean(strategy, io_context);
			BufReserveCheckClean(io_context);
			BufReserveFillFree(io_context);

			if (ret == InvalidBuffer)
				continue;
		}

		if (ret != InvalidBuffer)
			break;
		else if (bbr.free_cur_size == 0 && bbr.clean_clean_size == 0)
		{
			if (block)
				BufReserveWaitOneClean(io_context);
			else
				break;
		}
	}

	if (ret != NULL)
	{
		CheckBufferIsPinnedOnce(BufferDescriptorGetBuffer(ret));
	}

	}
	PG_FINALLY();
	{
		recursion--;
	}
	PG_END_TRY();

	return ret;
}

static void
BufReserveAtExit(int code, Datum arg)
{
	while (!dlist_is_empty(&bbr.free_list))
	{
		CleanBufferEntry *entry;
		uint32 buf_state;
		BufferDesc *buf_hdr;

		entry = dlist_head_element(CleanBufferEntry, node, &bbr.free_list);

		buf_hdr = GetBufferDescriptor(entry->buffer - 1);
		buf_state = LockBufHdr(buf_hdr);

		Assert(BUF_STATE_GET_REFCOUNT(buf_state) == 1);
		Assert(!(buf_state & (BM_VALID | BM_TAG_VALID | BM_DIRTY)));

		buf_state -= BUF_REFCOUNT_ONE;

		UnlockBufHdr(buf_hdr, buf_state);

		dlist_delete_from(&bbr.free_list, &entry->node);
		bbr.free_cur_size--;
		pfree(entry);
	}
}

void
BufReserveInit(void)
{
	int max_backends = MaxBackends;
	int buf_per_backend = NBuffers / max_backends;

	/* XXX: should be elsewhere */
	if (NBuffers < (max_backends * 2))
		elog(ERROR, "shared_buffers of %d is too small for max_connections, set at least to %d",
			 NBuffers, max_backends * 2);

	/*
	 * Don't allow buffers pinned to take more than 1/8 of s_b, otherwise
	 * we run into danger of running out of buffers.
	 */
	bbr.free_max_size = Min(Max(buf_per_backend / 8, 1), 8);
	bbr.free_target_size = bbr.free_max_size; // XXX: make smarter

	/*
	 * Buffers on clean list aren't pinned, we just end up writing buffers
	 * back more aggressively.
	 */
	bbr.clean_max_size = Min(Max(NBuffers / 128, 1), 256);
	bbr.clean_target_size = bbr.clean_max_size; // XXX: make smarter

	bbr.clean_batch_size = Min(64, bbr.clean_target_size);

	on_shmem_exit(BufReserveAtExit, 0);
}
