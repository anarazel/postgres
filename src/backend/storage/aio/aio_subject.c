/*-------------------------------------------------------------------------
 *
 * aio_subject.c
 *    IO completion handling for IOs on different subjects
 *
 * XXX Write me
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio_subject.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/memutils.h"


static const PgAioSubjectInfo *aio_subject_info[] = {
	[ASI_INVALID] = &(PgAioSubjectInfo) {
		.name = "invalid",
	},
};

static const PgAioHandleSharedCallbacks *aio_shared_cbs[] = {
};


void
pgaio_io_add_shared_cb(PgAioHandle *ioh, PgAioHandleSharedCallbackID cbid)
{
	if (cbid >= lengthof(aio_shared_cbs))
		elog(ERROR, "callback %d is out of range", cbid);
	if (aio_shared_cbs[cbid]->complete == NULL)
		elog(ERROR, "callback %d is undefined", cbid);
	if (ioh->num_shared_callbacks >= AIO_MAX_SHARED_CALLBACKS)
		elog(PANIC, "too many callbacks, the max is %d", AIO_MAX_SHARED_CALLBACKS);
	ioh->shared_callbacks[ioh->num_shared_callbacks] = cbid;

	elog(DEBUG3, "io:%d, op %s, subject %s, adding cbid num %d, id %d",
		 pgaio_io_get_id(ioh),
		 pgaio_io_get_op_name(ioh),
		 pgaio_io_get_subject_name(ioh),
		 ioh->num_shared_callbacks + 1, cbid);

	ioh->num_shared_callbacks++;
}

const char *
pgaio_io_get_subject_name(PgAioHandle *ioh)
{
	Assert(ioh->subject >= 0 && ioh->subject < ASI_COUNT);

	return aio_subject_info[ioh->subject]->name;
}

void
pgaio_io_prepare_subject(PgAioHandle *ioh)
{
	Assert(ioh->subject > ASI_INVALID && ioh->subject < ASI_COUNT);
	Assert(ioh->op >= 0 && ioh->op < PGAIO_OP_COUNT);

	for (int i = ioh->num_shared_callbacks; i > 0; i--)
	{
		PgAioHandleSharedCallbackID cbid = ioh->shared_callbacks[i - 1];
		const PgAioHandleSharedCallbacks *cbs = aio_shared_cbs[cbid];

		if (!cbs->prepare)
			continue;

		elog(DEBUG3, "io:%d, op %s, subject %s, calling cbid num %d, id %d: prepare",
			 pgaio_io_get_id(ioh),
			 pgaio_io_get_op_name(ioh),
			 pgaio_io_get_subject_name(ioh),
			 i, cbid);
		cbs->prepare(ioh);
	}
}

void
pgaio_io_process_completion_subject(PgAioHandle *ioh)
{
	PgAioResult result;

	Assert(ioh->subject >= 0 && ioh->subject < ASI_COUNT);
	Assert(ioh->op >= 0 && ioh->op < PGAIO_OP_COUNT);

	result.status = ARS_OK;		/* low level IO is always considered OK */
	result.result = ioh->result;
	result.id = 0;				/* FIXME */
	result.error_data = 0;

	for (int i = ioh->num_shared_callbacks; i > 0; i--)
	{
		PgAioHandleSharedCallbackID cbid;

		cbid = ioh->shared_callbacks[i - 1];
		elog(DEBUG3, "io:%d, op %s, subject %s, calling cbid num %d, id %d with distilled result status %d, id %u, error_data: %d, result: %d",
			 pgaio_io_get_id(ioh),
			 pgaio_io_get_op_name(ioh),
			 pgaio_io_get_subject_name(ioh),
			 i, cbid,
			 result.status,
			 result.id,
			 result.error_data,
			 result.result);
		result = aio_shared_cbs[cbid]->complete(ioh, result);
	}

	ioh->distilled_result = result;

	elog(DEBUG3, "io:%d, op %s, subject %s, distilled result status %d, id %u, error_data: %d, result: %d, raw_result %d",
		 pgaio_io_get_id(ioh),
		 pgaio_io_get_op_name(ioh),
		 pgaio_io_get_subject_name(ioh),
		 result.status,
		 result.id,
		 result.error_data,
		 result.result,
		 ioh->result);
}

bool
pgaio_io_can_reopen(PgAioHandle *ioh)
{
	return aio_subject_info[ioh->subject]->reopen != NULL;
}

void
pgaio_io_reopen(PgAioHandle *ioh)
{
	Assert(ioh->subject >= 0 && ioh->subject < ASI_COUNT);
	Assert(ioh->op >= 0 && ioh->op < PGAIO_OP_COUNT);

	aio_subject_info[ioh->subject]->reopen(ioh);
}



/* --------------------------------------------------------------------------------
 * IO Result
 * --------------------------------------------------------------------------------
 */

void
pgaio_result_log(PgAioResult result, const PgAioSubjectData *subject_data, int elevel)
{
	const PgAioHandleSharedCallbacks *scb;

	Assert(result.status != ARS_UNKNOWN);
	Assert(result.status != ARS_OK);

	scb = aio_shared_cbs[result.id];

	if (scb->error == NULL)
		elog(ERROR, "scb id %d does not have error callback", result.id);

	scb->error(result, subject_data, elevel);
}
