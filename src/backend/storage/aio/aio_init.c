/*-------------------------------------------------------------------------
 *
 * aio_init.c
 *    Asynchronous I/O subsytem - Initialization
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio_init.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "storage/aio.h"
#include "storage/aio_init.h"
#include "storage/aio_internal.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "storage/shmem.h"


static Size
AioCtlShmemSize(void)
{
	Size		sz;

	/* aio_ctl itself */
	sz = offsetof(PgAioCtl, io_handles);

	return sz;
}

static uint32
AioProcs(void)
{
	return MaxBackends + NUM_AUXILIARY_PROCS;
}

static Size
AioBackendShmemSize(void)
{
	return mul_size(AioProcs(), sizeof(PgAioPerBackend));
}

static Size
AioHandleShmemSize(void)
{
	Size		sz;

	/* ios */
	sz = mul_size(AioProcs(),
				  mul_size(io_max_concurrency, sizeof(PgAioHandle)));

	return sz;
}

static Size
AioIOVShmemSize(void)
{
	/* FIXME: io_combine_limit is USERSET */
	return mul_size(sizeof(struct iovec),
					mul_size(mul_size(io_combine_limit, AioProcs()),
							 io_max_concurrency));
}

static Size
AioIOVDataShmemSize(void)
{
	/* FIXME: io_combine_limit is USERSET */
	return mul_size(sizeof(uint64),
					mul_size(mul_size(io_combine_limit, AioProcs()),
							 io_max_concurrency));
}

static Size
AioBounceBufferDescShmemSize(void)
{
	Size		sz;

	/* PgAioBounceBuffer itself */
	sz = mul_size(sizeof(PgAioBounceBuffer),
				  mul_size(AioProcs(), io_bounce_buffers));

	return sz;
}

static Size
AioBounceBufferDataShmemSize(void)
{
	Size		sz;

	/* and the associated buffer */
	sz = mul_size(BLCKSZ,
				  mul_size(io_bounce_buffers, AioProcs()));
	/* memory for alignment */
	sz += BLCKSZ;

	return sz;
}

/*
 * Choose a suitable value for io_max_concurrency.
 *
 * It's unlikely that we could have more IOs in flight than buffers that we
 * would be allowed to pin.
 *
 * On the upper end, apply a cap too - just because shared_buffers is large,
 * it doesn't make sense have millions of buffers undergo IO concurrently.
 */
static int
AioChooseMaxConccurrency(void)
{
	uint32		max_backends;
	int			max_proportional_pins;

	/* Similar logic to LimitAdditionalPins() */
	max_backends = MaxBackends + NUM_AUXILIARY_PROCS;
	max_proportional_pins = NBuffers / max_backends;

	max_proportional_pins = Max(max_proportional_pins, 1);

	/* apply upper limit */
	return Min(max_proportional_pins, 64);
}

/*
 * Choose a suitable value for io_bounce_buffers.
 *
 * It's very unlikely to be useful to allocate more bounce buffers for each
 * backend than the backend is allowed to pin. Additionally, bounce buffers
 * currently are used for writes, it seems very uncommon for more than 10% of
 * shared_buffers to be written out concurrently.
 *
 * XXX: This quickly can take up significant amounts of memory, the logic
 * should probably fine tuned.
 */
static int
AioChooseBounceBuffers(void)
{
	uint32		max_backends;
	int			max_proportional_pins;

	/* Similar logic to LimitAdditionalPins() */
	max_backends = MaxBackends + NUM_AUXILIARY_PROCS;
	max_proportional_pins = (NBuffers / 10) / max_backends;

	max_proportional_pins = Max(max_proportional_pins, 1);

	/* apply upper limit */
	return Min(max_proportional_pins, 256);
}

Size
AioShmemSize(void)
{
	Size		sz = 0;

	/*
	 * We prefer to report this value's source as PGC_S_DYNAMIC_DEFAULT.
	 * However, if the DBA explicitly set wal_buffers = -1 in the config file,
	 * then PGC_S_DYNAMIC_DEFAULT will fail to override that and we must force
	 *
	 */
	if (io_max_concurrency == -1)
	{
		char		buf[32];

		snprintf(buf, sizeof(buf), "%d", AioChooseMaxConccurrency());
		SetConfigOption("io_max_concurrency", buf, PGC_POSTMASTER,
						PGC_S_DYNAMIC_DEFAULT);
		if (io_bounce_buffers == -1)	/* failed to apply it? */
			SetConfigOption("io_max_concurrency", buf, PGC_POSTMASTER,
							PGC_S_OVERRIDE);
	}


	/*
	 * If io_bounce_buffers is -1, we automatically choose a suitable value.
	 *
	 * See also comment above.
	 */
	if (io_bounce_buffers == -1)
	{
		char		buf[32];

		snprintf(buf, sizeof(buf), "%d", AioChooseBounceBuffers());
		SetConfigOption("io_bounce_buffers", buf, PGC_POSTMASTER,
						PGC_S_DYNAMIC_DEFAULT);
		if (io_bounce_buffers == -1)	/* failed to apply it? */
			SetConfigOption("io_bounce_buffers", buf, PGC_POSTMASTER,
							PGC_S_OVERRIDE);
	}

	sz = add_size(sz, AioCtlShmemSize());
	sz = add_size(sz, AioBackendShmemSize());
	sz = add_size(sz, AioHandleShmemSize());
	sz = add_size(sz, AioIOVShmemSize());
	sz = add_size(sz, AioIOVDataShmemSize());
	sz = add_size(sz, AioBounceBufferDescShmemSize());
	sz = add_size(sz, AioBounceBufferDataShmemSize());

	if (pgaio_impl->shmem_size)
		sz = add_size(sz, pgaio_impl->shmem_size());

	return sz;
}

