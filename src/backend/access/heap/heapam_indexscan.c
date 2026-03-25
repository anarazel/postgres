/*-------------------------------------------------------------------------
 *
 * heapam_indexscan.c
 *	  heap table plain index scan and index-only scan code
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam_indexscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/indexbatch.h"
#include "access/relscan.h"
#include "access/visibilitymap.h"
#include "optimizer/cost.h"
#include "storage/predicate.h"
#include "utils/pgstat_internal.h"


static bool heapam_index_plain_amgetbatch_next(IndexScanDesc scan,
											   ScanDirection direction,
											   TupleTableSlot *slot);
static bool heapam_index_only_amgetbatch_next(IndexScanDesc scan,
											  ScanDirection direction,
											  TupleTableSlot *slot);
static bool heapam_index_plain_amgettuple_next(IndexScanDesc scan,
											   ScanDirection direction,
											   TupleTableSlot *slot);
static bool heapam_index_only_amgettuple_next(IndexScanDesc scan,
											  ScanDirection direction,
											  TupleTableSlot *slot);
static pg_attribute_always_inline bool heapam_index_fetch_tuple_impl(Relation rel,
																	 IndexFetchHeapData *hscan,
																	 ItemPointer tid,
																	 Snapshot snapshot,
																	 TupleTableSlot *slot,
																	 bool *heap_continue,
																	 bool *all_dead);
static pg_attribute_always_inline bool heapam_index_getnext_slot(IndexScanDesc scan,
																 ScanDirection direction,
																 TupleTableSlot *slot,
																 bool index_only,
																 bool amgetbatch);
static pg_attribute_always_inline bool heapam_index_fetch_heap(IndexScanDesc scan,
															   IndexFetchHeapData *hscan,
															   TupleTableSlot *slot,
															   bool *heap_continue,
															   bool amgetbatch);
static pg_attribute_always_inline ItemPointer heapam_index_getnext_scanbatch_pos(IndexScanDesc scan,
																				 IndexFetchHeapData *hscan,
																				 ScanDirection direction,
																				 bool *all_visible);
static inline ItemPointer heapam_index_return_scanpos_tid(IndexScanDesc scan,
														  IndexFetchHeapData *hscan,
														  ScanDirection direction,
														  IndexScanBatch scanBatch,
														  BatchRingItemPos *scanPos,
														  bool *all_visible);
static void heapam_index_batch_pos_visibility(IndexScanDesc scan,
											  ScanDirection direction,
											  IndexScanBatch batch,
											  HeapBatchData *hbatch,
											  BatchRingItemPos *pos);
static pg_noinline void heapam_index_dirchange_reset(IndexFetchHeapData *hscan,
													 ScanDirection direction,
													 BatchRingBuffer *batchringbuf);
static pg_attribute_always_inline void heapam_index_consider_prefetching(IndexScanDesc scan,
																		 IndexFetchHeapData *hscan);
static BlockNumber heapam_index_prefetch_next_block(ReadStream *stream,
													void *callback_private_data,
													void *per_buffer_data);

/* ------------------------------------------------------------------------
 * Index Scan Callbacks for heap AM
 * ------------------------------------------------------------------------
 */

IndexFetchTableData *
heapam_index_fetch_begin(IndexScanDesc scan, uint32 flags)
{
	IndexFetchHeapData *hscan = palloc0_object(IndexFetchHeapData);

	hscan->xs_base.batch_opaque_size = MAXALIGN(sizeof(HeapBatchData));
	hscan->xs_base.batch_per_item_size = sizeof(uint8); /* visInfo element size */
	hscan->xs_base.flags = flags;

	/* Current heap block state */
	Assert(hscan->xs_cbuf == InvalidBuffer);
	hscan->xs_blk = InvalidBlockNumber;

	/* VM related state */
	Assert(hscan->xs_vmbuffer == InvalidBuffer);
	hscan->xs_vm_items = 1;

	/* xs_lastinblock optimization state */
	Assert(!hscan->xs_lastinblock);

	/* Read stream state (other fields initialized by callback) */
	Assert(hscan->xs_read_stream_dir == NoMovementScanDirection);
	Assert(hscan->xs_read_stream == NULL);

	/* Resolve which getnext_slot implementation to use for this scan */
	if (scan->indexRelation->rd_indam->amgetbatch != NULL)
	{
		/* amgetbatch index AM */
		if (scan->xs_want_itup)
			scan->xs_getnext_slot = heapam_index_only_amgetbatch_next;
		else
			scan->xs_getnext_slot = heapam_index_plain_amgetbatch_next;

		/* Set up scan's batch ring buffer in passing */
		tableam_util_batchscan_init(scan);
	}
	else
	{
		/* amgettuple index AM */
		if (scan->xs_want_itup)
			scan->xs_getnext_slot = heapam_index_only_amgettuple_next;
		else
			scan->xs_getnext_slot = heapam_index_plain_amgettuple_next;
	}

	return &hscan->xs_base;
}

/*
 * Initialize the heap table AM's per-batch opaque area (HeapBatchData).
 * Called by indexam_util_batch_alloc for each new or recycled batch.
 */
void
heapam_index_fetch_batch_init(IndexScanDesc scan, IndexScanBatch batch,
							  bool new_alloc)
{
	HeapBatchData *hbatch = heap_batch_data(scan, batch);

	if (scan->xs_want_itup)
	{
		if (new_alloc)
		{
			/*
			 * The visInfo pointer is stored at the very start of the palloc'd
			 * space, in the fixed-sized table AM opaque area.  visInfo points
			 * to just past the end of the variable-sized items[maxitemsbatch]
			 * array (to a space that is also sized according to whatever the
			 * index AM set maxitemsbatch to).
			 */
			Size		itemsEnd;

			itemsEnd = MAXALIGN(offsetof(IndexScanBatchData, items) +
								sizeof(BatchMatchingItem) * scan->maxitemsbatch);
			hbatch->visInfo = (uint8 *) ((char *) batch + itemsEnd);
		}

		/* Clear visibility flags (needed for both new and recycled batches) */
		memset(hbatch->visInfo, 0, scan->maxitemsbatch);
	}
	else
	{
		hbatch->visInfo = NULL;
	}
}

void
heapam_index_fetch_reset(IndexScanDesc scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan->xs_heapfetch;

	/* Rescans should avoid an excessive number of VM lookups */
	hscan->xs_vm_items = 1;

	/* Defensively do an unconditional read stream direction reset */
	hscan->xs_read_stream_dir = NoMovementScanDirection;

	if (hscan->xs_read_stream)
	{
		hscan->xs_paused = false;
		read_stream_reset(hscan->xs_read_stream);
	}

	/* Reset batch ring buffer state */
	if (scan->usebatchring)
		tableam_util_batchscan_reset(scan, false);

	/*
	 * Deliberately avoid dropping pins now held in xs_cbuf and xs_vmbuffer.
	 * This saves cycles during certain tight nested loop joins (it can avoid
	 * repeated pinning and unpinning of the same buffer across rescans).
	 */
}

