/*-------------------------------------------------------------------------
 *
 * indexbatch.c
 *	  Batch-based index scan infrastructure for the amgetbatch interface.
 *
 * This module provides the core infrastructure for batch-based index scans,
 * which allow index AMs to return multiple matching TIDs per page in a single
 * call.  The batch ring buffer is owned by the table AM, typically maintained
 * alongside a read stream used for prefetching table blocks.
 *
 * The ring buffer loads batches in index key space/index scan order.  This
 * allows the table AM to maintain an adequate prefetch distance: its read
 * stream callback is thereby able to request table blocks referenced by index
 * pages that are well ahead of the current scan position's index page.
 *
 * Most functions here are table AM utilities (tableam_util_*), called by
 * table AMs during amgetbatch index scans.  These manage the batch ring
 * buffer's lifecycle and positional state, and help with certain aspects of
 * resource management.  The table AM uses scanPos (and its scanBatch batch)
 * to return items from batches returned by amgetbatch.  Table AMs that
 * support index I/O prefetching use prefetchPos (and its prefetchBatch batch)
 * by implementing a read stream callback that consumes items well ahead of
 * scanPos.
 *
 * There are also some index AM utilities (indexam_util_*), called by index
 * AMs that implement the amgetbatch interface, to help manage resources like
 * memory, locks, and buffer pins.  Index AMs free and unlock batches as
 * described in indexam.sgml.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/indexbatch.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/indexbatch.h"
#include "access/tableam.h"
#include "common/int.h"
#include "lib/qunique.h"

static void batch_free(IndexScanDesc scan, IndexScanBatch batch,
					   bool allow_cache);
static inline bool batch_cache_store(IndexScanDesc scan, IndexScanBatch batch);
static int	batch_compare_int(const void *va, const void *vb);

/*
 * Reset ring buffer and related positional state used during an amgetbatch
 * index scan
 */
void
tableam_util_batchscan_reset(IndexScanDesc scan, bool endscan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	IndexScanBatch markBatch = batchringbuf->markBatch;
	bool		markBatchFreed = false;

	batchringbuf->scanPos.valid = false;
	batchringbuf->prefetchPos.valid = false;
	batchringbuf->markPos.valid = false;

	/* Ensure batch_free won't skip the old markBatch in the loop below */
	batchringbuf->markBatch = NULL;

	for (uint8 i = batchringbuf->headBatch; i != batchringbuf->nextBatch; i++)
	{
		IndexScanBatch batch = index_scan_batch(scan, i);

		if (batch == markBatch)
			markBatchFreed = true;

		batch_free(scan, batch, !endscan);
	}

	if (!markBatchFreed && unlikely(markBatch))
		batch_free(scan, markBatch, !endscan);

	batchringbuf->headBatch = 0;
	batchringbuf->nextBatch = 0;
}

/*
 * Free resources at end of a batch index scan.
 *
 * Called by table AM when an index scan is ending, right before the owning
 * scan descriptor goes away.  Cleans up all batch related resources.
 */
void
tableam_util_batchscan_end(IndexScanDesc scan)
{
	/* Free all remaining loaded batches (even markBatch), bypassing cache */
	tableam_util_batchscan_reset(scan, true);

	for (int i = 0; i < INDEX_SCAN_CACHE_BATCHES; i++)
	{
		IndexScanBatch cached = scan->batchcache[i];

		if (cached == NULL)
			continue;

		if (cached->deadItems)
			pfree(cached->deadItems);
		pfree(index_scan_batch_base(scan, cached));
	}
}

/*
 * Set a mark from scanPos position
 *
 * Called from the table AM's index_fetch_markpos callback.  Saves the current
 * scan position and associated batch so that the scan can be restored to this
 * point later, via tableam_util_batchscan_restore_pos.  The marked batch is
 * retained and not freed until a new mark is set or the scan ends (or until
 * the mark is restored).
 */
