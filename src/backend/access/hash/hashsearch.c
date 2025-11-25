/*-------------------------------------------------------------------------
 *
 * hashsearch.c
 *	  search code for postgres hash tables
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashsearch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/relscan.h"
#include "miscadmin.h"
#include "executor/instrument_node.h"
#include "pgstat.h"
#include "storage/predicate.h"
#include "utils/rel.h"

static bool _hash_readpage(IndexScanDesc scan, Buffer buf, ScanDirection dir,
						   IndexScanBatch batch);
static int	_hash_load_qualified_items(IndexScanDesc scan, Page page,
									   OffsetNumber offnum, ScanDirection dir,
									   IndexScanBatch batch);
static inline void _hash_saveitem(IndexScanBatch batch, int itemIndex,
								  OffsetNumber offnum, IndexTuple itup);
static void _hash_readnext(IndexScanDesc scan, Buffer *bufp,
						   Page *pagep, HashPageOpaque *opaquep);

/*
 *	_hash_next() -- Get the next batch of items in a scan.
 *
 *		On entry, priorbatch describes the current page batch with items
 *		already returned.
 *
 *		On successful exit, returns a batch containing matching items from
 *		next page.  Otherwise returns NULL, indicating that there are no
 *		further matches.  No locks are ever held when we return.
 *
 *		Retains pins according to the same rules as _hash_first.
 */
IndexScanBatch
_hash_next(IndexScanDesc scan, ScanDirection dir, IndexScanBatch priorbatch)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	HashBatchData *hashpriorbatch = HashBatchGetData(priorbatch);
	BlockNumber blkno;
	Buffer		buf;
	IndexScanBatch batch;

	/*
	 * Determine which page to read next based on scan direction and details
	 * taken from the prior batch
	 */
	if (ScanDirectionIsForward(dir))
		blkno = hashpriorbatch->nextPage;
	else
		blkno = hashpriorbatch->prevPage;

	/*
	 * For bitmap scan callers, release the prior batch now so that the
	 * allocation below can reuse its memory.  This way bitmap scans never
	 * need more than one batch allocation.
	 */
	if (!scan->usebatchring)
		indexam_util_batch_release(scan, priorbatch);

	if (!BlockNumberIsValid(blkno))
		return NULL;

	/* Allocate space for next batch */
	batch = indexam_util_batch_alloc(scan);

	/* Get the buffer for next batch */
	if (ScanDirectionIsForward(dir))
		buf = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
	else
	{
		buf = _hash_getbuf(rel, blkno, HASH_READ,
						   LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);

		/*
		 * We always maintain the pin on bucket page for whole scan operation,
		 * so releasing the additional pin we have acquired here.
		 */
		if (buf == so->hashso_bucket_buf ||
			buf == so->hashso_split_bucket_buf)
			_hash_dropbuf(rel, buf);
	}

	/* Read the next page and load items into allocated batch */
	if (!_hash_readpage(scan, buf, dir, batch))
	{
		indexam_util_batch_release(scan, batch);
		return NULL;
	}

	/* Return the batch containing matched items from next page */
	return batch;
}

/*
 * Advance to next page in a bucket, if any.  If we are scanning the bucket
 * being populated during split operation then this function advances to the
 * bucket being split after the last bucket page of bucket being populated.
 */
