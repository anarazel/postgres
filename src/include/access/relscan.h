/*-------------------------------------------------------------------------
 *
 * relscan.h
 *	  POSTGRES relation scan descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/relscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELSCAN_H
#define RELSCAN_H

#include "access/htup_details.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "nodes/tidbitmap.h"
#include "port/atomics.h"
#include "storage/relfilelocator.h"
#include "storage/spin.h"
#include "utils/relcache.h"


struct ParallelTableScanDescData;
struct TupleTableSlot;

/*
 * Generic descriptor for table scans. This is the base-class for table scans,
 * which needs to be embedded in the scans of individual AMs.
 */
typedef struct TableScanDescData
{
	/* scan parameters */
	Relation	rs_rd;			/* heap relation descriptor */
	struct SnapshotData *rs_snapshot;	/* snapshot to see */
	int			rs_nkeys;		/* number of scan keys */
	struct ScanKeyData *rs_key; /* array of scan key descriptors */

	/*
	 * Scan type-specific members
	 */
	union
	{
		/* Iterator for Bitmap Table Scans */
		TBMIterator rs_tbmiterator;

		/*
		 * Range of ItemPointers for table_scan_getnextslot_tidrange() to
		 * scan.
		 */
		struct
		{
			ItemPointerData rs_mintid;
			ItemPointerData rs_maxtid;
		}			tidrange;
	}			st;

	/*
	 * Information about type and behaviour of the scan, a bitmask of members
	 * of the ScanOptions enum (see tableam.h).
	 */
	uint32		rs_flags;

	struct ParallelTableScanDescData *rs_parallel;	/* parallel scan
													 * information */
} TableScanDescData;
typedef struct TableScanDescData *TableScanDesc;

/*
 * Shared state for parallel table scan.
 *
 * Each backend participating in a parallel table scan has its own
 * TableScanDesc in backend-private memory, and those objects all contain a
 * pointer to this structure.  The information here must be sufficient to
 * properly initialize each new TableScanDesc as workers join the scan, and it
 * must act as a information what to scan for those workers.
 */
typedef struct ParallelTableScanDescData
{
	RelFileLocator phs_locator; /* physical relation to scan */
	bool		phs_syncscan;	/* report location to syncscan logic? */
	bool		phs_snapshot_any;	/* SnapshotAny, not phs_snapshot_data? */
	Size		phs_snapshot_off;	/* data for snapshot */
} ParallelTableScanDescData;
typedef struct ParallelTableScanDescData *ParallelTableScanDesc;

/*
 * Shared state for parallel table scans, for block oriented storage.
 */
typedef struct ParallelBlockTableScanDescData
{
	ParallelTableScanDescData base;

	BlockNumber phs_nblocks;	/* # blocks in relation at start of scan */
	slock_t		phs_mutex;		/* mutual exclusion for setting startblock */
	BlockNumber phs_startblock; /* starting block number */
	BlockNumber phs_numblock;	/* # blocks to scan, or InvalidBlockNumber if
								 * no limit */
	pg_atomic_uint64 phs_nallocated;	/* number of blocks allocated to
										 * workers so far. */
}			ParallelBlockTableScanDescData;
typedef struct ParallelBlockTableScanDescData *ParallelBlockTableScanDesc;

/*
 * Per backend state for parallel table scan, for block-oriented storage.
 */
typedef struct ParallelBlockTableScanWorkerData
{
	uint64		phsw_nallocated;	/* Current # of blocks into the scan */
	uint32		phsw_chunk_remaining;	/* # blocks left in this chunk */
	uint32		phsw_chunk_size;	/* The number of blocks to allocate in
									 * each I/O chunk for the scan */
} ParallelBlockTableScanWorkerData;
typedef struct ParallelBlockTableScanWorkerData *ParallelBlockTableScanWorker;

/*
 * Base class for fetches from a table via an index. This is the base-class
 * for such scans, which needs to be embedded in the respective struct for
 * individual AMs.
 */