void
heapam_index_fetch_end(IndexScanDesc scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan->xs_heapfetch;

	/* drop pin if there's a pinned heap page */
	if (BufferIsValid(hscan->xs_cbuf))
		ReleaseBuffer(hscan->xs_cbuf);

	/* drop pin if there's a pinned visibility map page */
	if (BufferIsValid(hscan->xs_vmbuffer))
		ReleaseBuffer(hscan->xs_vmbuffer);

	if (hscan->xs_read_stream)
		read_stream_end(hscan->xs_read_stream);

	/* Free all batch related resources */
	if (scan->usebatchring)
		tableam_util_batchscan_end(scan);

	pfree(hscan);
}

/*
 * Save batch ring buffer's current scanPos as its markPos
 */
void
heapam_index_fetch_markpos(IndexScanDesc scan)
{
	Assert(scan->usebatchring);

	tableam_util_batchscan_mark_pos(scan);
}

/*
 * Restore batch ring buffer's markPos into its scanPos
 */
void
heapam_index_fetch_restrpos(IndexScanDesc scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan->xs_heapfetch;

	Assert(scan->usebatchring);

	if (hscan->xs_read_stream)
	{
		hscan->xs_paused = false;
		read_stream_reset(hscan->xs_read_stream);
	}

	tableam_util_batchscan_restore_pos(scan);
}

/*
 *	heap_hot_search_buffer	- search HOT chain for tuple satisfying snapshot
 *
 * On entry, *tid is the TID of a tuple (either a simple tuple, or the root
 * of a HOT chain), and buffer is the buffer holding this tuple.  We search
 * for the first chain member satisfying the given snapshot.  If one is
 * found, we update *tid to reference that tuple's offset number, and
 * return true.  If no match, return false without modifying *tid.
 *
 * heapTuple is a caller-supplied buffer.  When a match is found, we return
 * the tuple here, in addition to updating *tid.  If no match is found, the
 * contents of this buffer on return are undefined.
 *
 * If all_dead is not NULL, we check non-visible tuples to see if they are
 * globally dead; *all_dead is set true if all members of the HOT chain
 * are vacuumable, false if not.
 *
 * Unlike heap_fetch, the caller must already have pin and (at least) share
 * lock on the buffer; it is still pinned/locked at exit.
 */
bool
heap_hot_search_buffer(ItemPointer tid, Relation relation, Buffer buffer,
					   Snapshot snapshot, HeapTuple heapTuple,
					   bool *all_dead, bool first_call)
{
	Page		page = BufferGetPage(buffer);
	TransactionId prev_xmax = InvalidTransactionId;
	BlockNumber blkno;
	OffsetNumber offnum;
	bool		at_chain_start;
	bool		valid;
	bool		skip;
	GlobalVisState *vistest = NULL;

	/* If this is not the first call, previous call returned a (live!) tuple */
	if (all_dead)
		*all_dead = first_call;

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);
	at_chain_start = first_call;
	skip = !first_call;

	/* XXX: we should assert that a snapshot is pushed or registered */
	Assert(TransactionIdIsValid(RecentXmin));
	Assert(BufferGetBlockNumber(buffer) == blkno);

	/* Scan through possible multiple members of HOT-chain */
	for (;;)
	{
		ItemId		lp;

		/* check for bogus TID */
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
			break;

		lp = PageGetItemId(page, offnum);

		/* check for unused, dead, or redirected items */
		if (!ItemIdIsNormal(lp))
		{
			/* We should only see a redirect at start of chain */
			if (ItemIdIsRedirected(lp) && at_chain_start)
			{
				/* Follow the redirect */
				offnum = ItemIdGetRedirect(lp);
				at_chain_start = false;
				continue;
			}
			/* else must be end of chain */
			break;
		}

		/*
		 * Update heapTuple to point to the element of the HOT chain we're
		 * currently investigating. Having t_self set correctly is important
		 * because the SSI checks and the *Satisfies routine for historical
		 * MVCC snapshots need the correct tid to decide about the visibility.
		 */
		heapTuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
		heapTuple->t_len = ItemIdGetLength(lp);
		heapTuple->t_tableOid = RelationGetRelid(relation);
		ItemPointerSet(&heapTuple->t_self, blkno, offnum);

		/*
		 * Shouldn't see a HEAP_ONLY tuple at chain start.
		 */
		if (at_chain_start && HeapTupleIsHeapOnly(heapTuple))
			break;

		/*
		 * The xmin should match the previous xmax value, else chain is
		 * broken.
		 */
		if (TransactionIdIsValid(prev_xmax) &&
			!TransactionIdEquals(prev_xmax,
								 HeapTupleHeaderGetXmin(heapTuple->t_data)))
			break;

		/*
		 * When first_call is true (and thus, skip is initially false) we'll
		 * return the first tuple we find.  But on later passes, heapTuple
		 * will initially be pointing to the tuple we returned last time.
		 * Returning it again would be incorrect (and would loop forever), so
		 * we skip it and return the next match we find.
		 */
		if (!skip)
		{
			/* If it's visible per the snapshot, we must return it */
			valid = HeapTupleSatisfiesVisibility(heapTuple, snapshot, buffer);
			HeapCheckForSerializableConflictOut(valid, relation, heapTuple,
												buffer, snapshot);

			if (valid)
			{
				ItemPointerSetOffsetNumber(tid, offnum);
				PredicateLockTID(relation, &heapTuple->t_self, snapshot,
								 HeapTupleHeaderGetXmin(heapTuple->t_data));
				if (all_dead)
					*all_dead = false;
				return true;
			}
		}
		skip = false;

		/*
		 * If we can't see it, maybe no one else can either.  At caller
		 * request, check whether all chain members are dead to all
		 * transactions.
		 *
		 * Note: if you change the criterion here for what is "dead", fix the
		 * planner's get_actual_variable_range() function to match.
		 */
		if (all_dead && *all_dead)
		{
			if (!vistest)
				vistest = GlobalVisTestFor(relation);

			if (!HeapTupleIsSurelyDead(heapTuple, vistest))
				*all_dead = false;
		}

		/*
		 * Check to see if HOT chain continues past this tuple; if so fetch
		 * the next offnum and loop around.
		 */
		if (HeapTupleIsHotUpdated(heapTuple))
		{
			Assert(ItemPointerGetBlockNumber(&heapTuple->t_data->t_ctid) ==
				   blkno);
			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_data->t_ctid);
			at_chain_start = false;
			prev_xmax = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
		}
		else
			break;				/* end of chain */

	}

	return false;
}

/*
 * Simple, single-shot TID lookup for constraint enforcement code (unique
 * checks and similar).  This is essentially just a heap_hot_search_buffer
 * wrapper.
 *
 * This isn't actually related to index scans, but keeping it near
 * heap_hot_search_buffer may help the compiler generate better code.
 */