void
tableam_util_batchscan_mark_pos(IndexScanDesc scan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	BatchRingItemPos *markPos = &batchringbuf->markPos;
	IndexScanBatch scanBatch = index_scan_batch(scan, scanPos->batch);
	IndexScanBatch markBatch = batchringbuf->markBatch;
	bool		freeMarkBatch;

	Assert(scan->MVCCScan);

	/*
	 * Free the previous mark batch (if any) -- but only if it isn't our
	 * scanBatch (defensively make sure that markBatch isn't some later
	 * still-needed batch, too)
	 */
	if (!markBatch || markBatch == scanBatch)
	{
		/* Definitely no markBatch that we should free now */
		freeMarkBatch = false;
	}
	else if (likely(!index_scan_batch_loaded(scan, markPos->batch)))
	{
		/* Definitely have a no-longer-loaded markBatch to free */
		freeMarkBatch = true;
	}
	else
	{
		/*
		 * index_scan_batch_loaded indicates that markPos->batch is loaded,
		 * but after uint8 wraparound a stale batch offset can alias a
		 * currently-loaded range (false positive).  Confirm by checking
		 * whether the batch pointer in markPos->batch's slot still matches.
		 */
		freeMarkBatch = (index_scan_batch(scan, markPos->batch) != markBatch);
	}

	if (freeMarkBatch)
	{
		/* Free markBatch, since it isn't loaded/needed for batchringbuf */
		batchringbuf->markBatch = NULL; /* else call won't free markBatch */
		tableam_util_free_batch(scan, markBatch);
	}

	/* copy the scan's position */
	batchringbuf->markPos = *scanPos;
	batchringbuf->markBatch = scanBatch;
}

/*
 * Restore scanPos to the previously saved markPos position.
 *
 * Called from the table AM's index_fetch_restrpos callback.  Restores the
 * scan to a position saved using tableam_util_batchscan_mark_pos earlier.
 * The scan's markPos becomes its scanPos.  The marked batch is restored as
 * the current scanBatch when needed.
 *
 * We just discard all batches (other than markBatch/restored scanBatch),
 * except when markBatch is already the scan's current scanBatch.  We always
 * invalidate prefetchPos.  The read stream and related prefetching state are
 * reset by the caller (which calls this function as it resets that state).
 * This approach keeps things simple for table AMs: most code that deals with
 * batches is thereby able to assume that the common case where scan direction
 * never changes is the only case.
 */
void
tableam_util_batchscan_restore_pos(IndexScanDesc scan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	BatchRingItemPos *markPos = &batchringbuf->markPos;
	IndexScanBatch markBatch = batchringbuf->markBatch;
	IndexScanBatch scanBatch = index_scan_batch(scan, scanPos->batch);

	Assert(scan->MVCCScan);
	Assert(scan->xs_heapfetch);
	Assert(markPos->valid);

	/*
	 * Restoring a mark always requires stopping prefetching.  This is similar
	 * to the handling table AMs implement to deal with a tuple-level change
	 * in the scan's direction.  The read stream must have already been reset
	 * by the caller (via table_index_fetch_reset).
	 */
	batchringbuf->prefetchPos.valid = false;

	if (scanBatch == markBatch)
	{
		/* markBatch is already scanBatch; needn't change batchringbuf */
		Assert(scanPos->batch == markPos->batch);

		scanPos->item = markPos->item;
		return;
	}

	/*
	 * markBatch is behind scanBatch, and so must not be saved in ring buffer
	 * anymore.  We have to deal with restoring the mark the hard way: by
	 * invalidating all other loaded batches.  This is similar to the case
	 * where the scan direction changes and the scan actually crosses
	 * batch/index page boundaries (see tableam_util_scanbatch_dirchange).
	 *
	 * First, free all batches that are still in the ring buffer.
	 */
	for (uint8 i = batchringbuf->headBatch; i != batchringbuf->nextBatch; i++)
	{
		IndexScanBatch batch = index_scan_batch(scan, i);

		Assert(batch != markBatch);

		tableam_util_free_batch(scan, batch);
	}

	/*
	 * Next "append" standalone markBatch, which will become scanBatch
	 * (scanBatch is always the ring buffer's headBatch)
	 */
	markPos->batch = 0;
	batchringbuf->scanPos = *markPos;
	batchringbuf->nextBatch = batchringbuf->headBatch = markPos->batch;
	index_scan_batch_append(scan, markBatch);
	Assert(index_scan_batch(scan, batchringbuf->scanPos.batch) == markBatch);

	/*
	 * Finally, call amposreset to let index AM know to invalidate any private
	 * state that independently tracks the scan's progress
	 */
	if (scan->indexRelation->rd_indam->amposreset)
		scan->indexRelation->rd_indam->amposreset(scan, markBatch);

	/*
	 * Note: markBatch.deadItems[] might already contain dead items, and might
	 * yet have more dead items saved.  tableam_util_free_batch is prepared
	 * for that.
	 */
}

