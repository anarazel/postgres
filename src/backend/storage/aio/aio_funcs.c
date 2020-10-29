/*-------------------------------------------------------------------------
 *
 * aio_funcs.c
 *	  Asynchronous I/O subsytem - SQL callable functions.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/aio_funcs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/aio_internal.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/fmgrprotos.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "storage/proc.h"


Datum
pg_stat_get_aio_backends(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_AIO_BACKEND_COLS	13

	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	for (int i = 0; i < aio_ctl->backend_state_count; i++)
	{
		PgAioPerBackend *bs = &aio_ctl->backend_state[i];
		Datum		values[PG_STAT_GET_AIO_BACKEND_COLS];
		bool		nulls[PG_STAT_GET_AIO_BACKEND_COLS];
		int			pid = ProcGlobal->allProcs[i].pid;

		if (pid == 0)
			continue;

		memset(nulls, 0, sizeof(nulls));

		values[ 0] = Int32GetDatum(pid);
		values[ 1] = Int64GetDatum(bs->executed_total_count);
		values[ 2] = Int64GetDatum(bs->issued_total_count);
		values[ 3] = Int64GetDatum(bs->submissions_total_count);
		values[ 4] = Int64GetDatum(bs->foreign_completed_total_count);
		values[ 5] = Int64GetDatum(bs->retry_total_count);
		values[ 6] = Int64GetDatum(pg_atomic_read_u32(&bs->inflight_count));
		values[ 7] = Int32GetDatum(bs->unused_count);
		values[ 8] = Int32GetDatum(bs->outstanding_count);
		values[ 9] = Int32GetDatum(bs->pending_count);
		values[10] = Int32GetDatum(bs->local_completed_count);
		values[11] = Int32GetDatum(bs->foreign_completed_count);
		values[12] = Int32GetDatum(bs->last_context);


		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

Datum
pg_stat_get_aios(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_AIOS_COLS	10

	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	StringInfoData tmps;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	initStringInfo(&tmps);

	for (int i = 0; i < max_aio_in_progress; i++)
	{
		PgAioInProgress *raw_io = &aio_ctl->in_progress_io[i];
		PgAioInProgress io_copy;
		Datum		values[PG_STAT_GET_AIOS_COLS];
		bool		nulls[PG_STAT_GET_AIOS_COLS];
		uint64 ref_generation;
		PgAioIPFlags flags;

		/* first check if IO is in use */
		flags = raw_io->flags;
		if (flags & PGAIOIP_UNUSED)
			continue;

		/*
		 * Fetch the generation from before we copy the IO to local memory. By
		 * re-checking if still the same generation after the copy, we can
		 * ensure the IO hasn't been recycled while copying.
		 */
		ref_generation = raw_io->generation;
		pg_read_barrier();

		/*
		 * Copy IO to local memory - makes it easier to deal with possibility
		 * of concurrent changes to the IO.
		 */
		io_copy = *raw_io;

		if (pgaio_io_recycled(raw_io, ref_generation, &flags) ||
			flags & PGAIOIP_UNUSED)
			continue;

		memset(nulls, 0, sizeof(nulls));

		values[ 0] = Int32GetDatum(i);
		values[ 1] = Int64GetDatum(io_copy.generation);
		values[ 2] = PointerGetDatum(cstring_to_text(pgaio_io_operation_string(io_copy.op)));
		values[ 3] = PointerGetDatum(cstring_to_text(pgaio_io_shared_callback_string(io_copy.scb)));

		pgaio_io_flag_string(flags, &tmps);
		values[ 4] = PointerGetDatum(cstring_to_text(tmps.data));
		resetStringInfo(&tmps);

		values[ 5] = Int32GetDatum(io_copy.ring);

		if (io_copy.owner_id != INVALID_PGPROCNO)
			values[ 6] = Int32GetDatum(ProcGlobal->allProcs[io_copy.owner_id].pid);
		else
			nulls[ 6] = true;

		if (io_copy.merge_with_idx != PGAIO_MERGE_INVALID)
			values[ 7] = Int32GetDatum(io_copy.merge_with_idx);
		else
			nulls[ 7] = true;

		values[ 8] = Int32GetDatum(io_copy.result);

		pgaio_io_call_shared_desc(&io_copy, &tmps);
		values[ 9] = PointerGetDatum(cstring_to_text(tmps.data));
		resetStringInfo(&tmps);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}