bool
heapam_fetch_tid(Relation rel, ItemPointer tid, Snapshot snapshot,
				 TupleTableSlot *slot, bool *all_dead)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	Buffer		buf;
	bool		found;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	buf = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	found = heap_hot_search_buffer(tid, rel, buf, snapshot,
								   &bslot->base.tupdata, all_dead, true);
	bslot->base.tupdata.t_self = *tid;
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	if (found)
	{
		slot->tts_tableOid = RelationGetRelid(rel);
		ExecStorePinnedBufferHeapTuple(&bslot->base.tupdata, slot,
									   buf);
	}
	else
		ReleaseBuffer(buf);

	return found;
}

/* table_index_getnext_slot callback: amgetbatch, plain index scan */
static pg_attribute_hot bool
heapam_index_plain_amgetbatch_next(IndexScanDesc scan,
								   ScanDirection direction,
								   TupleTableSlot *slot)
{
	Assert(!scan->xs_want_itup && scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amgetbatch != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, false, true);
}

/* table_index_getnext_slot callback: amgetbatch, index-only scan */
static pg_attribute_hot bool
heapam_index_only_amgetbatch_next(IndexScanDesc scan,
								  ScanDirection direction,
								  TupleTableSlot *slot)
{
	Assert(scan->xs_want_itup && scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amgetbatch != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, true, true);
}

/* table_index_getnext_slot callback: amgettuple, plain index scan */
static pg_attribute_hot bool
heapam_index_plain_amgettuple_next(IndexScanDesc scan,
								   ScanDirection direction,
								   TupleTableSlot *slot)
{
	Assert(!scan->xs_want_itup && !scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amgettuple != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, false, false);
}

/* table_index_getnext_slot callback: amgettuple, index-only scan */
static pg_attribute_hot bool
heapam_index_only_amgettuple_next(IndexScanDesc scan,
								  ScanDirection direction,
								  TupleTableSlot *slot)
{
	Assert(scan->xs_want_itup && !scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amgettuple != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, true, false);
}

static pg_attribute_always_inline bool
heapam_index_fetch_tuple_impl(Relation rel,
							  IndexFetchHeapData *hscan,
							  ItemPointer tid,
							  Snapshot snapshot,
							  TupleTableSlot *slot,
							  bool *heap_continue, bool *all_dead)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	bool		got_heap_tuple;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	/* We can skip the buffer-switching logic if we're on the same page. */
	if (hscan->xs_blk != ItemPointerGetBlockNumber(tid))
	{
		Assert(!*heap_continue);

		/* Remember this buffer's block number for next time */
		hscan->xs_blk = ItemPointerGetBlockNumber(tid);

		/*
		 * Drop the xs_blk pin independently held on by slot (if any) now,
		 * before calling ReleaseBuffer.  This avoids expensive calls to
		 * GetPrivateRefCountEntrySlow caused by ExecStoreBufferHeapTuple
		 * failing to hit the backend's cache for the release of the old pin.
		 */
		ExecClearTuple(slot);

		if (BufferIsValid(hscan->xs_cbuf))
			ReleaseBuffer(hscan->xs_cbuf);

		/*
		 * When using a read stream, the stream will already know which block
		 * number comes next (though an assertion will verify a match below)
		 */
		if (hscan->xs_read_stream)
			hscan->xs_cbuf = read_stream_next_buffer(hscan->xs_read_stream, NULL);
		else
			hscan->xs_cbuf = ReadBuffer(rel, hscan->xs_blk);

		/*
		 * Prune page when it is pinned for the first time
		 */
		heap_page_prune_opt(rel, hscan->xs_cbuf,
							&hscan->xs_vmbuffer,
							hscan->xs_base.flags & SO_HINT_REL_READ_ONLY);
	}

	Assert(BufferGetBlockNumber(hscan->xs_cbuf) == hscan->xs_blk);
	Assert(hscan->xs_blk == ItemPointerGetBlockNumber(tid));

	/* Obtain share-lock on the buffer so we can examine visibility */
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
	got_heap_tuple = heap_hot_search_buffer(tid,
											rel,
											hscan->xs_cbuf,
											snapshot,
											&bslot->base.tupdata,
											all_dead,
											!*heap_continue);
	bslot->base.tupdata.t_self = *tid;
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);

	if (got_heap_tuple)
	{
		/*
		 * Only in a non-MVCC snapshot can more than one member of the HOT
		 * chain be visible.
		 */
		*heap_continue = !IsMVCCLikeSnapshot(snapshot);

		slot->tts_tableOid = RelationGetRelid(rel);

		/*
		 * If this is the last TID on the current heap block within the batch,
		 * transfer our buffer pin to the slot rather than having the slot
		 * increment the pin count.  This saves a pair of IncrBufferRefCount
		 * and ReleaseBuffer calls, since the caller would just release its
		 * pin on xs_cbuf when switching to the next block anyway.
		 *
		 * We can only do this when heap_continue is false, since otherwise
		 * the caller will need xs_cbuf to remain valid for the next call.
		 */
		if (hscan->xs_lastinblock && !*heap_continue)
		{
			ExecStorePinnedBufferHeapTuple(&bslot->base.tupdata, slot,
										   hscan->xs_cbuf);
			hscan->xs_cbuf = InvalidBuffer;
			hscan->xs_blk = InvalidBlockNumber;

			/*
			 * Note: the pin now owned by the slot is expected to be released
			 * on the next call here, via an explicit ExecClearTuple.  This
			 * avoids churn in the backend's private refcount cache.
			 */
		}
		else
			ExecStoreBufferHeapTuple(&bslot->base.tupdata, slot,
									 hscan->xs_cbuf);
	}
	else
	{
		/* We've reached the end of the HOT chain. */
		*heap_continue = false;
	}

	return got_heap_tuple;
}

/*
 * Common implementation for all four heapam_index_*_getnext_slot variants.
 *
 * The result is true if a tuple satisfying the scan keys and the snapshot was
 * found, false otherwise.  The tuple is stored in the specified slot.
 *
 * On success, resources (like buffer pins) are likely to be held, and will be
 * dropped by a future call here (or by a later call to heapam_index_fetch_end
 * through index_endscan).
 *
 * The index_only and amgetbatch parameters are compile-time constants at each
 * call site, allowing the compiler to specialize the code for each variant:
 */