/*
 * Handle cross-batch change in scan direction
 *
 * Called by table AM when its scan changes direction in a way that
 * necessitates backing the scan up to an index page originally associated
 * with a now-freed batch.
 *
 * When we return, batchringbuf will only contain one batch (the current
 * headBatch/scanBatch) and will look as if the new scan direction had been
 * used from the start.  Caller can then safely pass this batch to amgetbatch
 * to determine which batch comes next in the new scan direction.  This
 * approach isn't particularly efficient, but it works well enough for what
 * ought to be a relatively rare occurrence.
 *
 * Caller must have reset the scan's read stream before calling here.  That
 * needs to happen as soon as the scan requests a tuple in whatever scan
 * direction is opposite-to-current.  We only deal with the case where the
 * scan backs up by enough items to cross a batch boundary (when the scan
 * resumes scanning in its original direction/ends before crossing a boundary,
 * there isn't any need to call here).
 */
void
tableam_util_scanbatch_dirchange(IndexScanDesc scan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	IndexScanBatch scanBatch;

	/*
	 * Release batches starting from the current "tail" batch, working
	 * backwards until the current head batch (which is also the current
	 * scanBatch) is the only batch hasn't been freed
	 */
	while (index_scan_batch_count(scan) > 1)
	{
		uint8		tailidx = batchringbuf->nextBatch - 1;
		IndexScanBatch tail = index_scan_batch(scan, tailidx);

		Assert(tailidx != batchringbuf->scanPos.batch);

		tableam_util_free_batch(scan, tail);
		batchringbuf->nextBatch--;
	}

	/* scanBatch is now the only batch still loaded */
	Assert(batchringbuf->headBatch == batchringbuf->scanPos.batch);
	scanBatch = index_scan_batch(scan, batchringbuf->headBatch);

	/*
	 * Flip scanBatch's scan direction to reflect the reversal.  Also reset
	 * any index AM state that independently tracks scan progress.
	 */
	scanBatch->dir = -scanBatch->dir;
	if (scan->indexRelation->rd_indam->amposreset)
		scan->indexRelation->rd_indam->amposreset(scan, scanBatch);
}

/*
 * Record that scanPos item is dead
 *
 * Records an offset to the current scanBatch/scanPos item, saving it in
 * scanBatch's deadItems array.  The items' index tuples will later be
 * marked LP_DEAD when current scanBatch is freed.
 */
void
tableam_util_scanpos_killitem(IndexScanDesc scan)
{
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	IndexScanBatch scanBatch = index_scan_batch(scan, scanPos->batch);

	if (scanBatch->deadItems == NULL)
		scanBatch->deadItems = palloc_array(int, scan->maxitemsbatch);
	if (scanBatch->numDead < scan->maxitemsbatch)
		scanBatch->deadItems[scanBatch->numDead++] = scanPos->item;
}

/*
 * Release resources associated with a batch
 *
 * Called by table AM's ordered index scan implementation when it is finished
 * with a batch and wishes to release its resources.
 *
 * We call amunguardbatch to drop the TID recycling interlock (e.g. buffer
 * pin) when it hasn't been dropped yet.  For plain MVCC scans (where
 * batchImmediateUnguard is set), the interlock was already dropped eagerly
 * in indexam_util_batch_unlock, so we skip the amunguardbatch call here.
 * Index-only scans must delay dropping the interlock until visibility is
 * resolved for all items in the batch, so amunguardbatch may still need to
 * act here.  For non-MVCC snapshot scans, the interlock is always held
 * until amunguardbatch drops it here -- this is the only place willing to
 * unguard a non-MVCC scan's batch.
 *
 * When the batch has dead items (numDead > 0) and the index AM provides an
 * amkillitemsbatch callback, we call it to set LP_DEAD bits in the index
 * page.  We always recycle the batch memory via indexam_util_batch_release.
 *
 * Note: Calling here when 'batch' is also batchringbuf.markBatch is a no-op.
 * Callers that don't want this should set batchringbuf.markBatch to NULL
 * before calling us.  Note that markBatch has to be explicitly freed.
 */
void
tableam_util_free_batch(IndexScanDesc scan, IndexScanBatch batch)
{
	/* Pass through to implementation function, with allow_cache=true */
	batch_free(scan, batch, true);
}

/*
 * Free a batch, optionally caching it for reuse.
 *
 * tableam_util_free_batch implementation function.  We split out the
 * implementation like this because we don't want to give external table AM
 * callers the option of passing allow_cache=false.
 *
 * When allow_cache is true, we try to store the batch in the scan's batch
 * cache for later reuse.  When allow_cache is false (typically because the
 * scan is shutting down), we pfree the caller's batch unconditionally.
 */
