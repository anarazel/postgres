/*-------------------------------------------------------------------------
 *
 * delay_execution.c
 *		Test module to allow delay between parsing and execution of a query.
 *
 * The delay is implemented by taking and immediately releasing a specified
 * advisory lock.  If another process has previously taken that lock, the
 * current process will be blocked until the lock is released; otherwise,
 * there's no effect.  This allows an isolationtester script to reliably
 * test behaviors where some specified action happens in another backend
 * between parsing and execution of any desired query.
 *
 * Copyright (c) 2020-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/delay_execution/delay_execution.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/lwlock.h"
#include "storage/ipc.h"
#include "access/relation.h"
#include "utils/rel.h"
#include "utils/injection_point.h"


PG_MODULE_MAGIC;


typedef struct InjIoErrorState
{
	bool		enabled;
	bool		result_set;
	int			result;
}			InjIoErrorState;

static InjIoErrorState * inj_io_error_state;

/* Shared memory init callbacks */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;


static PgAioHandle *last_handle;
static PgAioBounceBuffer *last_bb;



static void
test_aio_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(sizeof(InjIoErrorState));
}

static void
test_aio_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* Create or attach to the shared memory state */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	inj_io_error_state = ShmemInitStruct("injection_points",
										 sizeof(InjIoErrorState),
										 &found);

	if (!found)
	{
		/*
		 * First time through, so initialize.  This is shared with the dynamic
		 * initialization using a DSM.
		 */
		inj_io_error_state->enabled = false;

#ifdef USE_INJECTION_POINTS
		InjectionPointAttach("AIO_PROCESS_COMPLETION_BEFORE_SHARED",
							 "test_aio",
							 "inj_io_short_read",
							 NULL,
							 0);
		InjectionPointLoad("AIO_PROCESS_COMPLETION_BEFORE_SHARED");
#endif
	}
	else
	{
#ifdef USE_INJECTION_POINTS
		InjectionPointLoad("AIO_PROCESS_COMPLETION_BEFORE_SHARED");
		elog(LOG, "injection point loaded");
#endif
	}

	LWLockRelease(AddinShmemInitLock);
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	/* Shared memory initialization */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = test_aio_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = test_aio_shmem_startup;
}


PG_FUNCTION_INFO_V1(grow_rel);
Datum
grow_rel(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	uint32		nblocks = PG_GETARG_UINT32(1);
	Relation	rel;
#define MAX_BUFFERS_TO_EXTEND_BY 64
	Buffer		victim_buffers[MAX_BUFFERS_TO_EXTEND_BY];

	rel = relation_open(relid, AccessExclusiveLock);

	while (nblocks > 0)
	{
		uint32		extend_by_pages;

		extend_by_pages = Min(nblocks, MAX_BUFFERS_TO_EXTEND_BY);

		ExtendBufferedRelBy(BMR_REL(rel),
							MAIN_FORKNUM,
							NULL,
							0,
							extend_by_pages,
							victim_buffers,
							&extend_by_pages);

		nblocks -= extend_by_pages;

		for (uint32 i = 0; i < extend_by_pages; i++)
		{
			ReleaseBuffer(victim_buffers[i]);
		}
	}

	relation_close(rel, NoLock);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(corrupt_rel_block);
Datum
corrupt_rel_block(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	uint32		block = PG_GETARG_UINT32(1);
	Relation	rel;
	Buffer		buf;
	Page		page;
	PageHeader	ph;

	rel = relation_open(relid, AccessExclusiveLock);

	buf = ReadBuffer(rel, block);
	page = BufferGetPage(buf);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	MarkBufferDirty(buf);

	PageInit(page, BufferGetPageSize(buf), 0);

	ph = (PageHeader) page;
	ph->pd_special = BLCKSZ + 1;

	FlushOneBuffer(buf);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	ReleaseBuffer(buf);

	EvictUnpinnedBuffer(buf);

	relation_close(rel, NoLock);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(read_corrupt_rel_block);
Datum
read_corrupt_rel_block(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	uint32		block = PG_GETARG_UINT32(1);
	Relation	rel;
	Buffer		buf;
	BufferDesc *buf_hdr;
	Page		page;
	PgAioHandle *ioh;
	PgAioHandleRef ior;
	SMgrRelation smgr;
	uint32		buf_state;

	rel = relation_open(relid, AccessExclusiveLock);

	/* read buffer without erroring out */
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, block, RBM_ZERO_AND_LOCK, NULL);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	page = BufferGetBlock(buf);

	ioh = pgaio_io_get(CurrentResourceOwner, NULL);
	pgaio_io_get_ref(ioh, &ior);

	buf_hdr = GetBufferDescriptor(buf - 1);
	smgr = RelationGetSmgr(rel);

	/* FIXME: even if just a test, we should verify nobody else uses this */
	buf_state = LockBufHdr(buf_hdr);
	buf_state &= ~(BM_VALID | BM_DIRTY);
	UnlockBufHdr(buf_hdr, buf_state);

	StartBufferIO(buf_hdr, true, false);

	pgaio_io_set_io_data_32(ioh, (uint32 *) &buf, 1);
	pgaio_io_add_shared_cb(ioh, ASC_SHARED_BUFFER_READ);

	smgrstartreadv(ioh, smgr, MAIN_FORKNUM, block,
				   (void *) &page, 1);

	ReleaseBuffer(buf);

	pgaio_io_ref_wait(&ior);

	relation_close(rel, NoLock);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(invalidate_rel_block);
Datum
invalidate_rel_block(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	uint32		block = PG_GETARG_UINT32(1);
	Relation	rel;
	PrefetchBufferResult pr;
	Buffer		buf;

	rel = relation_open(relid, AccessExclusiveLock);

	/* this is a gross hack, but there's no good API exposed */
	pr = PrefetchBuffer(rel, MAIN_FORKNUM, block);
	buf = pr.recent_buffer;
	elog(LOG, "recent: %d", buf);
	if (BufferIsValid(buf))
	{
		/* if the buffer contents aren't valid, this'll return false */
		if (ReadRecentBuffer(rel->rd_locator, MAIN_FORKNUM, block, buf))
		{
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			FlushOneBuffer(buf);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buf);

			if (!EvictUnpinnedBuffer(buf))
				elog(ERROR, "couldn't unpin");
		}
	}

	relation_close(rel, AccessExclusiveLock);

	PG_RETURN_VOID();
}

#if 0
PG_FUNCTION_INFO_V1(test_unsubmitted_vs_close);
Datum
test_unsubmitted_vs_close(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	uint32		block = PG_GETARG_UINT32(1);
	Relation	rel;
	Buffer		buf;
	Page		page;
	PageHeader	ph;

	rel = relation_open(relid, AccessExclusiveLock);

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, block, RBM_ZERO_AND_LOCK, NULL);

	buf = ReadBuffer(rel, block);
	page = BufferGetPage(buf);

	EvictUnpinnedBuffer(buf);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);


	MarkBufferDirty(buf);
	ph->pd_special = BLCKSZ + 1;

	/* last_handle = pgaio_io_get(); */

	PG_RETURN_VOID();
}
#endif