typedef struct IndexFetchTableData
{
	Relation	rel;

	/* Table AM per-batch opaque area size (MAXALIGN'd), set by AM */
	uint16		batch_opaque_size;

	/* Per-item trailing data size in each batch */
	uint16		batch_per_item_size;
} IndexFetchTableData;

/*
 * Location of a BatchMatchingItem that appears in a IndexScanBatch returned
 * by (and subsequently passed to) an amgetbatch routine
 */
typedef struct BatchRingItemPos
{
	/* Position references a valid BatchRingBuffer.batches[] entry? */
	bool		valid;

	/* BatchRingBuffer.batches[]-wise index to relevant IndexScanBatch */
	uint8		batch;

	/* IndexScanBatch.items[]-wise index to relevant BatchMatchingItem */
	int			item;

} BatchRingItemPos;

/*
 * Matching item returned by amgetbatch (in returned IndexScanBatch) during an
 * index scan.  Used by table AM to locate relevant matching table tuple.
 */
typedef struct BatchMatchingItem
{
	ItemPointerData tableTid;	/* TID of referenced table item */
	OffsetNumber indexOffset;	/* index item's location within page */
	LocationIndex tupleOffset;	/* IndexTuple's offset in workspace, if any */
} BatchMatchingItem;

/*
 * Data about one batch of items returned by (and passed to) amgetbatch during
 * index scans.
 *
 * Each batch allocation has the following memory layout:
 *
 *   [table AM opaque area]    <- at -(batch_table_offset) from batch ptr
 *   [index AM opaque area]    <- at -(batch_index_opaque_size) from batch ptr
 *   [IndexScanBatchData]      <- the returned pointer
 *   [items[maxitemsbatch]]
 *   [table AM trailing data]  <- e.g. per-item visibility flags
 *   [currTuples workspace]    <- sized by index AM (batch_tuples_workspace)
 *
 * The AM-specific opaque areas are accessed via accessor functions defined by
 * each table AM and index AM that supports the batch interfaces.
 */
typedef struct IndexScanBatchData
{
	XLogRecPtr	lsn;			/* index page's LSN */

	/* scan direction when the index page was read */
	ScanDirection dir;

	/*
	 * knownEndBackward and knownEndForward are set by the table AM to
	 * indicate that this batch is the last one with matching items in the
	 * relevant scan direction.  When amgetbatch returns NULL for a given
	 * direction, the table AM sets the corresponding flag on the priorbatch
	 * that was passed to that call.  We cannot know this when a batch is
	 * first returned by amgetbatch; it only becomes apparent when we try and
	 * fail to continue the scan past it.
	 *
	 * This allows table AMs to avoid redundant amgetbatch calls with the same
	 * priorbatch -- the index AM might need to read additional index pages to
	 * determine there are no more matching items beyond caller's priorbatch.
	 * In particular, during prefetching the read stream callback discovers
	 * the end-of-scan via prefetchBatch.  The table AM checks these flags so
	 * that the scan side doesn't repeat the same amgetbatch call when it
	 * later reaches that batch as scanBatch.
	 */
	bool		knownEndBackward;
	bool		knownEndForward;

	/*
	 * Matching items state for this batch.  Output by index AM for table AM.
	 *
	 * The items array is always ordered in index order (ie, by increasing
	 * indexoffset).  When scanning backwards it is convenient for index AMs
	 * to fill the array back-to-front, so we start at the last item slot and
	 * fill downwards.  This is why we need both a first-valid-entry and a
	 * last-valid-entry counter.
	 *
	 * Note: these are signed because it's sometimes convenient to use -1 to
	 * represent an out-of-bounds space just before firstItem (when it's 0).
	 */
	int			firstItem;		/* first valid index in items[] */
	int			lastItem;		/* last valid index in items[] */

	/* info about dead items if any (deadItems is NULL if never used) */
	int			numDead;		/* number of currently stored items */
	int		   *deadItems;		/* indexes of dead items */

	/*
	 * If we are doing an index-only scan, this is the tuple storage workspace
	 * for the matching tuples (tuples referenced by items[]).  The workspace
	 * size is determined by the index AM (batch_tuples_workspace).
	 *
	 * currTuples points into the trailing portion of this allocation, past
	 * items[] and any table AM trailing data.  It is NULL for plain index
	 * scans.
	 */
	char	   *currTuples;		/* tuple storage for items[] */
	BatchMatchingItem items[FLEXIBLE_ARRAY_MEMBER]; /* matching items */
} IndexScanBatchData;