static void
batch_free(IndexScanDesc scan, IndexScanBatch batch, bool allow_cache)
{
	Assert(!(scan->batchImmediateUnguard && batch->isGuarded));
	Assert(batch->isGuarded || scan->MVCCScan);

	/* don't free caller's batch if it is scan's current markBatch */
	if (batch == scan->batchringbuf.markBatch)
		return;

	/* Drop TID recycling interlock via amunguardbatch as needed */
	if (!scan->batchImmediateUnguard && batch->isGuarded)
		tableam_util_unguard_batch(scan, batch);

	/*
	 * Let the index AM set LP_DEAD bits in the index page, if applicable.
	 *
	 * batch.deadItems[] is now in whatever order the scan returned items in.
	 * We might have even saved the same item/TID twice.
	 *
	 * Sort and unique-ify deadItems[].  That way the index AM can safely
	 * assume that items will always be in their original index page order.
	 */
	if (batch->numDead > 0 &&
		scan->indexRelation->rd_indam->amkillitemsbatch != NULL)
	{
		if (batch->numDead > 1)
		{
			qsort(batch->deadItems, batch->numDead, sizeof(int),
				  batch_compare_int);
			batch->numDead = qunique(batch->deadItems, batch->numDead,
									 sizeof(int), batch_compare_int);
		}

		scan->indexRelation->rd_indam->amkillitemsbatch(scan, batch);
	}

	/*
	 * Try to store caller's batch in this amgetbatch scan's cache of
	 * previously released batches first (when caller requests it)
	 */
	if (allow_cache && batch_cache_store(scan, batch))
		return;

	/* just pfree the caller's batch (plus batch's deadItems, if any) */
	if (batch->deadItems)
		pfree(batch->deadItems);
	pfree(index_scan_batch_base(scan, batch));
}

/*
 * Drop the batch's TID recycling interlock via amunguardbatch
 *
 * Called by the table AM when it's safe to drop whatever interlock the index
 * AM holds to prevent unsafe concurrent TID recycling by VACUUM (typically a
 * buffer pin on the batch's index page in batch's opaque area).
 */
void
tableam_util_unguard_batch(IndexScanDesc scan, IndexScanBatch batch)
{
	/* Should be called exactly once iff !batchImmediateUnguard */
	Assert(!scan->batchImmediateUnguard);
	Assert(batch->isGuarded);

	scan->indexRelation->rd_indam->amunguardbatch(scan, batch);

	batch->isGuarded = false;
}

/*
 * Unlock batch's index page buffer lock
 *
 * Unlocks the given buffer in preparation for amgetbatch returning items
 * saved in that batch.  Performs extra steps required by amgetbatch callers
 * in passing.
 *
 * Only call here when a batch has one or more matching items to return using
 * amgetbatch (or for amgetbitmap to load into its bitmap of matching TIDs).
 * When an index page has no matches, it's always safe for index AMs to drop
 * both the lock and the pin for themselves.
 *
 * Note: It is convenient for index AMs that implement both amgetbatch and
 * amgetbitmap to consistently use the same batch management approach, since
 * that avoids introducing special cases to lower-level code.  We drop both
 * the lock and the pin on batch's page on behalf of amgetbitmap callers.
 *
 * For amgetbatch callers, when batchImmediateUnguard is set (plain MVCC
 * scans), we also release the pin here (the TID recycling interlock), so
 * that no later amunguardbatch callback will be needed.  Otherwise the table
 * AM will call amunguardbatch later when it's safe to drop the interlock.
 *
 * Index AMs whose TID recycling interlock is not just a buffer pin, or whose
 * amunguardbatch does not simply release a pin, are not obligated to use this
 * function.  They can implement their own equivalent.  Such index AMs are also
 * free to use the batch LSN field themselves; their amkillitemsbatch routine
 * can use that LSN in the usual way, or in whatever way the AM deems necessary
 * (core code will not use it for any other purpose).
 */