PG_FUNCTION_INFO_V1(handle_get);
Datum
handle_get(PG_FUNCTION_ARGS)
{
	last_handle = pgaio_io_get(CurrentResourceOwner, NULL);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(handle_release_last);
Datum
handle_release_last(PG_FUNCTION_ARGS)
{
	if (!last_handle)
		elog(ERROR, "no handle");

	pgaio_io_release(last_handle);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(handle_get_and_error);
Datum
handle_get_and_error(PG_FUNCTION_ARGS)
{
	pgaio_io_get(CurrentResourceOwner, NULL);

	elog(ERROR, "as you command");
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(handle_get_twice);
Datum
handle_get_twice(PG_FUNCTION_ARGS)
{
	pgaio_io_get(CurrentResourceOwner, NULL);
	pgaio_io_get(CurrentResourceOwner, NULL);

	PG_RETURN_VOID();
}


PG_FUNCTION_INFO_V1(handle_get_release);
Datum
handle_get_release(PG_FUNCTION_ARGS)
{
	PgAioHandle *handle;

	handle = pgaio_io_get(CurrentResourceOwner, NULL);
	pgaio_io_release(handle);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bb_get);
Datum
bb_get(PG_FUNCTION_ARGS)
{
	last_bb = pgaio_bounce_buffer_get();

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bb_release_last);
Datum
bb_release_last(PG_FUNCTION_ARGS)
{
	if (!last_bb)
		elog(ERROR, "no bb");

	pgaio_bounce_buffer_release(last_bb);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bb_get_and_error);
Datum
bb_get_and_error(PG_FUNCTION_ARGS)
{
	pgaio_bounce_buffer_get();

	elog(ERROR, "as you command");
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bb_get_twice);
Datum
bb_get_twice(PG_FUNCTION_ARGS)
{
	pgaio_bounce_buffer_get();
	pgaio_bounce_buffer_get();

	PG_RETURN_VOID();
}


PG_FUNCTION_INFO_V1(bb_get_release);
Datum
bb_get_release(PG_FUNCTION_ARGS)
{
	PgAioBounceBuffer *bb;

	bb = pgaio_bounce_buffer_get();
	pgaio_bounce_buffer_release(bb);

	PG_RETURN_VOID();
}

#ifdef USE_INJECTION_POINTS
extern PGDLLEXPORT void inj_io_short_read(const char *name, const void *private_data);

void
inj_io_short_read(const char *name, const void *private_data)
{
	PgAioHandle *ioh;

	elog(LOG, "short read called: %d", inj_io_error_state->enabled);

	if (inj_io_error_state->enabled)
	{
		ioh = pgaio_inj_io_get();

		if (inj_io_error_state->result_set)
		{
			elog(LOG, "short read, changing result from %d to %d",
				 ioh->result, inj_io_error_state->result);

			ioh->result = inj_io_error_state->result;
		}
	}
}
#endif

PG_FUNCTION_INFO_V1(inj_io_short_read_attach);
Datum
inj_io_short_read_attach(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	inj_io_error_state->enabled = true;
	inj_io_error_state->result_set = !PG_ARGISNULL(0);
	if (inj_io_error_state->result_set)
		inj_io_error_state->result = PG_GETARG_INT32(0);
#else
	elog(ERROR, "injection points not supported");
#endif

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(inj_io_short_read_detach);
Datum
inj_io_short_read_detach(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	inj_io_error_state->enabled = false;
#else
	elog(ERROR, "injection points not supported");
#endif
	PG_RETURN_VOID();
}