static pg_attribute_always_inline bool
heapam_index_getnext_slot(IndexScanDesc scan, ScanDirection direction,
						  TupleTableSlot *slot, bool index_only,
						  bool amgetbatch)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan->xs_heapfetch;
	bool	   *heap_continue = &scan->xs_heap_continue;
	bool		all_visible = false;
	BlockNumber last_visited_block = InvalidBlockNumber;
	uint8		n_visited_pages = 0;
	ItemPointer tid = NULL;

	for (;;)
	{
		if (!*heap_continue)
		{
			/* Get the next TID from the index */
			if (amgetbatch)
				tid = heapam_index_getnext_scanbatch_pos(scan, hscan,
														 direction,
														 index_only ?
														 &all_visible : NULL);
			else
				tid = index_getnext_tid(scan, direction);

			/* If we're out of index entries, we're done */
			if (tid == NULL)
				break;

			/* For non-batch index-only scans, check the visibility map */
			if (index_only && !amgetbatch)
				all_visible = VM_ALL_VISIBLE(scan->heapRelation,
											 ItemPointerGetBlockNumber(tid),
											 &hscan->xs_vmbuffer);
		}

		Assert(ItemPointerIsValid(&scan->xs_heaptid));

		if (index_only)
		{
			/*
			 * We can skip the heap fetch if the TID references a heap page on
			 * which all tuples are known visible to everybody.  In any case,
			 * we'll use the index tuple not the heap tuple as the data
			 * source.
			 */
			if (!all_visible)
			{
				/*
				 * Rats, we have to visit the heap to check visibility.
				 */
				if (scan->instrument)
					scan->instrument->ntabletuplefetches++;

				if (!heapam_index_fetch_heap(scan, hscan, slot,
											 heap_continue, amgetbatch))
				{
					/*
					 * No visible tuple.  If caller set a visited-pages limit
					 * (only selfuncs.c does this), count distinct heap pages
					 * and give up once we've visited too many.
					 */
					if (unlikely(scan->xs_visited_pages_limit > 0))
					{
						Assert(hscan->xs_blk == ItemPointerGetBlockNumber(tid));

						if (hscan->xs_blk != last_visited_block)
						{
							last_visited_block = hscan->xs_blk;
							if (++n_visited_pages > scan->xs_visited_pages_limit)
								return false;	/* give up */
						}
					}
					continue;	/* no visible tuple, try next index entry */
				}

				/* We don't actually need the heap tuple for anything */
				ExecClearTuple(slot);

				/*
				 * Only MVCC snapshots are supported with standard index-only
				 * scans, so there should be no need to keep following the HOT
				 * chain once a visible entry has been found.  Other callers
				 * (currently only selfuncs.c) use SnapshotNonVacuumable, and
				 * want us to assume that just having one visible tuple in the
				 * hot chain is always good enough.
				 */
				Assert(!(*heap_continue && scan->MVCCScan));
			}
			else
			{
				/*
				 * We didn't access the heap, so we'll need to take a
				 * predicate lock explicitly, as if we had.  For now we do
				 * that at page level.
				 */
				PredicateLockPage(scan->heapRelation,
								  ItemPointerGetBlockNumber(tid),
								  scan->xs_snapshot);
			}

			/*
			 * Return matching index tuple now set in scan->xs_itup (or return
			 * matching heap tuple now set in scan->xs_hitup)
			 */
			return true;
		}
		else
		{
			/*
			 * Fetch the next (or only) visible heap tuple for this index
			 * entry.  If we don't find anything, loop around and grab the
			 * next TID from the index.
			 */
			if (heapam_index_fetch_heap(scan, hscan, slot, heap_continue,
										amgetbatch))
				return true;
		}
	}

	return false;
}

/*
 * Get the scan's next heap tuple.
 *
 * The result is a visible heap tuple associated with the index TID most
 * recently fetched by our caller in scan->xs_heaptid, or NULL if no more
 * matching tuples exist.  (There can be more than one matching tuple because
 * of HOT chains, although when using an MVCC snapshot it should be impossible
 * for more than one such tuple to exist.)
 *
 * On success, the buffer containing the heap tup is pinned.  The pin must be
 * dropped elsewhere.
 */
static pg_attribute_always_inline bool
heapam_index_fetch_heap(IndexScanDesc scan, IndexFetchHeapData *hscan,
						TupleTableSlot *slot, bool *heap_continue,
						bool amgetbatch)
{
	bool		all_dead = false;
	bool		found;

	found = heapam_index_fetch_tuple_impl(scan->heapRelation, hscan,
										  &scan->xs_heaptid,
										  scan->xs_snapshot, slot,
										  heap_continue, &all_dead);

	if (found)
		pgstat_count_heap_fetch(scan->indexRelation);

	/*
	 * If we scanned a whole HOT chain and found only dead tuples, remember it
	 * for later.  We do not do this when in recovery because it may violate
	 * MVCC to do so.  See comments in RelationGetIndexScan().
	 */
	if (!scan->xactStartedInRecovery)
	{
		if (amgetbatch)
		{
			if (all_dead)
				tableam_util_scanpos_killitem(scan);
		}
		else
		{
			/*
			 * Tell amgettuple-based index AM to kill its entry for that TID
			 * (this will take effect in the next call, in index_getnext_tid)
			 */
			scan->kill_prior_tuple = all_dead;
		}
	}

	return found;
}

/*
 * Get next TID from batch ring buffer, moving in the given scan direction.
 * Also sets *all_visible for item when caller passes a non-NULL arg.
 */