void
indexam_util_batch_unlock(IndexScanDesc scan, IndexScanBatch batch, Buffer buf)
{
	/* batch must have one or more matching items returned by index AM */
	Assert(batch->firstItem >= 0 && batch->firstItem <= batch->lastItem);

	if (scan->usebatchring)
	{
		/* amgetbatch (not amgetbitmap) caller */
		Assert(scan->heapRelation != NULL);

		/*
		 * Have to set batch->lsn so that amkillitemsbatch has a way to detect
		 * when concurrent table TID recycling by VACUUM might have taken
		 * place.  It'll only be safe to set any index tuple LP_DEAD bits when
		 * the page LSN hasn't advanced.
		 *
		 * Plain MVCC scans (batchImmediateUnguard) also release the pin now,
		 * dropping the TID recycling interlock so that no amunguardbatch
		 * callback will be needed later.  The index AM caller must clear its
		 * own opaque buf field after we return.
		 *
		 * Non-immediate-unguard scans retain the pin; the table AM will call
		 * amunguardbatch to drop the interlock when ready.
		 */
		batch->lsn = BufferGetLSNAtomic(buf);
		if (scan->batchImmediateUnguard)
		{
			/* drop both the lock and the pin */
			UnlockReleaseBuffer(buf);
		}
		else
		{
			/* just drop the lock (hold on to interlock pin) */
			UnlockBuffer(buf);
		}

		/* If we released buffer pin, batch is now unguarded */
		batch->isGuarded = !scan->batchImmediateUnguard;
	}
	else
	{
		/* amgetbitmap (not amgetbatch) caller */
		Assert(scan->heapRelation == NULL);

		/* drop both the lock and the pin */
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Allocate a new batch
 *
 * Used by index AMs that support amgetbatch interface (both during amgetbatch
 * and amgetbitmap scans).
 *
 * Returns IndexScanBatch with space to fit scan->maxitemsbatch-many
 * BatchMatchingItem entries.  This will either be a newly allocated batch, or
 * a batch recycled from the cache managed by indexam_util_batch_release.  See
 * comments above indexam_util_batch_release.
 *
 * Housekeeping fields (buf, knownEndBackward/Forward, firstItem, lastItem,
 * numDead, deadItems, currTuples) are initialized here.  The table AM's
 * batch_init callback is invoked here to initialize the table AM opaque area.
 * The index AM caller is responsible for filling in its per-batch opaque
 * fields and the matching items[] array.
 *
 * Once the batch has the required matching items, caller should generally
 * pass it to indexam_util_batch_unlock, ahead of it being returned through
 * index AM's amgetbatch routine.  If it turns out that the batch won't need
 * to be returned like this (e.g., due to the scan having no more matches),
 * caller should pass its empty/unused batch to indexam_util_batch_release.
 */
IndexScanBatch
indexam_util_batch_alloc(IndexScanDesc scan)
{
	IndexScanBatch batch = NULL;
	bool		new_alloc = false;

	/*
	 * Lazily compute batch_table_offset on first allocation.  This combines
	 * the table AM and index AM opaque sizes into a single offset that can be
	 * used to find the table AM opaque area (and the true allocation base)
	 * from the batch pointer.
	 */
	if (scan->batch_table_offset == 0 &&
		(scan->batch_index_opaque_size > 0 ||
		 (scan->xs_heapfetch && scan->xs_heapfetch->batch_opaque_size > 0)))
	{
		uint16		table_opaque = scan->xs_heapfetch ?
			scan->xs_heapfetch->batch_opaque_size : 0;

		scan->batch_table_offset = table_opaque +
			scan->batch_index_opaque_size;
	}

	/* First look for an existing batch from the cache */
	if (scan->usebatchring)
	{
		for (int i = 0; i < INDEX_SCAN_CACHE_BATCHES; i++)
		{
			if (scan->batchcache[i] != NULL)
			{
				/* Return cached unreferenced batch */
				batch = scan->batchcache[i];
				scan->batchcache[i] = NULL;
				break;
			}
		}
	}
	else if (scan->batchcache[0] != NULL)
	{
		/*
		 * Reuse cached batch from prior amgetbitmap iteration.  This path is
		 * hit on every amgetbitmap call here after the scan's first.
		 */
		batch = scan->batchcache[0];
		scan->batchcache[0] = NULL;
	}

	if (!batch)
	{
		Size		prefix_sz;
		Size		base_sz;
		Size		trailing_sz;
		Size		allocsz;
		char	   *raw;

		/* AM opaque areas before the batch pointer */
		prefix_sz = scan->batch_table_offset;

		/* IndexScanBatchData header + items[] */
		base_sz = offsetof(IndexScanBatchData, items) +
			sizeof(BatchMatchingItem) * scan->maxitemsbatch;

		/*
		 * Trailing data after items[]: per-item data (owned by table AM),
		 * then currTuples workspace (owned by index AM, read by table AM)
		 */
		trailing_sz = 0;
		if (scan->xs_want_itup)
		{
			if (scan->xs_heapfetch &&
				scan->xs_heapfetch->batch_per_item_size > 0)
				trailing_sz += MAXALIGN(scan->xs_heapfetch->batch_per_item_size *
										scan->maxitemsbatch);
			trailing_sz += scan->batch_tuples_workspace;
		}

		allocsz = prefix_sz + MAXALIGN(base_sz) + trailing_sz;
		raw = palloc(allocsz);
		batch = (IndexScanBatch) (raw + prefix_sz);

		/* Set up currTuples pointer for index-only scans */
		if (scan->xs_want_itup && scan->batch_tuples_workspace > 0)
		{
			Size		itemsEnd = MAXALIGN(base_sz);
			Size		tableTrailing = 0;

			if (scan->xs_heapfetch &&
				scan->xs_heapfetch->batch_per_item_size > 0)
				tableTrailing = MAXALIGN(scan->xs_heapfetch->batch_per_item_size *
										 scan->maxitemsbatch);
			batch->currTuples = (char *) batch + itemsEnd + tableTrailing;
		}
		else
			batch->currTuples = NULL;

		/*
		 * Batches allocate deadItems lazily (though note that cached batches
		 * keep their deadItems allocation when recycled)
		 */
		batch->deadItems = NULL;
		new_alloc = true;
	}

	/* xs_want_itup scans must get a currTuples space */
	Assert(!(scan->xs_want_itup && scan->batch_tuples_workspace > 0 &&
			 batch->currTuples == NULL));

	/* Let the table AM initialize its per-batch opaque area */
	if (scan->xs_heapfetch)
		table_index_fetch_batch_init(scan, batch, new_alloc);

	/* shared initialization */
	batch->knownEndBackward = false;
	batch->knownEndForward = false;
	batch->isGuarded = false;
	batch->firstItem = -1;
	batch->lastItem = -1;
	batch->numDead = 0;

	return batch;
}

/*
 * Release allocated batch
 *
 * This function is called by index AMs to release a batch allocated by
 * indexam_util_batch_alloc.  Batches are cached here for reuse to reduce
 * palloc/pfree overhead.
 *
 * It's safe to release a batch immediately when it was used to read a page
 * that returned no matches to the scan.  Batches actually returned by index
 * AM's amgetbatch routine (i.e. batches for pages with one or more matches)
 * must be released by tableam_util_free_batch, which calls here after the
 * index AM's amkillitemsbatch routine (if any).  Index AMs that use batches
 * should call here to release a batch from their amgetbatch or amgetbitmap
 * routines.
 *
 * The rules for batch ownership differ slightly for amgetbitmap scans; see
 * the amgetbitmap documentation in doc/src/sgml/indexam.sgml for details.
 */
void
indexam_util_batch_release(IndexScanDesc scan, IndexScanBatch batch)
{
	if (!scan->usebatchring)
	{
		/*
		 * amgetbitmap scan caller.
		 *
		 * amgetbitmap routines are required to allocate no more than one
		 * batch at a time, so we'll always have a free slot.
		 */
		Assert(scan->batchcache[0] == NULL);
		Assert(scan->heapRelation == NULL);
		Assert(batch->deadItems == NULL);
		Assert(batch->currTuples == NULL);

		scan->batchcache[0] = batch;
		return;
	}

	/* amgetbatch scan caller */
	Assert(scan->heapRelation != NULL);

	/*
	 * Try to store caller's batch in this amgetbatch scan's cache of
	 * previously released batches first
	 */
	if (batch_cache_store(scan, batch))
		return;

	/* Cache full; just free the caller's batch */
	if (batch->deadItems)
		pfree(batch->deadItems);
	pfree(index_scan_batch_base(scan, batch));
}

/*
 * Try to store a batch in the scan's batch cache.
 *
 * Returns true if a free slot was found, false if the cache is full.
 */
static inline bool
batch_cache_store(IndexScanDesc scan, IndexScanBatch batch)
{
	for (int i = 0; i < INDEX_SCAN_CACHE_BATCHES; i++)
	{
		if (scan->batchcache[i] == NULL)
		{
			scan->batchcache[i] = batch;
			return true;
		}
	}

	return false;
}

/*
 * qsort comparison function for int arrays
 */
static int
batch_compare_int(const void *va, const void *vb)
{
	int			a = *((const int *) va);
	int			b = *((const int *) vb);

	return pg_cmp_s32(a, b);
}
