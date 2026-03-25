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
 * individual table AMs.
 *
 * This is essentially the table AM specific portion of IndexScanDescData,
 * accessed through its xs_heapfetch field.
 */
typedef struct IndexFetchTableData
{
	/* Table AM per-batch opaque area size (MAXALIGN'd), set by AM */
	uint16		batch_opaque_size;

	/* Per-item trailing data size in each batch */
	uint16		batch_per_item_size;

	/*
	 * Bitmask of ScanOptions affecting the relation. No SO_INTERNAL_FLAGS are
	 * permitted.
	 */
	uint32		flags;
} IndexFetchTableData;

/*
 * Location of a BatchMatchingItem within the scan's ring buffer
 */
typedef struct BatchRingItemPos
{
	/* Position references a valid IndexScanDescData.batchbuf[] entry? */
	bool		valid;

	/* IndexScanDescData.batchbuf[]-wise index to relevant IndexScanBatch */
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
	LocationIndex tupleOffset;	/* index tuple's currTuples offset, if any */
} BatchMatchingItem;

/*
 * Data about one batch of items returned by (and passed to) amgetbatch during
 * index scans.
 *
 * Each batch allocation has the following memory layout:
 *
 *   [table AM opaque area]    <- allocation base, at -(batch_table_offset)
 *   [index AM opaque area]    <- at -(batch_index_opaque_size)
 *   [IndexScanBatchData]      <- batch pointer, returned by amgetbatch
 *   [items[maxitemsbatch]]
 *   [table AM trailing data]  <- per-item area (e.g., for visibility info)
 *   [currTuples workspace]    <- index AM stores index tuples here for
 *                                index-only scans (batch_tuples_workspace)
 *
 * batch_table_offset combines both AM opaque sizes into a single offset from
 * the batch pointer to the true allocation base.  The indexbatch.c utilities
 * pfree a batch by passing pfree a pointer returned by index_scan_batch_base.
 * We rely on the assumption that batches have a fixed layout for the duration
 * of an index scan (batches are cached for reuse to avoid palloc churn).
 *
 * The table AM can overlay a small fixed-size struct at the start of the
 * allocated space, which it accesses using a index_scan_batch_base shim
 * accessor function.  Convention for table AMs is to store a pointer to its
 * per-item area in this fixed-size area (e.g., heapam stores a visInfo
 * pointer here), in addition to other information tracked at the batch level.
 *
 * The index AM opaque area is accessed via a custom accessor that uses a
 * fixed compile-time constant offset for efficiency (a constant that is
 * tracked in the scan descriptor as batch_index_opaque_size).
 */
