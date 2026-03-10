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
#include "access/relscan.h"
#include "access/visibilitymap.h"
#include "storage/predicate.h"
#include "utils/pgstat_internal.h"


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
																 bool index_only);
static pg_attribute_always_inline bool heapam_index_fetch_heap(IndexScanDesc scan,
															   IndexFetchHeapData *hscan,
															   TupleTableSlot *slot,
															   bool *heap_continue);

/* ------------------------------------------------------------------------
 * Index Scan Callbacks for heap AM
 * ------------------------------------------------------------------------
 */

IndexFetchTableData *
heapam_index_fetch_begin(IndexScanDesc scan, uint32 flags)
{
	IndexFetchHeapData *hscan = palloc0_object(IndexFetchHeapData);

	hscan->xs_base.flags = flags;
	hscan->xs_cbuf = InvalidBuffer;
	hscan->xs_blk = InvalidBlockNumber;
	hscan->xs_vmbuffer = InvalidBuffer;

	/* Resolve which getnext_slot implementation to use for this scan */
	if (scan->xs_want_itup)
		scan->xs_getnext_slot = heapam_index_only_amgettuple_next;
	else
		scan->xs_getnext_slot = heapam_index_plain_amgettuple_next;

	return &hscan->xs_base;
}

void
heapam_index_fetch_reset(IndexScanDesc scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan->xs_heapfetch;

	if (BufferIsValid(hscan->xs_cbuf))
	{
		ReleaseBuffer(hscan->xs_cbuf);
		hscan->xs_cbuf = InvalidBuffer;
		hscan->xs_blk = InvalidBlockNumber;
	}

	if (BufferIsValid(hscan->xs_vmbuffer))
	{
		ReleaseBuffer(hscan->xs_vmbuffer);
		hscan->xs_vmbuffer = InvalidBuffer;
	}
}

void
heapam_index_fetch_end(IndexScanDesc scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan->xs_heapfetch;

	heapam_index_fetch_reset(scan);

	pfree(hscan);
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

/* table_index_getnext_slot callback: amgettuple, plain index scan */
static pg_attribute_hot bool
heapam_index_plain_amgettuple_next(IndexScanDesc scan,
								   ScanDirection direction,
								   TupleTableSlot *slot)
{
	Assert(!scan->xs_want_itup);
	Assert(scan->indexRelation->rd_indam->amgettuple != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, false);
}

/* table_index_getnext_slot callback: amgettuple, index-only scan */
static pg_attribute_hot bool
heapam_index_only_amgettuple_next(IndexScanDesc scan,
								  ScanDirection direction,
								  TupleTableSlot *slot)
{
	Assert(scan->xs_want_itup);
	Assert(scan->indexRelation->rd_indam->amgettuple != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, true);
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

		if (BufferIsValid(hscan->xs_cbuf))
			ReleaseBuffer(hscan->xs_cbuf);

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
		ExecStoreBufferHeapTuple(&bslot->base.tupdata, slot, hscan->xs_cbuf);
	}
	else
	{
		/* We've reached the end of the HOT chain. */
		*heap_continue = false;
	}

	return got_heap_tuple;
}

/*
 * Common implementation for both heapam_index_*_getnext_slot variants.
 *
 * The result is true if a tuple satisfying the scan keys and the snapshot was
 * found, false otherwise.  The tuple is stored in the specified slot.
 *
 * On success, resources (like buffer pins) are likely to be held, and will be
 * dropped by a future call here (or by a later call to heapam_index_fetch_end
 * through index_endscan).
 *
 * The index_only parameter is a compile-time constant at each call site,
 * allowing the compiler to specialize the code for each variant.
 */
static pg_attribute_always_inline bool
heapam_index_getnext_slot(IndexScanDesc scan, ScanDirection direction,
						  TupleTableSlot *slot, bool index_only)
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
			tid = index_getnext_tid(scan, direction);

			/* If we're out of index entries, we're done */
			if (tid == NULL)
				break;

			/* For index-only scans, check the visibility map */
			if (index_only)
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
											 heap_continue))
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
				Assert(!(*heap_continue && IsMVCCSnapshot(scan->xs_snapshot)));
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
			if (heapam_index_fetch_heap(scan, hscan, slot, heap_continue))
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
						TupleTableSlot *slot, bool *heap_continue)
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
	 * If we scanned a whole HOT chain and found only dead tuples, tell index
	 * AM to kill its entry for that TID (this will take effect in the next
	 * amgettuple call, in index_getnext_tid).  We do not do this when in
	 * recovery because it may violate MVCC to do so.  See comments in
	 * RelationGetIndexScan().
	 */
	if (!scan->xactStartedInRecovery)
		scan->kill_prior_tuple = all_dead;

	return found;
}
