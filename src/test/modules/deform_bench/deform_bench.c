/*-------------------------------------------------------------------------
 *
 * deform_bench.c
 *
 * for benchmarking tuple deformation routines
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <sys/time.h>

#include "access/heapam.h"
#include "access/relscan.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/arrayaccess.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(deform_bench);

Datum
deform_bench(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P(1);
	TableScanDesc scan;
	Relation	rel;
	TupleDesc	tupdesc;
	TupleTableSlot *slot;
	Datum	   *elem_datums = NULL;
	bool	   *elem_nulls = NULL;
	int			elem_count;
	int		   *attnums;
	clock_t		start,
				end;

	rel = relation_open(tableoid, AccessShareLock);

	if (rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only heap AM is supported")));

	tupdesc = RelationGetDescr(rel);
	slot = MakeTupleTableSlot(tupdesc, &TTSOpsBufferHeapTuple);
	slot->tts_flags |= TTS_FLAG_OBEYS_NOT_NUL_CONSTRAINTS;

	scan = table_beginscan_strat(rel, GetActiveSnapshot(), 0, NULL, true, false);

	/*
	 * The array is used to allow callers to define how many atts to deform.
	 * e.g: '{1,10}'::int[] would deform attnum=1, then in a 2nd pass deform
	 * the remainder up to attnum=10.  Passing an element as NULL means all
	 * attnums.  This allows simulation of incremental deformation.  Generally
	 * if you're passing an array with more than 1 element, then the array
	 * should be in ascending order.  Doing something like '{10,1}' would mean
	 * we've already deformed 10 attributes and on the 2nd pass there's
	 * nothing to do since attnum=1 was already deformed in the first pass.
	 *
	 * You'll get an ERROR if you pass a number higher than the number of
	 * attributes in the table.
	 */
	deconstruct_array(array,
					  INT4OID,
					  sizeof(int32),
					  true,
					  'i',
					  &elem_datums,
					  &elem_nulls,
					  &elem_count);

	attnums = palloc_array(int, elem_count);

	for (int i = 0; i < elem_count; i++)
	{
		/* Make a NULL element mean all attributes */
		if (elem_nulls[i])
			attnums[i] = tupdesc->natts;
		else
			attnums[i] = DatumGetInt32(elem_datums[i]);
	}

	start = clock();

	while (heap_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		/* Deform in stages according to the attnums array */
		for (int i = 0; i < elem_count; i++)
			slot_getsomeattrs_int(slot, attnums[i]);
	}

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	relation_close(rel, AccessShareLock);

	end = clock();

	/* Returns the number of milliseconds to run the test */
	PG_RETURN_FLOAT8((double) (end - start) / (CLOCKS_PER_SEC / 1000));
}