static pg_attribute_always_inline ItemPointer
heapam_index_getnext_scanbatch_pos(IndexScanDesc scan, IndexFetchHeapData *hscan,
								   ScanDirection direction, bool *all_visible)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos = &batchringbuf->scanPos;
	IndexScanBatch scanBatch = NULL;
	bool		hadExistingScanBatch;

	Assert(!scanPos->valid || batchringbuf->headBatch == scanPos->batch);
	Assert(scanPos->valid || index_scan_batch_count(scan) == 0);
	Assert(all_visible == NULL || scan->xs_want_itup);

	/* Handle resetting the read stream when scan direction changes */
	if (hscan->xs_read_stream_dir == NoMovementScanDirection)
		hscan->xs_read_stream_dir = direction;	/* first call */
	else if (unlikely(hscan->xs_read_stream_dir != direction))
		heapam_index_dirchange_reset(hscan, direction, batchringbuf);

	/*
	 * Check if there's an existing loaded scanBatch for us to return the next
	 * matching item's TID/index tuple from
	 */
	hadExistingScanBatch = scanPos->valid;
	if (scanPos->valid)
	{
		/*
		 * scanPos is valid, so scanBatch must already be loaded in batch ring
		 * buffer.  We rely on that here (can't do this with prefetchBatch).
		 */
		pg_assume(batchringbuf->headBatch == scanPos->batch);

		scanBatch = index_scan_batch(scan, scanPos->batch);

		if (index_scan_pos_advance(direction, scanBatch, scanPos))
			return heapam_index_return_scanpos_tid(scan, hscan, direction,
												   scanBatch, scanPos,
												   all_visible);
	}

	/*
	 * Either ran out of items from our existing scanBatch, or it hasn't been
	 * loaded yet (because this is the first call here for the entire scan).
	 * Try to advance scanBatch to the next batch (or get the first batch).
	 */
	scanBatch = tableam_util_fetch_next_batch(scan, direction,
											  scanBatch, scanPos);

	if (!scanBatch)
	{
		/*
		 * We're done; no more batches in the current scan direction.
		 *
		 * Note: scanPos is generally still valid at this point.  The scan
		 * might still back up in the other direction.
		 */
		return NULL;
	}

	if (hadExistingScanBatch && !hscan->xs_read_stream)
	{
		Assert(!scan->batchringbuf.prefetchPos.valid);

		/*
		 * Not using a read stream to do index prefetching.  Decide whether to
		 * start one now.
		 */
		heapam_index_consider_prefetching(scan, hscan);
	}

	/*
	 * Advanced scanBatch.  Now position scanPos to the start of new
	 * scanBatch.
	 */
	index_scan_pos_nextbatch(direction, scanBatch, scanPos);
	Assert(index_scan_batch(scan, scanPos->batch) == scanBatch);

	/*
	 * Remove the head batch from the batch ring buffer (except when this new
	 * scanBatch is our only one)
	 */
	if (hadExistingScanBatch)
	{
		IndexScanBatch headBatch = index_scan_batch(scan,
													batchringbuf->headBatch);
		BatchRingItemPos *prefetchPos = &batchringbuf->prefetchPos;

		Assert(headBatch != scanBatch);
		Assert(batchringbuf->headBatch != scanPos->batch);

		/* free obsolescent head batch (unless it is scan's markBatch) */
		tableam_util_free_batch(scan, headBatch);

		/*
		 * If we're about to release the batch that prefetchPos currently
		 * points to, just invalidate prefetchPos.  See the comments about
		 * prefetchPos/scanPos within heapam_index_prefetch_next_block for an
		 * explanation.
		 *
		 * This handling is approximately the opposite of resuming a paused
		 * read stream: this helps the scan deal with prefetchPos falling
		 * behind scanPos, whereas pausing is used when scanPos has fallen
		 * behind (very far behind) prefetchPos.
		 */
		if (prefetchPos->valid &&
			prefetchPos->batch == batchringbuf->headBatch)
			prefetchPos->valid = false;

		/* Remove the batch from the ring buffer (even if it's markBatch) */
		batchringbuf->headBatch++;

		if (unlikely(hscan->xs_paused))
		{
			/*
			 * heapam_index_prefetch_next_block paused the scan's read stream
			 * due to our running out of free batch slots.  Now that we've
			 * freed up one such slot, we can resume the read stream (since
			 * there's now space for heapam_index_prefetch_next_block to store
			 * one more batch).
			 */
			Assert(prefetchPos->batch != scanPos->batch);
			Assert(prefetchPos->valid &&
				   index_scan_batch_loaded(scan, prefetchPos->batch));
			Assert(index_scan_pos_cmp(prefetchPos, scanPos, direction) > 0);
			Assert(!index_scan_batch_full(scan));

			read_stream_resume(hscan->xs_read_stream);
			hscan->xs_paused = false;
		}
	}

	/* In practice scanBatch will always be the ring buffer's headBatch */
	Assert(batchringbuf->headBatch == scanPos->batch);
	Assert(!hscan->xs_paused);

	return heapam_index_return_scanpos_tid(scan, hscan, direction,
										   scanBatch, scanPos, all_visible);
}

/*
 * Save the current scanPos/scanBatch item's TID in scan's xs_heaptid, and
 * return a pointer to that TID.  When all_visible isn't NULL (during an
 * index-only scan), also sets item's visibility status in *all_visible.
 *
 * heapam_index_getnext_scanbatch_pos helper function.
 */
static inline ItemPointer
heapam_index_return_scanpos_tid(IndexScanDesc scan, IndexFetchHeapData *hscan,
								ScanDirection direction,
								IndexScanBatch scanBatch,
								BatchRingItemPos *scanPos,
								bool *all_visible)
{
	HeapBatchData *hbatch;

	pgstat_count_index_tuples(scan->indexRelation, 1);

	/* Set xs_heaptid, which caller (and core executor) will need */
	scan->xs_heaptid = scanBatch->items[scanPos->item].tableTid;

	if (all_visible == NULL)
	{
		int			nextItem;
		bool		hasNext;

		/*
		 * Plain index scan.
		 *
		 * Determine if the next item in the current scan direction is on a
		 * different heap block.  When it is, heapam_index_fetch_tuple_impl
		 * can transfer its buffer pin to the slot instead of incrementing the
		 * pin count, saving a pair of IncrBufferRefCount/ReleaseBuffer calls.
		 *
		 * Note: We cannot do this for index-only scans because all-visible
		 * items are skipped by both the scan and the read stream callback. It
		 * doesn't seem worth the trouble of reasoning about these issues,
		 * since the optimization only helps when heap fetches are required.
		 *
		 * Note: We deliberately don't consider the batch after scanBatch,
		 * because doing so would add complexity for little benefit.  It's
		 * okay if xs_lastinblock is spuriously set to false.
		 */
		Assert(!scan->xs_want_itup);
		if (ScanDirectionIsForward(direction))
		{
			nextItem = scanPos->item + 1;
			hasNext = (nextItem <= scanBatch->lastItem);
		}
		else
		{
			nextItem = scanPos->item - 1;
			hasNext = (nextItem >= scanBatch->firstItem);
		}

		hscan->xs_lastinblock = hasNext &&
			ItemPointerGetBlockNumber(&scanBatch->items[nextItem].tableTid) !=
			ItemPointerGetBlockNumber(&scan->xs_heaptid);

		return &scan->xs_heaptid;
	}

	/*
	 * Index-only scan.
	 *
	 * Also set xs_itup, which caller also needs.
	 */
	Assert(scan->xs_want_itup && !hscan->xs_lastinblock);
	scan->xs_itup = (IndexTuple) (scanBatch->currTuples +
								  scanBatch->items[scanPos->item].tupleOffset);

	/*
	 * Set visibility info for the current scanPos item (plus possibly some
	 * additional items in the current scan direction) as needed
	 */
	hbatch = heap_batch_data(scan, scanBatch);
	if (!(hbatch->visInfo[scanPos->item] & HEAP_BATCH_VIS_CHECKED))
		heapam_index_batch_pos_visibility(scan, direction, scanBatch, hbatch,
										  scanPos);

	/* Finally, set all_visible for caller */
	*all_visible =
		(hbatch->visInfo[scanPos->item] & HEAP_BATCH_VIS_ALL_VISIBLE) != 0;

	return &scan->xs_heaptid;
}