void
AioShmemInit(void)
{
	bool		found;
	uint32		io_handle_off = 0;
	uint32		iovec_off = 0;
	uint32		bounce_buffers_off = 0;
	uint32		per_backend_iovecs = io_max_concurrency * io_combine_limit;
	uint32		per_backend_bb = io_bounce_buffers;
	char	   *bounce_buffers_data;

	aio_ctl = (PgAioCtl *)
		ShmemInitStruct("AioCtl", AioCtlShmemSize(), &found);

	if (found)
		goto out;

	memset(aio_ctl, 0, AioCtlShmemSize());

	aio_ctl->io_handle_count = AioProcs() * io_max_concurrency;
	aio_ctl->iovec_count = AioProcs() * per_backend_iovecs;
	aio_ctl->bounce_buffers_count = AioProcs() * per_backend_bb;

	aio_ctl->backend_state = (PgAioPerBackend *)
		ShmemInitStruct("AioBackend", AioBackendShmemSize(), &found);

	aio_ctl->io_handles = (PgAioHandle *)
		ShmemInitStruct("AioHandle", AioHandleShmemSize(), &found);

	aio_ctl->iovecs = ShmemInitStruct("AioIOV", AioIOVShmemSize(), &found);
	aio_ctl->iovecs_data = ShmemInitStruct("AioIOVData", AioIOVDataShmemSize(), &found);

	aio_ctl->bounce_buffers = ShmemInitStruct("AioBounceBufferDesc", AioBounceBufferDescShmemSize(), &found);

	bounce_buffers_data = ShmemInitStruct("AioBounceBufferData", AioBounceBufferDataShmemSize(), &found);
	bounce_buffers_data = (char *) TYPEALIGN(BLCKSZ, (uintptr_t) bounce_buffers_data);
	aio_ctl->bounce_buffers_data = bounce_buffers_data;


	/* Initialize IO handles. */
	for (uint64 i = 0; i < aio_ctl->io_handle_count; i++)
	{
		PgAioHandle *ioh = &aio_ctl->io_handles[i];

		ioh->op = PGAIO_OP_INVALID;
		ioh->subject = ASI_INVALID;
		ioh->state = AHS_IDLE;

		slist_init(&ioh->bounce_buffers);
	}

	/* Initialize Bounce Buffers. */
	for (uint64 i = 0; i < aio_ctl->bounce_buffers_count; i++)
	{
		PgAioBounceBuffer *bb = &aio_ctl->bounce_buffers[i];

		bb->buffer = bounce_buffers_data;
		bounce_buffers_data += BLCKSZ;
	}


	for (int procno = 0; procno < AioProcs(); procno++)
	{
		PgAioPerBackend *bs = &aio_ctl->backend_state[procno];

		bs->io_handle_off = io_handle_off;
		io_handle_off += io_max_concurrency;

		bs->bounce_buffers_off = bounce_buffers_off;
		bounce_buffers_off += per_backend_bb;

		dclist_init(&bs->idle_ios);
		memset(bs->staged_ios, 0, sizeof(PgAioHandle *) * PGAIO_SUBMIT_BATCH_SIZE);
		slist_init(&bs->idle_bbs);

		/* initialize per-backend IOs */
		for (int i = 0; i < io_max_concurrency; i++)
		{
			PgAioHandle *ioh = &aio_ctl->io_handles[bs->io_handle_off + i];

			ioh->generation = 1;
			ioh->owner_procno = procno;
			ioh->iovec_off = iovec_off;
			ioh->iovec_data_len = 0;
			ioh->report_return = NULL;
			ioh->resowner = NULL;
			ioh->num_shared_callbacks = 0;
			ioh->distilled_result.status = ARS_UNKNOWN;
			ioh->flags = 0;

			ConditionVariableInit(&ioh->cv);

			dclist_push_tail(&bs->idle_ios, &ioh->node);
			iovec_off += io_combine_limit;
		}

		/* initialize per-backend bounce buffers */
		for (int i = 0; i < per_backend_bb; i++)
		{
			PgAioBounceBuffer *bb = &aio_ctl->bounce_buffers[bs->bounce_buffers_off + i];

			slist_push_head(&bs->idle_bbs, &bb->node);
		}
	}

out:
	/* Initialize IO method specific resources. */
	if (pgaio_impl->shmem_init)
		pgaio_impl->shmem_init(!found);
}

void
pgaio_postmaster_init(void)
{
	if (pgaio_impl->postmaster_init)
		pgaio_impl->postmaster_init();
}

void
pgaio_postmaster_child_init(void)
{
	/* shouldn't be initialized twice */
	Assert(!my_aio);

	if (MyProc == NULL || MyProcNumber >= AioProcs())
		elog(ERROR, "aio requires a normal PGPROC");

	my_aio = &aio_ctl->backend_state[MyProcNumber];

	if (pgaio_impl->postmaster_child_init)
		pgaio_impl->postmaster_child_init();
}

void
pgaio_postmaster_child_init_local(void)
{
	if (pgaio_impl->postmaster_child_init_local)
		pgaio_impl->postmaster_child_init_local();
}