static void
_hash_readnext(IndexScanDesc scan,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	bool		block_found = false;

	blkno = (*opaquep)->hasho_nextblkno;

	/*
	 * Retain the pin on primary bucket page till the end of scan.  Refer the
	 * comments in _hash_first to know the reason of retaining pin.
	 */
	if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
		LockBuffer(*bufp, BUFFER_LOCK_UNLOCK);
	else
		_hash_relbuf(rel, *bufp);

	*bufp = InvalidBuffer;
	/* check for interrupts while we're not holding any buffer lock */
	CHECK_FOR_INTERRUPTS();
	if (BlockNumberIsValid(blkno))
	{
		*bufp = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
		block_found = true;
	}
	else if (so->hashso_buc_populated && !so->hashso_buc_split)
	{
		/*
		 * end of bucket, scan bucket being split if there was a split in
		 * progress at the start of scan.
		 */
		*bufp = so->hashso_split_bucket_buf;

		/*
		 * buffer for bucket being split must be valid as we acquire the pin
		 * on it before the start of scan and retain it till end of scan.
		 */
		Assert(BufferIsValid(*bufp));

		LockBuffer(*bufp, BUFFER_LOCK_SHARE);
		PredicateLockPage(rel, BufferGetBlockNumber(*bufp), scan->xs_snapshot);

		/*
		 * setting hashso_buc_split to true indicates that we are scanning
		 * bucket being split.
		 */
		so->hashso_buc_split = true;

		block_found = true;
	}

	if (block_found)
	{
		*pagep = BufferGetPage(*bufp);
		*opaquep = HashPageGetOpaque(*pagep);
	}
}

/*
 * Advance to previous page in a bucket, if any.  If the current scan has
 * started during split operation then this function advances to bucket
 * being populated after the first bucket page of bucket being split.
 */