typedef struct IndexScanBatchData *IndexScanBatch;

/*
 * Maximum number of batches (leaf pages) we can keep in memory.  We need a
 * minimum of two, since we'll only consider releasing one batch when another
 * is read.
 *
 * The current maximum of 64 batches is somewhat of an arbitrary limit.  Very
 * few scans ever get near to this limit in practice.
 */
#define INDEX_SCAN_MAX_BATCHES		64
#define INDEX_SCAN_CACHE_BATCHES	2

/*
 * State used by table AMs to manage an index scan that uses the amgetbatch
 * interface.  Scans use a ring buffer of batches returned by amgetbatch.
 *
 * Batches are kept in the order that they were returned in by amgetbatch,
 * since that is the same order that table_index_getnext_slot will return
 * matches in.  However, table AMs are free to fetch table tuples in whatever
 * order is most convenient/efficient -- provided that such reordering cannot
 * affect the order that table_index_getnext_slot later returns tuples in.
 *
 * This data structure also provides table AMs with a way to read ahead of the
 * current read position by _multiple_ batches/index pages.  The further out
 * the table AM reads ahead like this, the further it can see into the future.
 * That way the table AM is able to reorder work as aggressively as desired.
 * For example, index scans sometimes need to readahead by as many as a few
 * dozen amgetbatch batches in order to maintain an optimal I/O prefetch
 * distance (distance for reading table blocks/fetching table tuples).
 */
typedef struct BatchRingBuffer
{
	/* current positions in batches[] for scan */
	BatchRingItemPos scanPos;	/* scan's read position */
	BatchRingItemPos markPos;	/* mark/restore position */
	BatchRingItemPos prefetchPos;	/* prefetching position */

	IndexScanBatch markBatch;

	/*
	 * headBatch is an index to the earliest still-valid batch in 'batches'.
	 * In practice this must be the scan's current scanPos batch (scanBatch).
	 */
	uint8		headBatch;

	/*
	 * nextBatch is an index to the next empty batch slot in 'batches'.  This
	 * is only actually usable when the scan is !index_scan_batch_full().
	 */
	uint8		nextBatch;

	/* Array of pointers to cached recyclable batches */
	IndexScanBatch cache[INDEX_SCAN_CACHE_BATCHES];

	/* Array of pointers to ring buffer batches */
	IndexScanBatch batches[INDEX_SCAN_MAX_BATCHES];

} BatchRingBuffer;

struct IndexScanInstrumentation;

/*
 * We use the same IndexScanDescData structure for both amgettuple-based
 * and amgetbitmap-based index scans.  Some fields are only relevant in
 * amgettuple-based scans.
 */