/*
 * Obtain visibility information for a TID from caller's batch.
 *
 * Called during amgetbatch index-only scans.  We always check the visibility
 * of caller's item (an offset into caller's batch->items[] array).  We might
 * also set visibility info for other items from caller's batch more
 * proactively when that makes sense.
 *
 * We keep two competing considerations in balance when determining whether to
 * check additional items: the need to keep the cost of visibility map access
 * under control when most items will never be returned by the scan anyway
 * (important for inner index scans of anti-joins and semi-joins), and the
 * need to unguard batches promptly.
 *
 * In no event will the scan be allowed to guard more than one batch at a
 * time.  The primary reason for this restriction is to avoid unintended
 * interactions with the read stream, which has its own strategy for keeping
 * the number of pins held by the backend under control.  (Unguarding via
 * the amunguardbatch callback often means releasing a buffer pin on an
 * index page, which counts against the same shared pin limit.)
 *
 * Once we've resolved visibility for all items in a batch, we can safely
 * unguard it by calling amunguardbatch.  This is safe with respect to
 * concurrent VACUUM because the batch's guard (typically a buffer pin on the
 * originating index page) blocks VACUUM from acquiring a conflicting cleanup
 * lock on that page.  Copying the relevant visibility map data into our local
 * cache suffices to prevent unsafe concurrent TID recycling: if any of these
 * TIDs point to dead heap tuples, VACUUM cannot possibly return from
 * ambulkdelete and mark the pointed-to heap pages as all-visible.  VACUUM
 * _can_ do so once the batch is unguarded, but that's okay; we'll be working
 * off of cached visibility info that indicates that the dead TIDs are NOT
 * all-visible.
 *
 * What about the opposite case, where a page was all-visible when we cached
 * the VM bits but tuples on it are deleted afterwards?  That is safe too: any
 * tuple that was visible to all when we read the VM must also be visible to
 * our MVCC snapshot, so it is correct to skip the heap fetch for those TIDs.
 */
static void
heapam_index_batch_pos_visibility(IndexScanDesc scan, ScanDirection direction,
								  IndexScanBatch batch, HeapBatchData *hbatch,
								  BatchRingItemPos *pos)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan->xs_heapfetch;
	int			posItem = pos->item;
	bool		allbatchitemsvisible;
	BlockNumber curvmheapblkno = InvalidBlockNumber;
	uint8		curvmheapblkflags = 0;

	Assert(hbatch == heap_batch_data(scan, batch));

	/*
	 * The batch must still be guarded whenever we're called.
	 *
	 * amunguardbatch can't be called until we've already set _every_ batch
	 * item's visInfo[] status, but if we've already done so for this batch
	 * then it shouldn't ever get passed to us again by some subsequent call.
	 * (This relies on index-only scans always being !batchImmediateUnguard.)
	 */
	Assert(batch->isGuarded && !scan->batchImmediateUnguard);

	/*
	 * Set visibility info for a range of items, in scan order.
	 *
	 * Note: visibilitymap_get_status does not lock the visibility map buffer,
	 * so the result could be slightly stale.  See the "Memory ordering
	 * effects" discussion above visibilitymap_get_status for an explanation
	 * of why this is okay.
	 */
	if (ScanDirectionIsForward(direction))
	{
		int			lastSetItem = Min(batch->lastItem,
									  posItem + hscan->xs_vm_items - 1);

		for (int setItem = posItem; setItem <= lastSetItem; setItem++)
		{
			ItemPointer tid = &batch->items[setItem].tableTid;
			BlockNumber heapblkno = ItemPointerGetBlockNumber(tid);
			uint8		flags;

			if (heapblkno == curvmheapblkno)
			{
				hbatch->visInfo[setItem] = curvmheapblkflags;
				continue;
			}

			flags = HEAP_BATCH_VIS_CHECKED;
			if (VM_ALL_VISIBLE(scan->heapRelation, heapblkno, &hscan->xs_vmbuffer))
				flags |= HEAP_BATCH_VIS_ALL_VISIBLE;

			hbatch->visInfo[setItem] = curvmheapblkflags = flags;
			curvmheapblkno = heapblkno;
		}

		allbatchitemsvisible = lastSetItem >= batch->lastItem &&
			(posItem == batch->firstItem ||
			 (hbatch->visInfo[batch->firstItem] & HEAP_BATCH_VIS_CHECKED));
	}
	else
	{
		int			lastSetItem = Max(batch->firstItem,
									  posItem - hscan->xs_vm_items + 1);

		for (int setItem = posItem; setItem >= lastSetItem; setItem--)
		{
			ItemPointer tid = &batch->items[setItem].tableTid;
			BlockNumber heapblkno = ItemPointerGetBlockNumber(tid);
			uint8		flags;

			if (heapblkno == curvmheapblkno)
			{
				hbatch->visInfo[setItem] = curvmheapblkflags;
				continue;
			}

			flags = HEAP_BATCH_VIS_CHECKED;
			if (VM_ALL_VISIBLE(scan->heapRelation, heapblkno, &hscan->xs_vmbuffer))
				flags |= HEAP_BATCH_VIS_ALL_VISIBLE;

			hbatch->visInfo[setItem] = curvmheapblkflags = flags;
			curvmheapblkno = heapblkno;
		}

		allbatchitemsvisible = lastSetItem <= batch->firstItem &&
			(posItem == batch->lastItem ||
			 (hbatch->visInfo[batch->lastItem] & HEAP_BATCH_VIS_CHECKED));
	}

	/*
	 * It's safe to unguard the batch (via amunguardbatch) as soon as we've
	 * resolved the visibility status of all of its items (unless this is a
	 * non-MVCC scan)
	 */
	if (allbatchitemsvisible)
	{
		Assert(hbatch->visInfo[batch->firstItem] & HEAP_BATCH_VIS_CHECKED);
		Assert(hbatch->visInfo[batch->lastItem] & HEAP_BATCH_VIS_CHECKED);

		/*
		 * Note: nodeIndexonlyscan.c only supports MVCC snapshots, but we
		 * still cope with index-only scan callers with other snapshot types.
		 * This is certainly not unexpected; selfuncs.c performs index-only
		 * scans that use SnapshotNonVacuumable.
		 */
		if (scan->MVCCScan)
			tableam_util_unguard_batch(scan, batch);
	}

	/*
	 * Else check visibility for twice as many items next time, or all items.
	 * We check all items in one go once we're passed the scan's first batch.
	 */
	else if (hscan->xs_vm_items < (batch->lastItem - batch->firstItem))
		hscan->xs_vm_items *= 2;
	else
		hscan->xs_vm_items = scan->maxitemsbatch;
}

/*
 * Handle a change in index scan direction (at the tuple granularity).
 *
 * Resets the read stream, since we can't rely on scanPos continuing to agree
 * with the blocks that read stream already consumed using prefetchPos.
 *
 * Note: iff the scan _continues_ in this new direction, and actually steps
 * off scanBatch to an earlier index page, tableam_util_fetch_next_batch will
 * deal with it.  But that might never happen; the scan might yet change
 * direction again (or just end before returning more items).
 */