static void
_hash_readprev(IndexScanDesc scan,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	bool		haveprevblk;

	blkno = (*opaquep)->hasho_prevblkno;

	/*
	 * Retain the pin on primary bucket page till the end of scan.  Refer the
	 * comments in _hash_first to know the reason of retaining pin.
	 */
	if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
	{
		LockBuffer(*bufp, BUFFER_LOCK_UNLOCK);
		haveprevblk = false;
	}
	else
	{
		_hash_relbuf(rel, *bufp);
		haveprevblk = true;
	}

	*bufp = InvalidBuffer;
	/* check for interrupts while we're not holding any buffer lock */
	CHECK_FOR_INTERRUPTS();

	if (haveprevblk)
	{
		Assert(BlockNumberIsValid(blkno));
		*bufp = _hash_getbuf(rel, blkno, HASH_READ,
							 LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
		*pagep = BufferGetPage(*bufp);
		*opaquep = HashPageGetOpaque(*pagep);

		/*
		 * We always maintain the pin on bucket page for whole scan operation,
		 * so releasing the additional pin we have acquired here.
		 */
		if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
			_hash_dropbuf(rel, *bufp);
	}
	else if (so->hashso_buc_populated && so->hashso_buc_split)
	{
		/*
		 * end of bucket, scan bucket being populated if there was a split in
		 * progress at the start of scan.
		 */
		*bufp = so->hashso_bucket_buf;

		/*
		 * buffer for bucket being populated must be valid as we acquire the
		 * pin on it before the start of scan and retain it till end of scan.
		 */
		Assert(BufferIsValid(*bufp));

		LockBuffer(*bufp, BUFFER_LOCK_SHARE);
		*pagep = BufferGetPage(*bufp);
		*opaquep = HashPageGetOpaque(*pagep);

		/* move to the end of bucket chain */
		while (BlockNumberIsValid((*opaquep)->hasho_nextblkno))
			_hash_readnext(scan, bufp, pagep, opaquep);

		/*
		 * setting hashso_buc_split to false indicates that we are scanning
		 * bucket being populated.
		 */
		so->hashso_buc_split = false;
	}
}

/*
 *	_hash_first() -- Find the first batch of items in a scan.
 *
 *		We find the first batch of items (or, if backward scan, the last
 *		batch) in the index that satisfies the qualification associated with
 *		the scan descriptor.
 *
 *		On successful exit, returns a batch containing matching items.
 *		Otherwise returns NULL, indicating that there are no further matches.
 *		No locks are ever held when we return.
 *
 *		We always retain our own pin on the bucket page.  When we return a
 *		batch with a bucket page, it will retain its own reference pin.
 */
IndexScanBatch
_hash_first(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	ScanKey		cur;
	uint32		hashkey;
	Bucket		bucket;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	IndexScanBatch batch;

	pgstat_count_index_scan(rel);
	if (scan->instrument)
		scan->instrument->nsearches++;

	/*
	 * We do not support hash scans with no index qualification, because we
	 * would have to read the whole index rather than just one bucket. That
	 * creates a whole raft of problems, since we haven't got a practical way
	 * to lock all the buckets against splits or compactions.
	 */
	if (scan->numberOfKeys < 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("hash indexes do not support whole-index scans")));

	/* There may be more than one index qual, but we hash only the first */
	cur = &scan->keyData[0];

	/* We support only single-column hash indexes */
	Assert(cur->sk_attno == 1);
	/* And there's only one operator strategy, too */
	Assert(cur->sk_strategy == HTEqualStrategyNumber);

	/*
	 * If the constant in the index qual is NULL, assume it cannot match any
	 * items in the index.
	 */
	if (cur->sk_flags & SK_ISNULL)
		return NULL;

	/*
	 * Okay to compute the hash key.  We want to do this before acquiring any
	 * locks, in case a user-defined hash function happens to be slow.
	 *
	 * If scankey operator is not a cross-type comparison, we can use the
	 * cached hash function; otherwise gotta look it up in the catalogs.
	 *
	 * We support the convention that sk_subtype == InvalidOid means the
	 * opclass input type; this is a hack to simplify life for ScanKeyInit().
	 */
	if (cur->sk_subtype == rel->rd_opcintype[0] ||
		cur->sk_subtype == InvalidOid)
		hashkey = _hash_datum2hashkey(rel, cur->sk_argument);
	else
		hashkey = _hash_datum2hashkey_type(rel, cur->sk_argument,
										   cur->sk_subtype);

	so->hashso_sk_hash = hashkey;

	buf = _hash_getbucketbuf_from_hashkey(rel, hashkey, HASH_READ, NULL);
	PredicateLockPage(rel, BufferGetBlockNumber(buf), scan->xs_snapshot);
	page = BufferGetPage(buf);
	opaque = HashPageGetOpaque(page);
	bucket = opaque->hasho_bucket;

	so->hashso_bucket_buf = buf;

	/*
	 * If a bucket split is in progress, then while scanning the bucket being
	 * populated, we need to skip tuples that were copied from bucket being
	 * split.  We also need to maintain a pin on the bucket being split to
	 * ensure that split-cleanup work done by vacuum doesn't remove tuples
	 * from it till this scan is done.  We need to maintain a pin on the
	 * bucket being populated to ensure that vacuum doesn't squeeze that
	 * bucket till this scan is complete; otherwise, the ordering of tuples
	 * can't be maintained during forward and backward scans.  Here, we have
	 * to be cautious about locking order: first, acquire the lock on bucket
	 * being split; then, release the lock on it but not the pin; then,
	 * acquire a lock on bucket being populated and again re-verify whether
	 * the bucket split is still in progress.  Acquiring the lock on bucket
	 * being split first ensures that the vacuum waits for this scan to
	 * finish.
	 */
	if (H_BUCKET_BEING_POPULATED(opaque))
	{
		BlockNumber old_blkno;
		Buffer		old_buf;

		old_blkno = _hash_get_oldblock_from_newbucket(rel, bucket);

		/*
		 * release the lock on new bucket and re-acquire it after acquiring
		 * the lock on old bucket.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		old_buf = _hash_getbuf(rel, old_blkno, HASH_READ, LH_BUCKET_PAGE);

		/*
		 * remember the split bucket buffer so as to use it later for
		 * scanning.
		 */
		so->hashso_split_bucket_buf = old_buf;
		LockBuffer(old_buf, BUFFER_LOCK_UNLOCK);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		opaque = HashPageGetOpaque(page);
		Assert(opaque->hasho_bucket == bucket);

		if (H_BUCKET_BEING_POPULATED(opaque))
			so->hashso_buc_populated = true;
		else
		{
			_hash_dropbuf(rel, so->hashso_split_bucket_buf);
			so->hashso_split_bucket_buf = InvalidBuffer;
		}
	}

	/* If a backwards scan is requested, move to the end of the chain */
	if (ScanDirectionIsBackward(dir))
	{
		/*
		 * Backward scans that start during split needs to start from end of
		 * bucket being split.
		 */
		while (BlockNumberIsValid(opaque->hasho_nextblkno) ||
			   (so->hashso_buc_populated && !so->hashso_buc_split))
			_hash_readnext(scan, &buf, &page, &opaque);
	}

	/* Allocate space for first batch */
	batch = indexam_util_batch_alloc(scan);

	/* Read the first page and load items into allocated batch */
	if (!_hash_readpage(scan, buf, dir, batch))
	{
		indexam_util_batch_release(scan, batch);
		return NULL;
	}

	/* Return the batch containing matched items */
	return batch;
}