typedef struct IndexScanBatchData
{
	/* Index page's LSN, optionally used by amkillitemsbatch routines */
	XLogRecPtr	lsn;

	/* scan direction when the index page was read */
	ScanDirection dir;

	/*
	 * knownEndBackward and knownEndForward indicate that this batch is the
	 * last one with matching items in the relevant scan direction.  When
	 * amgetbatch returns NULL for a given direction, the corresponding flag
	 * is set on the priorbatch that was passed to that call.  We cannot know
	 * this when a batch is first returned by amgetbatch; it only becomes
	 * apparent when we try and fail to continue the scan past it.
	 *
	 * This allows table AMs to avoid redundant amgetbatch calls with the same
	 * priorbatch -- the index AM might need to read additional index pages to
	 * determine there are no more matching items beyond caller's priorbatch.
	 * In particular, during prefetching the read stream callback discovers
	 * the end-of-scan via prefetchBatch.  tableam_util_fetch_next_batch()
	 * checks these flags so that the scan side doesn't repeat the same
	 * amgetbatch call when it later reaches that batch as scanBatch.
	 */
	bool		knownEndBackward;
	bool		knownEndForward;

	/*
	 * Batch still holds TID recycling interlock?
	 */
	bool		isGuarded;

	/*
	 * Matching items state for this batch.  Output by index AM for table AM.
	 *
	 * The items array is always ordered in index order (ie, by increasing
	 * indexoffset).  When scanning backwards it is convenient for index AMs
	 * to fill the array back-to-front, starting at the last item slot and
	 * filling downwards.  This is why we need both a first-valid-entry and a
	 * last-valid-entry counter.
	 *
	 * Note: these are signed because it's sometimes convenient to use -1 to
	 * represent an out-of-bounds space just before firstItem (when it's 0).
	 */
	int			firstItem;		/* first valid index in items[] */
	int			lastItem;		/* last valid index in items[] */

	/* info about dead items, if any (palloc'd separately, NULL if unused) */
	int			numDead;		/* number of currently stored items */
	int		   *deadItems;		/* items[]-wise indexes of dead items */

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
 * State used by table AMs to manage an index scan that uses the amgetbatch
 * interface.  Scans use a ring buffer of batches returned by amgetbatch.
 *
 * This data structure provides table AMs with a way to read ahead of the
 * current read position by _multiple_ batches/index pages.  The further out
 * the table AM reads ahead like this, the further it can see into the future.
 * That way the table AM is able to reorder work as aggressively as desired.
 * Index scans sometimes need to readahead by several dozen batches in order
 * to maintain an optimal I/O prefetch distance (for reading table blocks).
 */
typedef struct BatchRingBuffer
{
	/* current positions in IndexScanDescData.batchbuf[] for scan */
	BatchRingItemPos scanPos;	/* scan's read position */
	BatchRingItemPos prefetchPos;	/* prefetching position */
	BatchRingItemPos markPos;	/* mark/restore position */

	/* markPos's batch (not in ring buffer when markBatch != scanBatch) */
	IndexScanBatch markBatch;

	/*
	 * headBatch is an index to the earliest still-valid ring buffer batch
	 * slot in batchbuf[].  The actual array position for its IndexScanBatch
	 * is headBatch & (INDEX_SCAN_MAX_BATCHES - 1), since these indexes use
	 * unsigned wrapping arithmetic.  headBatch must be the scan's current
	 * scanBatch (i.e. the current scanPos batch).
	 */
	uint8		headBatch;

	/*
	 * nextBatch is an index to the next _empty_ ring buffer batch slot in
	 * batchbuf[].  As with headBatch, the actual batchbuf[] array position is
	 * nextBatch & (INDEX_SCAN_MAX_BATCHES - 1).  A new batch can only be
	 * appended to this position/slot when !index_scan_batch_full().
	 *
	 * Note: the scan's most recently appended batch (its tail batch) is
	 * always located at (nextBatch - 1) & (INDEX_SCAN_MAX_BATCHES - 1).
	 */
	uint8		nextBatch;
} BatchRingBuffer;

struct IndexScanInstrumentation;

/*
 * We use the same IndexScanDescData structure for both amgettuple-based
 * and amgetbitmap-based index scans.  Some fields are only relevant in
 * amgettuple-based scans.  Others are only used in amgetbatch-based scans.
 *
 * The ring buffer used by amgetbatch scans is stored here as a fixed array of
 * pointers to batches.  We need a minimum of two ring buffer batches (but use
 * INDEX_SCAN_MAX_BATCHES), since table AMs only remove a batch after they've
 * already called amgetbatch again and appended the returned batch.
 */
#define INDEX_SCAN_CACHE_BATCHES	2
#define INDEX_SCAN_MAX_BATCHES		64

StaticAssertDecl(INDEX_SCAN_MAX_BATCHES <= PG_INT8_MAX + 1,
				 "index_scan_batch_loaded relies on int8 ring buffer arithmetic");
StaticAssertDecl((INDEX_SCAN_MAX_BATCHES & (INDEX_SCAN_MAX_BATCHES - 1)) == 0,
				 "INDEX_SCAN_MAX_BATCHES must be a power of 2");

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

	/* scan's amgetbatch state (only used by amgetbatch/usebatchring scans) */
	BatchRingBuffer batchringbuf;

	/*
	 * Array of pointers to recyclable batches, used by all amgetbatch scans
	 * and by amgetbitmap scans of an index AM that supports amgetbatch
	 */
	IndexScanBatch batchcache[INDEX_SCAN_CACHE_BATCHES];

	/* Array of pointers to batches, referenced within batchringbuf */
	IndexScanBatch batchbuf[INDEX_SCAN_MAX_BATCHES];

	bool		usebatchring;	/* scan uses amgetbatch/batchringbuf? */
	bool		batchImmediateUnguard;	/* eagerly drop TID recycling
										 * interlock? */

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
	bool		xs_heap_continue;	/* T if must keep walking, potential
									 * further results */
	IndexFetchTableData *xs_heapfetch;

	/*
	 * Resolved table_index_getnext_slot callback, which is set by
	 * table_index_fetch_begin at the start of amgetbatch/amgettuple scans
	 */
	bool		(*xs_getnext_slot) (struct IndexScanDescData *scan,
									ScanDirection direction,
									struct TupleTableSlot *slot);

	bool		xs_recheck;		/* T means scan keys must be rechecked */

	/* batch size information, set once by index AM in ambeginscan */
	uint16		maxitemsbatch;	/* size of each batch's items[] array */
	uint16		batch_index_opaque_size;	/* MAXALIGN'd index AM opaque size */
	uint16		batch_tuples_workspace; /* currTuples workspace size */

	/* Computed offset, used to get table AM's opaque area from a batch */
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

	/*
	 * An approximate limit on the amount of work, measured in pages touched,
	 * imposed on the index scan.  The default, 0, means no limit.  Used by
	 * selfuncs.c to bound the cost of get_actual_variable_endpoint().
	 */
	uint8		xs_visited_pages_limit;

	/* parallel index scan information, in shared memory */
	struct ParallelIndexScanDescData *parallel_scan;
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
 * How many batches are currently loaded in the ring buffer?
 */
static inline uint8
index_scan_batch_count(IndexScanDescData *scan)
{
	return (uint8) (scan->batchringbuf.nextBatch -
					scan->batchringbuf.headBatch);
}

/*
 * Do we already have a batch loaded at 'idx' offset in scan's ring buffer?
 *
 * NOTE: a stale batch idx can alias a currently-loaded range due to
 * wraparound, producing a false positive.  False negatives are not possible.
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

	return scan->batchbuf[idx & (INDEX_SCAN_MAX_BATCHES - 1)];
}

/*
 * Append given batch to scan's batch ring buffer.
 */
static inline void
index_scan_batch_append(IndexScanDescData *scan, IndexScanBatch batch)
{
	BatchRingBuffer *ringbuf = &scan->batchringbuf;
	uint8		nextBatch = ringbuf->nextBatch;

	Assert(!index_scan_batch_full(scan));

	scan->batchbuf[nextBatch & (INDEX_SCAN_MAX_BATCHES - 1)] = batch;
	ringbuf->nextBatch++;
}

/*
 * Return the true allocation base of a batch (used to pfree batches)
 */
static inline void *
index_scan_batch_base(IndexScanDescData *scan, IndexScanBatch batch)
{
	return (char *) batch - scan->batch_table_offset;
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
	int8		batchdiff;

	Assert(pos1->valid && pos2->valid);

	batchdiff = (int8) (pos1->batch - pos2->batch);
	if (batchdiff != 0)
	{
		/* Resolve comparison using differing batch offsets */
		return batchdiff;
	}

	/*
	 * Resolve comparison using items[]-wise indexes from caller's positions,
	 * since both positions point to the same ring buffer batch
	 */
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
 * are no more items from the batch remaining in the given scan direction.
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
 * When we're called, this position should point to a batch that caller just
 * finished consuming from.  When we return, this position will point to
 * nextBatch, the next batch from the ring buffer.  We'll have also set the
 * position's item offset to nextBatch's first item in the given direction
 * (which is actually nextBatch's _last_ item when scanning backwards).
 *
 * nextBatch doesn't have to be (and often isn't) the most recently appended
 * batch in the scan's ring buffer.  It is merely the next batch in line to be
 * consumed from the point of view of our caller.
 */
static inline void
index_scan_pos_nextbatch(ScanDirection direction,
						 IndexScanBatch nextBatch, BatchRingItemPos *pos)
{
	Assert(nextBatch->dir == direction);
	Assert(nextBatch->firstItem <= nextBatch->lastItem);

	/* Increment batch (might wrap), or initialize it to zero */
	if (pos->valid)
		pos->batch++;
	else
		pos->batch = 0;

	pos->valid = true;

	if (ScanDirectionIsForward(direction))
		pos->item = nextBatch->firstItem;
	else
		pos->item = nextBatch->lastItem;
}

#endif							/* RELSCAN_H */