typedef struct IndexScanDescData
{
	/* scan parameters */
	Relation	heapRelation;	/* heap relation descriptor, or NULL */
	Relation	indexRelation;	/* index relation descriptor */
	struct SnapshotData *xs_snapshot;	/* snapshot to see */
	int			numberOfKeys;	/* number of index qualifier conditions */
	int			numberOfOrderBys;	/* number of ordering operators */
	struct ScanKeyData *keyData;	/* array of index qualifier descriptors */
	struct ScanKeyData *orderByData;	/* array of ordering op descriptors */

	/* index access method's private state */
	void	   *opaque;			/* access-method-specific info */

	/* table access method's private amgetbatch state */
	BatchRingBuffer batchringbuf;	/* amgetbatch related state */

	bool		usebatchring;	/* scan uses amgetbatch/batchringbuf? */
	bool		batchImmediateUnguard;	/* drop TID recycling interlock in
										 * indexam_util_batch_unlock? */

	/* Cached batch for amgetbitmap callers (avoids repeated alloc/free) */
	IndexScanBatch xs_bitmap_batch;

	bool		xs_want_itup;	/* caller requests index tuples */
	bool		xs_temp_snap;	/* unregister snapshot at scan end? */

	/* signaling to index AM about killing index tuples */
	bool		kill_prior_tuple;	/* last-returned tuple is dead */
	bool		ignore_killed_tuples;	/* do not return killed entries */
	bool		xactStartedInRecovery;	/* prevents killing/seeing killed
										 * tuples */
	/* xs_snapshot uses an MVCC snapshot? */
	bool		MVCCScan;

	/*
	 * Instrumentation counters maintained by all index AMs during both
	 * amgettuple calls and amgetbitmap calls (unless field remains NULL)
	 */
	struct IndexScanInstrumentation *instrument;

	/*
	 * In an index-only scan, a successful table_index_getnext_slot call must
	 * fill either xs_itup (and xs_itupdesc) or xs_hitup (and xs_hitupdesc) to
	 * provide the data returned by the scan.  It can fill both, in which case
	 * the heap format will be used.
	 */
	IndexTuple	xs_itup;		/* index tuple returned by AM */
	struct TupleDescData *xs_itupdesc;	/* rowtype descriptor of xs_itup */
	HeapTuple	xs_hitup;		/* index data returned by AM, as HeapTuple */
	struct TupleDescData *xs_hitupdesc; /* rowtype descriptor of xs_hitup */

	ItemPointerData xs_heaptid; /* result */
	IndexFetchTableData *xs_heapfetch;

	/* Resolved getnext_slot implementation, set by index_beginscan */
	bool		(*xs_getnext_slot) (struct IndexScanDescData *scan,
									ScanDirection direction,
									struct TupleTableSlot *slot);

	bool		xs_recheck;		/* T means scan keys must be rechecked */
	uint16		maxitemsbatch;	/* set by ambeginscan when amgetbatch used */

	/* Per-batch opaque area sizes, set by index AM in ambeginscan */
	uint16		batch_index_opaque_size;	/* MAXALIGN'd index AM opaque size */
	uint16		batch_tuples_workspace; /* currTuples workspace size */

	/* Computed offset from batch pointer to table AM opaque (includes both) */
	uint16		batch_table_offset;

	/*
	 * When fetching with an ordering operator, the values of the ORDER BY
	 * expressions of the last returned tuple, according to the index.  If
	 * xs_recheckorderby is true, these need to be rechecked just like the
	 * scan keys, and the values returned here are a lower-bound on the actual
	 * values.
	 */
	Datum	   *xs_orderbyvals;
	bool	   *xs_orderbynulls;
	bool		xs_recheckorderby;

	/* parallel index scan information, in shared memory */
	struct ParallelIndexScanDescData *parallel_scan;

	/*
	 * Limit on distinct heap pages visited before giving up (0 = no limit).
	 * Used by selfuncs.c to bound the cost of get_actual_variable_endpoint().
	 */
	uint8		xs_visited_pages_limit;

} IndexScanDescData;

/* Generic structure for parallel scans */
typedef struct ParallelIndexScanDescData
{
	RelFileLocator ps_locator;	/* physical table relation to scan */
	RelFileLocator ps_indexlocator; /* physical index relation to scan */
	Size		ps_offset_ins;	/* Offset to SharedIndexScanInstrumentation */
	Size		ps_offset_am;	/* Offset to am-specific structure */
	char		ps_snapshot_data[FLEXIBLE_ARRAY_MEMBER];
}			ParallelIndexScanDescData;

/* Struct for storage-or-index scans of system tables */
typedef struct SysScanDescData
{
	Relation	heap_rel;		/* catalog being scanned */
	Relation	irel;			/* NULL if doing heap scan */
	struct TableScanDescData *scan; /* only valid in storage-scan case */
	struct IndexScanDescData *iscan;	/* only valid in index-scan case */
	struct SnapshotData *snapshot;	/* snapshot to unregister at end of scan */
	struct TupleTableSlot *slot;
} SysScanDescData;

