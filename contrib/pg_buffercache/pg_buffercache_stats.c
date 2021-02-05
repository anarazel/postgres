/*-------------------------------------------------------------------------
 *
 * pg_buffercache_pages.c
 *	  display some contents of the buffer cache
 *
 *	  contrib/pg_buffercache/pg_buffercache_pages.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"


PG_FUNCTION_INFO_V1(pg_buffercache_stats);

Datum
pg_buffercache_stats(PG_FUNCTION_ARGS)
{
	uint64		valid_count = 0;
	uint64		dirty_count = 0;
	uint64		unused_count = 0;
	uint64		partially_valid_count = 0;
	uint64		locked_count = 0;
	uint64		io_in_progress_count = 0;
#define NUM_BUFFERCACHE_STATS_COLS	6
	TupleDesc	tupdesc;
	Datum		values[NUM_BUFFERCACHE_STATS_COLS];
	bool		nulls[NUM_BUFFERCACHE_STATS_COLS];

	tupdesc = CreateTemplateTupleDesc(NUM_BUFFERCACHE_STATS_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "valid_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "dirty_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "unused_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "io_in_progress_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "partially_valid_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "header_locked_count", INT8OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);


	/*
	 * Scan through all the buffers, updating the counters. As this is just a
	 * summary, we don't take either partition locks nor buffer header
	 * locks. The overhead of doing either would increase the cost
	 * substantially.
	 */
	for (int i = 0; i < NBuffers; i++)
	{
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state = pg_atomic_read_u32(&bufHdr->state);

		if (buf_state & BM_LOCKED)
			locked_count++;

		if (!(buf_state & BM_TAG_VALID))
		{
			unused_count++;
			continue;
		}

		if (buf_state & BM_IO_IN_PROGRESS)
			io_in_progress_count++;

		if (!(buf_state & BM_VALID))
		{
			partially_valid_count++;
			continue;
		}

		valid_count++;

		if (buf_state & BM_DIRTY)
			dirty_count++;
	}

	MemSet(nulls, 0, sizeof(nulls));

	values[0] = Int64GetDatum(valid_count);
	values[1] = Int64GetDatum(dirty_count);
	values[2] = Int64GetDatum(unused_count);
	values[3] = Int64GetDatum(io_in_progress_count);
	values[4] = Int64GetDatum(partially_valid_count);
	values[5] = Int64GetDatum(locked_count);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));

}