/*
 *	_hash_readpage() -- Load data from current index page into batch
 *
 *	We scan all the items in the current index page and save them into
 *	the batch if they satisfy the qualification. If no matching items
 *	are found in the current page, we move to the next or previous page
 *	in a bucket chain as indicated by the direction.
 *
 *	Return true if any matching items are found else return false.
 */
static bool
_hash_readpage(IndexScanDesc scan, Buffer buf, ScanDirection dir,
			   IndexScanBatch batch)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	HashBatchData *hashbatch = HashBatchGetData(batch);
	Page		page;
	HashPageOpaque opaque;
	OffsetNumber offnum;
	uint16		itemIndex;

	Assert(BufferIsValid(buf));
	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);
	opaque = HashPageGetOpaque(page);

	hashbatch->buf = buf;
	hashbatch->currPage = BufferGetBlockNumber(buf);
	batch->dir = dir;

	if (ScanDirectionIsForward(dir))
	{
		for (;;)
		{
			/* new page, locate starting position by binary search */
			offnum = _hash_binsearch(page, so->hashso_sk_hash);

			itemIndex = _hash_load_qualified_items(scan, page, offnum, dir,
												   batch);

			if (itemIndex != 0)
				break;

			/*
			 * Could not find any matching tuples in the current page, try to
			 * move to the next page
			 */
			_hash_readnext(scan, &buf, &page, &opaque);
			if (!BufferIsValid(buf))
				return false;

			hashbatch->buf = buf;
			hashbatch->currPage = BufferGetBlockNumber(buf);
		}

		batch->firstItem = 0;
		batch->lastItem = itemIndex - 1;
	}
	else
	{
		for (;;)
		{
			/* new page, locate starting position by binary search */
			offnum = _hash_binsearch_last(page, so->hashso_sk_hash);

			itemIndex = _hash_load_qualified_items(scan, page, offnum, dir,
												   batch);

			if (itemIndex != MaxIndexTuplesPerPage)
				break;

			/*
			 * Could not find any matching tuples in the current page, try to
			 * move to the previous page
			 */
			_hash_readprev(scan, &buf, &page, &opaque);
			if (!BufferIsValid(buf))
				return false;

			hashbatch->buf = buf;
			hashbatch->currPage = BufferGetBlockNumber(buf);
		}

		batch->firstItem = itemIndex;
		batch->lastItem = MaxIndexTuplesPerPage - 1;
	}

	/*
	 * Saved at least one match in batch.items[].  Prepare for hashgetbatch to
	 * return it by initializing remaining uninitialized fields.
	 */
	if (hashbatch->buf == so->hashso_bucket_buf ||
		hashbatch->buf == so->hashso_split_bucket_buf)
	{
		/*
		 * Batch's buffer is either the primary bucket, or a bucket being
		 * populated due to a split.
		 *
		 * Increment local reference count so that batch gets an independent
		 * buffer reference that can be released (by the core code/table AM)
		 * before the hashso_bucket_buf/hashso_split_bucket_buf references are
		 * released.
		 */
		IncrBufferRefCount(hashbatch->buf);

		/* Can only use opaque->hasho_nextblkno */
		hashbatch->prevPage = InvalidBlockNumber;
		hashbatch->nextPage = opaque->hasho_nextblkno;
	}
	else
	{
		/* Can use opaque->hasho_prevblkno and opaque->hasho_nextblkno */
		hashbatch->prevPage = opaque->hasho_prevblkno;
		hashbatch->nextPage = opaque->hasho_nextblkno;
	}

	/* we saved one or more matches in batch.items[] */
	indexam_util_batch_unlock(scan, batch, hashbatch->buf);

	Assert(batch->firstItem <= batch->lastItem);
	return true;
}