static pg_noinline void
heapam_index_dirchange_reset(IndexFetchHeapData *hscan,
							 ScanDirection direction,
							 BatchRingBuffer *batchringbuf)
{
	/* Reset read stream state */
	batchringbuf->prefetchPos.valid = false;
	hscan->xs_paused = false;
	hscan->xs_read_stream_dir = direction;

	/* Reset read stream itself */
	if (hscan->xs_read_stream)
		read_stream_reset(hscan->xs_read_stream);
}

/*
 * Decide whether to start a read stream for heap block prefetching during an
 * index scan.
 *
 * Called each time a new batch is obtained from the index AM, barring the
 * first time that happens.  We delay initializing the stream until reading
 * from the scan's second batch.  This heuristic avoids wasting cycles on
 * starting a read stream for very selective index scans.
 *
 * We avoid prefetching during scans where we're unable to unguard (unpin)
 * each batch's buffers right away (non-MVCC snapshot scans).  We are not
 * prepared to sensibly limit the total number of buffer pins held (read
 * stream handles all pin resource management for us, and knows nothing
 * about pins held on index pages/within batches).
 *
 * We also delay creating a read stream during index-only scans that haven't
 * done any heap fetches yet.  We don't want to waste any cycles on
 * allocating a read stream until we have a demonstrated need to perform
 * heap fetches.
 */
static pg_attribute_always_inline void
heapam_index_consider_prefetching(IndexScanDesc scan,
								  IndexFetchHeapData *hscan)
{
	Assert(!hscan->xs_read_stream);
	Assert(!scan->batchringbuf.prefetchPos.valid);

	if (scan->MVCCScan && enable_indexscan_prefetch &&
		hscan->xs_blk != InvalidBlockNumber)	/* for index-only scans */
		hscan->xs_read_stream =
			read_stream_begin_relation(READ_STREAM_DEFAULT, NULL,
									   scan->heapRelation, MAIN_FORKNUM,
									   heapam_index_prefetch_next_block,
									   scan, 0);
	/* else don't start a read stream for prefetching (not yet, at least) */
}

/*
 * Return the next block to the read stream when performing index prefetching.
 *
 * The initial batch is always loaded by heapam_index_getnext_scanbatch_pos.
 * We don't get called until the first read_stream_next_buffer call, when a
 * heap block is requested from the scan's stream for the first time.
 *
 * The position of the read_stream is stored in prefetchPos.  It is typical
 * for prefetchPos to consistently stay ahead of the scanPos position that's
 * used to track the next TID heapam_index_getnext_scanbatch_pos will return
 * to the scan (after the first time we get called).  However, that isn't a
 * strict precondition (though as explained below we implement a scheme
 * essentially equivalent to making it a strict precondition).  There is a
 * true strict postcondition, though: when we return we'll always leave
 * scanPos <= prefetchPos.
 */