/*
 * Return the true allocation base of a batch (accounting for AM opaque areas
 * stored before the IndexScanBatchData pointer).
 */
static inline void *
batch_alloc_base(IndexScanBatch batch, IndexScanDescData *scan)
{
	return (char *) batch - scan->batch_table_offset;
}

/*
 * Count how many batches are currently loaded in the ring buffer.
 */
static inline uint8
index_scan_batch_count(IndexScanDescData *scan)
{
	return (uint8) (scan->batchringbuf.nextBatch -
					scan->batchringbuf.headBatch);
}

/*
 * Did we already load batch with the requested index?
 */
static inline bool
index_scan_batch_loaded(IndexScanDescData *scan, uint8 idx)
{
	return (int8) (idx - scan->batchringbuf.headBatch) >= 0 &&
		(int8) (idx - scan->batchringbuf.nextBatch) < 0;
}

/*
 * Have we loaded the maximum number of batches?
 */
static inline bool
index_scan_batch_full(IndexScanDescData *scan)
{
	return index_scan_batch_count(scan) == INDEX_SCAN_MAX_BATCHES;
}

/*
 * Return batch for the provided index.
 */
static inline IndexScanBatch
index_scan_batch(IndexScanDescData *scan, uint8 idx)
{
	Assert(index_scan_batch_loaded(scan, idx));

	return scan->batchringbuf.batches[idx & (INDEX_SCAN_MAX_BATCHES - 1)];
}

/*
 * Append given batch to scan's batch ring buffer.
 */
static inline void
index_scan_batch_append(IndexScanDescData *scan, IndexScanBatch batch)
{
	BatchRingBuffer *ringbuf = &scan->batchringbuf;
	uint8		nextBatch = ringbuf->nextBatch;

	ringbuf->batches[nextBatch & (INDEX_SCAN_MAX_BATCHES - 1)] = batch;
	ringbuf->nextBatch++;
}

/*
 * Compare two batch ring positions in the given scan direction.
 *
 * Returns negative if pos1 is behind pos2, 0 if equal, positive if pos1 is
 * ahead of pos2.
 */
static inline int
index_scan_pos_cmp(BatchRingItemPos *pos1, BatchRingItemPos *pos2,
				   ScanDirection direction)
{
	int8		batchdiff = (int8) (pos1->batch - pos2->batch);

	if (batchdiff != 0)
		return batchdiff;

	/* Same batch, compare items */
	if (ScanDirectionIsForward(direction))
		return pos1->item - pos2->item;
	else
		return pos2->item - pos1->item;
}

/*
 * Advance position to its next item in the batch.
 *
 * Advance to the next item within the provided batch (or to the previous item,
 * when scanning backwards).
 *
 * Returns true if the position could be advanced.  Returns false when there
 * are no more items in the batch in the given direction.
 */
static inline bool
index_scan_pos_advance(ScanDirection direction,
					   IndexScanBatch batch, BatchRingItemPos *pos)
{
	Assert(pos->valid);

	if (ScanDirectionIsForward(direction))
	{
		if (++pos->item > batch->lastItem)
			return false;
	}
	else						/* ScanDirectionIsBackward */
	{
		if (--pos->item < batch->firstItem)
			return false;
	}

	/* Advanced within batch */
	return true;
}

/*
 * Advance batch position to the start of its new batch.
 *
 * Sets the given position to the fist item in the given scan direction (or to
 * the last item, when scanning backwards).   Also advances/increments batch
 * offset from position such that it points to newBatchForPos.
 */
static inline void
index_scan_pos_nextbatch(ScanDirection direction,
						 IndexScanBatch newBatch, BatchRingItemPos *pos)
{
	Assert(newBatch->dir == direction);

	/* Increment batch (often wraps uint8 batch field) */
	if (pos->valid)
		pos->batch++;
	else
		pos->batch = 0;

	pos->valid = true;

	if (ScanDirectionIsForward(direction))
		pos->item = newBatch->firstItem;
	else
		pos->item = newBatch->lastItem;
}

#endif							/* RELSCAN_H */