/*
 * Load all the qualified items from a current index page
 * into batch. Helper function for _hash_readpage.
 */
static int
_hash_load_qualified_items(IndexScanDesc scan, Page page,
						   OffsetNumber offnum, ScanDirection dir,
						   IndexScanBatch batch)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	IndexTuple	itup;
	int			itemIndex;
	OffsetNumber maxoff;

	maxoff = PageGetMaxOffsetNumber(page);

	if (ScanDirectionIsForward(dir))
	{
		/* load items[] in ascending order */
		itemIndex = 0;

		while (offnum <= maxoff)
		{
			Assert(offnum >= FirstOffsetNumber);
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

			/*
			 * skip the tuples that are moved by split operation for the scan
			 * that has started when split was in progress. Also, skip the
			 * tuples that are marked as dead.
			 */
			if ((so->hashso_buc_populated && !so->hashso_buc_split &&
				 (itup->t_info & INDEX_MOVED_BY_SPLIT_MASK)) ||
				(scan->ignore_killed_tuples &&
				 (ItemIdIsDead(PageGetItemId(page, offnum)))))
			{
				offnum = OffsetNumberNext(offnum);	/* move forward */
				continue;
			}

			if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup) &&
				_hash_checkqual(scan, itup))
			{
				/* tuple is qualified, so remember it */
				_hash_saveitem(batch, itemIndex, offnum, itup);
				itemIndex++;
			}
			else
			{
				/*
				 * No more matching tuples exist in this page. so, exit while
				 * loop.
				 */
				break;
			}

			offnum = OffsetNumberNext(offnum);
		}

		Assert(itemIndex <= MaxIndexTuplesPerPage);
		return itemIndex;
	}
	else
	{
		/* load items[] in descending order */
		itemIndex = MaxIndexTuplesPerPage;

		while (offnum >= FirstOffsetNumber)
		{
			Assert(offnum <= maxoff);
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

			/*
			 * skip the tuples that are moved by split operation for the scan
			 * that has started when split was in progress. Also, skip the
			 * tuples that are marked as dead.
			 */
			if ((so->hashso_buc_populated && !so->hashso_buc_split &&
				 (itup->t_info & INDEX_MOVED_BY_SPLIT_MASK)) ||
				(scan->ignore_killed_tuples &&
				 (ItemIdIsDead(PageGetItemId(page, offnum)))))
			{
				offnum = OffsetNumberPrev(offnum);	/* move back */
				continue;
			}

			if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup) &&
				_hash_checkqual(scan, itup))
			{
				itemIndex--;
				/* tuple is qualified, so remember it */
				_hash_saveitem(batch, itemIndex, offnum, itup);
			}
			else
			{
				/*
				 * No more matching tuples exist in this page. so, exit while
				 * loop.
				 */
				break;
			}

			offnum = OffsetNumberPrev(offnum);
		}

		Assert(itemIndex >= 0);
		return itemIndex;
	}
}

/* Save an index item into batch->items[itemIndex] */
static inline void
_hash_saveitem(IndexScanBatch batch, int itemIndex,
			   OffsetNumber offnum, IndexTuple itup)
{
	BatchMatchingItem *currItem = &batch->items[itemIndex];

	currItem->tableTid = itup->t_tid;
	currItem->indexOffset = offnum;
	currItem->tupleOffset = 0;
}