static BlockNumber
heapam_index_prefetch_next_block(ReadStream *stream,
								 void *callback_private_data,
								 void *per_buffer_data)
{
	IndexScanDesc scan = (IndexScanDesc) callback_private_data;
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan->xs_heapfetch;
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos = &batchringbuf->scanPos;
	BatchRingItemPos *prefetchPos = &batchringbuf->prefetchPos;
	ScanDirection xs_read_stream_dir = hscan->xs_read_stream_dir;
	IndexScanBatch prefetchBatch;
	bool		fromScanPos = false;

	/*
	 * scanPos must always be valid when prefetching takes place.  There has
	 * to be at least one batch, loaded as our scanBatch.  The scan direction
	 * must be established, too.
	 */
	Assert(index_scan_batch_count(scan) > 0);
	Assert(scanPos->valid && index_scan_batch_loaded(scan, scanPos->batch));
	Assert(scan->MVCCScan);
	Assert(!hscan->xs_paused);
	Assert(xs_read_stream_dir != NoMovementScanDirection);

	/*
	 * Handle initialization on the first call here, when prefetchPos isn't
	 * yet valid (also handles the prefetchPos < scanPos edge case).
	 *
	 * If prefetchPos has not been initialized yet, that typically indicates
	 * that this is the first call here for the entire scan.  We initialize
	 * prefetchPos using the current scanPos, since the current scanBatch
	 * item's TID should have its block number returned by the read stream
	 * first.  When this happens, it's likely that prefetchPos will get ahead
	 * of scanPos very soon, after the _next_ call here returns.
	 *
	 * There's also an edge case that we handle using exactly the same steps.
	 * It's possible for prefetchPos to "fall behind" scanPos, at least in a
	 * trivial sense: if many adjacent matching items contain TIDs that all
	 * point to the same heap block, scanPos can actually overtake prefetchPos
	 * (prefetchPos can't advance until we're actually called).  A similar
	 * issue arises during index-only scans that require only a few heap
	 * fetches: we'll tend to be called far less often than we'd be called
	 * during an equivalent plain index scan due to all-visible items.  An
	 * all-visible item will advance scanPos, but can't trigger a call to here
	 * (just like an item that points to the same heap block that the previous
	 * item also pointed to).
	 *
	 * This scheme produces exactly the same block prefetch requests as a
	 * scheme that requires heapam_index_getnext_scanbatch_pos to actively
	 * ensure that "prefetchPos < scanPos" can never happen.  That isn't a
	 * strict precondition for this function because making it explicit would
	 * impose a performance penalty on heapam_index_getnext_scanbatch_pos.
	 *
	 * Note: when heapam_index_getnext_scanbatch_pos frees a batch that
	 * prefetchPos points to, it'll at least invalidate prefetchPos for us.
	 * This removes any danger of prefetchPos.batch falling so far behind
	 * scanPos.batch that it wraps around (and appears to be ahead of scanPos
	 * instead of behind it).  In other words, in a certain sense we actually
	 * _can_ trust heapam_index_getnext_scanbatch_pos to not let prefetchPos
	 * fall behind scanPos: it can't happen at the batch granularity (only at
	 * the item/tuple granularity, which we can always cope with here).
	 */
	if (!prefetchPos->valid ||
		index_scan_pos_cmp(prefetchPos, scanPos, xs_read_stream_dir) < 0)
	{
		hscan->xs_prefetch_block = InvalidBlockNumber;
		*prefetchPos = *scanPos;
		fromScanPos = true;

		/*
		 * We must avoid keeping any batch guarded for more than an instant,
		 * to avoid undesirable interactions with the scan's read stream. See
		 * comment and assertion at the top of the loop below.
		 */
		if (scan->xs_want_itup)
		{
			/*
			 * Make heapam_index_batch_pos_visibility release resources
			 * eagerly
			 */
			hscan->xs_vm_items = scan->maxitemsbatch;

			/* Make sure that this new prefetchBatch is unguarded */
			prefetchBatch = index_scan_batch(scan, prefetchPos->batch);
			if (prefetchBatch->isGuarded)
			{
				HeapBatchData *hbatch = heap_batch_data(scan, prefetchBatch);

				/* Set visibility info not set through scanBatch */
				heapam_index_batch_pos_visibility(scan, xs_read_stream_dir,
												  prefetchBatch, hbatch,
												  prefetchPos);
			}
		}
	}

	prefetchBatch = index_scan_batch(scan, prefetchPos->batch);

	/*
	 * If prefetchPos wasn't just initialized using scanPos, we're directly
	 * picking up prefetching where the last call here left off.  Assert that
	 * xs_prefetch_block matches the last item we returned as expected.
	 *
	 * Note: we don't actually need a xs_prefetch_block field at all; we could
	 * just take the last block we returned from prefetchPos directly instead.
	 * But maintaining xs_prefetch_block explicitly is slightly more robust.
	 * It gives us a way to make sure that the last call here left prefetchPos
	 * in a consistent state (e.g., when the read stream had to be paused).
	 */
#ifdef USE_ASSERT_CHECKING
	if (!fromScanPos)
	{
		BatchMatchingItem *lastitem = &prefetchBatch->items[prefetchPos->item];
		BlockNumber last_block = ItemPointerGetBlockNumber(&lastitem->tableTid);

		Assert(last_block == hscan->xs_prefetch_block);
	}
#endif

	for (;;)
	{
		BatchMatchingItem *item;
		BlockNumber prefetch_block;

		/*
		 * We never call amgetbatch without immediately unguarding the batch,
		 * either within the index AM or here (when we eagerly load all of the
		 * batch's visibility information during an index-only scan).  The
		 * index AM won't hold onto TID interlock buffer pins, keeping the
		 * absolute number of pins held to a minimum.
		 *
		 * This is defensive.  The read stream tries to be careful about not
		 * pinning too many buffers, and that's harder to do reliably if there
		 * are variable numbers of pins taken without such care.
		 */
		Assert(!prefetchBatch->isGuarded);
		if (fromScanPos)
		{
			/*
			 * Don't increment item when prefetchPos was just initialized
			 * using scanPos.  We'll return the scanPos item's heap block
			 * directly on the first call here.  In other words, we'll return
			 * the heap block from TID passed to heapam_index_fetch_tuple_impl
			 * at the point where it called read_stream_next_buffer for the
			 * first time during the scan. (As explained above, we also end up
			 * here during the first call to read_stream_next_buffer following
			 * prefetchPos falling behind scanPos/being invalidated for us.)
			 */
			fromScanPos = false;
		}
		else if (!index_scan_pos_advance(xs_read_stream_dir,
										 prefetchBatch, prefetchPos))
		{
			/*
			 * Ran out of items from prefetchBatch.  Try to advance to the
			 * scan's next batch.
			 */
			if (unlikely(index_scan_batch_full(scan)))
			{
				/*
				 * Can't advance prefetchBatch because all available
				 * batchringbuf batch slots are currently in use.
				 *
				 * Deal with this by momentarily pausing the read stream.
				 * heapam_index_getnext_scanbatch_pos will resume the read
				 * stream later, though only after scanPos has consumed all
				 * remaining items from scanBatch (at which point the current
				 * head batch will be freed, making a slot available for reuse
				 * here by us).
				 *
				 * In practice we hardly ever need to do this.  It would be
				 * possible to avoid the need to pause the read stream by
				 * dynamically allocating slots, but that would add complexity
				 * for no real benefit.  It also seems like a good idea to
				 * impose some hard limit on the number of batches that
				 * prefetchPos can get ahead of scanPos by (especially in the
				 * case of index-only scans, where we often won't have any
				 * heap block to return from most of the scan's batches).
				 */
				hscan->xs_paused = true;

				/*
				 * Before returning, advance prefetchPos in the opposite
				 * direction to the one used by the scan.  This undoes the
				 * effects of the most recent advance.  We're not going to
				 * return any block, so it seems like a good idea to leave
				 * prefetchPos in a state consistent with that.
				 */
				if (ScanDirectionIsForward(xs_read_stream_dir))
				{
					Assert(prefetchPos->item == prefetchBatch->lastItem + 1);
					prefetchPos->item = prefetchBatch->lastItem;
				}
				else
				{
					Assert(prefetchPos->item == prefetchBatch->firstItem - 1);
					prefetchPos->item = prefetchBatch->firstItem;
				}

				return read_stream_pause(stream);
			}

			prefetchBatch = tableam_util_fetch_next_batch(scan,
														  xs_read_stream_dir,
														  prefetchBatch,
														  prefetchPos);
			if (!prefetchBatch)
			{
				/*
				 * No more batches in this direction, so all the batches that
				 * the scan will ever require have now been returned
				 */
				return InvalidBlockNumber;
			}

			/* Position prefetchPos to the start of new prefetchBatch */
			index_scan_pos_nextbatch(xs_read_stream_dir,
									 prefetchBatch, prefetchPos);

			if (scan->xs_want_itup)
			{
				HeapBatchData *hbatch = heap_batch_data(scan, prefetchBatch);

				/* make sure we have visibility info for the entire batch */
				Assert(hscan->xs_vm_items == scan->maxitemsbatch);
				heapam_index_batch_pos_visibility(scan, xs_read_stream_dir,
												  prefetchBatch, hbatch,
												  prefetchPos);
			}
		}

		/*
		 * prefetchPos now points to the next item whose TID's heap block
		 * number might need to be prefetched
		 */
		Assert(index_scan_batch(scan, prefetchPos->batch) == prefetchBatch);
		Assert(prefetchPos->item >= prefetchBatch->firstItem &&
			   prefetchPos->item <= prefetchBatch->lastItem);
		/* scanPos is always <= prefetchPos when we return */
		Assert(index_scan_pos_cmp(scanPos, prefetchPos, xs_read_stream_dir) <= 0);

		if (scan->xs_want_itup)
		{
			HeapBatchData *hbatch = heap_batch_data(scan, prefetchBatch);

			Assert(hbatch->visInfo[prefetchPos->item] & HEAP_BATCH_VIS_CHECKED);
			if (hbatch->visInfo[prefetchPos->item] & HEAP_BATCH_VIS_ALL_VISIBLE)
			{
				/* item is known to be all-visible -- don't prefetch */
				continue;
			}
		}

		item = &prefetchBatch->items[prefetchPos->item];
		prefetch_block = ItemPointerGetBlockNumber(&item->tableTid);

		if (prefetch_block == hscan->xs_prefetch_block)
		{
			/*
			 * prefetch_block matches the last prefetchPos item's TID's heap
			 * block number; we must not return the same prefetch_block twice
			 * (twice in succession)
			 */
			continue;
		}

		/* We have a new heap block number to return to read stream */
		hscan->xs_prefetch_block = prefetch_block;
		return prefetch_block;
	}

	return InvalidBlockNumber;
}
