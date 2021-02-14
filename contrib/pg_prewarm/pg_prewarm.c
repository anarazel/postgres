/*-------------------------------------------------------------------------
 *
 * pg_prewarm.c
 *		  prewarming utilities
 *
 * Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/pg_prewarm/pg_prewarm.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/relation.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/aio.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_prewarm);

typedef enum
{
	PREWARM_PREFETCH,
	PREWARM_READ_AIO,
	PREWARM_READ_NOAIO,
	PREWARM_BUFFER_AIO,
	PREWARM_BUFFER_NOAIO
} PrewarmType;

static PGIOAlignedBlock blockbuffer;

typedef struct prefetch
{
	Relation rel;
	ForkNumber forkNumber;
	int64 curblock;
	int64 lastblock;
	List *bbs;
} prefetch;

static PgStreamingReadNextStatus
prewarm_buffer_next(uintptr_t pgsr_private, PgAioInProgress *aio, uintptr_t *read_private)
{
	prefetch *p = (prefetch *) pgsr_private;
	Buffer buf;
	bool already_valid;
	BlockNumber blockno;

	if (p->curblock <= p->lastblock)
		blockno = p->curblock++;
	else
		return PGSR_NEXT_END;

	buf = ReadBufferAsync(p->rel, p->forkNumber, blockno,
						  RBM_NORMAL, NULL, &already_valid,
						  &aio);

	*read_private = (uintptr_t) buf;

	if (already_valid)
	{
		ereport(DEBUG3,
				errmsg("pgsr %s: found block %d already in buf %d",
					   NameStr(p->rel->rd_rel->relname),
					   blockno, buf),
				errhidestmt(true),
				errhidecontext(true));
		return PGSR_NEXT_NO_IO;
	}
	else
	{
		ereport(DEBUG3,
				errmsg("pgsr %s: fetching block %d into buf %d",
					   NameStr(p->rel->rd_rel->relname),
					   blockno, buf),
				errhidestmt(true),
				errhidecontext(true));
		return PGSR_NEXT_IO;
	}
}

static void
prewarm_buffer_release(uintptr_t pgsr_private, uintptr_t read_private)
{
	prefetch *p = (prefetch *) pgsr_private;
	Buffer buf = (Buffer) read_private;

	ereport(DEBUG2,
			errmsg("pgsr %s: releasing buf %d",
				   NameStr(p->rel->rd_rel->relname),
				   buf),
			errhidestmt(true),
			errhidecontext(true));

	Assert(BufferIsValid(buf));
	ReleaseBuffer(buf);
}


static PgStreamingReadNextStatus
prewarm_smgr_next(uintptr_t pgsr_private, PgAioInProgress *aio, uintptr_t *read_private)
{
	prefetch *p = (prefetch *) pgsr_private;
	BlockNumber blockno;
	PgAioBounceBuffer *bb;

	if (p->curblock <= p->lastblock)
		blockno = p->curblock++;
	else
		return PGSR_NEXT_END;

	if (p->bbs != NIL)
	{
		bb = lfirst(list_tail(p->bbs));
		p->bbs = list_delete_last(p->bbs);
	}
	else
		bb = pgaio_bounce_buffer_get();

	pgaio_assoc_bounce_buffer(aio, bb);

	pgaio_io_start_read_smgr(aio, p->rel->rd_smgr, p->forkNumber, blockno,
							 pgaio_bounce_buffer_buffer(bb));

	*read_private = (uintptr_t) bb;

	return PGSR_NEXT_IO;
}

static void
prewarm_smgr_release(uintptr_t pgsr_private, uintptr_t read_private)
{
}

/*
 * pg_prewarm(regclass, mode text, fork text,
 *			  first_block int8, last_block int8)
 *
 * The first argument is the relation to be prewarmed; the second controls
 * how prewarming is done; legal options are 'prefetch', 'read', and 'buffer'.
 * The third is the name of the relation fork to be prewarmed.  The fourth
 * and fifth arguments specify the first and last block to be prewarmed.
 * If the fourth argument is NULL, it will be taken as 0; if the fifth argument
 * is NULL, it will be taken as the number of blocks in the relation.  The
 * return value is the number of blocks successfully prewarmed.
 */
Datum
pg_prewarm(PG_FUNCTION_ARGS)
{
	Oid			relOid;
	text	   *forkName;
	text	   *type;
	int64		first_block;
	int64		last_block;
	int64		nblocks;
	int64		blocks_done = 0;
	int64		block;
	Relation	rel;
	ForkNumber	forkNumber;
	char	   *forkString;
	char	   *ttype;
	PrewarmType ptype;
	AclResult	aclresult;

	/* Basic sanity checking. */
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be null")));
	relOid = PG_GETARG_OID(0);
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("prewarm type cannot be null")));
	type = PG_GETARG_TEXT_PP(1);
	ttype = text_to_cstring(type);
	if (strcmp(ttype, "prefetch") == 0)
		ptype = PREWARM_PREFETCH;
	else if (strcmp(ttype, "read") == 0)
		ptype = PREWARM_READ_AIO;
	else if (strcmp(ttype, "buffer") == 0)
		ptype = PREWARM_BUFFER_AIO;
	else if (strcmp(ttype, "buffer_noaio") == 0)
		ptype = PREWARM_BUFFER_NOAIO;
	else if (strcmp(ttype, "read_aio") == 0)
		ptype = PREWARM_READ_NOAIO;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid prewarm type"),
				 errhint("Valid prewarm types are \"prefetch\", \"read\", \"read_noaio\", \"buffer\" and , \"buffer_noaio\".")));
		PG_RETURN_INT64(0);		/* Placate compiler. */
	}
	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation fork cannot be null")));
	forkName = PG_GETARG_TEXT_PP(2);
	forkString = text_to_cstring(forkName);
	forkNumber = forkname_to_number(forkString);

	/* Open relation and check privileges. */
	rel = relation_open(relOid, AccessShareLock);
	aclresult = pg_class_aclcheck(relOid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind), get_rel_name(relOid));

	/* Check that the fork exists. */
	if (!smgrexists(RelationGetSmgr(rel), forkNumber))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fork \"%s\" does not exist for this relation",
						forkString)));

	/* Validate block numbers, or handle nulls. */
	nblocks = RelationGetNumberOfBlocksInFork(rel, forkNumber);
	if (PG_ARGISNULL(3))
		first_block = 0;
	else
	{
		first_block = PG_GETARG_INT64(3);
		if (first_block < 0 || first_block >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("starting block number must be between 0 and %lld",
							(long long) (nblocks - 1))));
	}
	if (PG_ARGISNULL(4))
		last_block = nblocks - 1;
	else
	{
		last_block = PG_GETARG_INT64(4);
		if (last_block < 0 || last_block >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ending block number must be between 0 and %lld",
							(long long) (nblocks - 1))));
	}

	/* Now we're ready to do the real work. */
	if (ptype == PREWARM_PREFETCH)
	{
#ifdef USE_PREFETCH

		/*
		 * In prefetch mode, we just hint the OS to read the blocks, but we
		 * don't know whether it really does it, and we don't wait for it to
		 * finish.
		 *
		 * It would probably be better to pass our prefetch requests in chunks
		 * of a megabyte or maybe even a whole segment at a time, but there's
		 * no practical way to do that at present without a gross modularity
		 * violation, so we just do this.
		 */
		for (block = first_block; block <= last_block; ++block)
		{
			CHECK_FOR_INTERRUPTS();
			PrefetchBuffer(rel, forkNumber, block);
			++blocks_done;
		}
#else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prefetch is not supported by this build")));
#endif
	}
	else if (ptype == PREWARM_READ_NOAIO)
	{
		/*
		 * In read mode, we actually read the blocks, but not into shared
		 * buffers.  This is more portable than prefetch mode (it works
		 * everywhere) and is synchronous.
		 */
		for (block = first_block; block <= last_block; ++block)
		{
			CHECK_FOR_INTERRUPTS();
			smgrread(RelationGetSmgr(rel), forkNumber, block, blockbuffer.data);
			++blocks_done;
		}
	}
	else if (ptype == PREWARM_BUFFER_NOAIO)
	{
		/*
		 * In buffer mode, we actually pull the data into shared_buffers.
		 */
		for (block = first_block; block <= last_block; ++block)
		{
			Buffer		buf;

			CHECK_FOR_INTERRUPTS();
			buf = ReadBufferExtended(rel, forkNumber, block, RBM_NORMAL, NULL);
			ReleaseBuffer(buf);
			++blocks_done;
		}
	}
	else if (ptype == PREWARM_BUFFER_AIO)
	{
		PgStreamingRead *pgsr;
		prefetch p;

		p.rel = rel;
		p.forkNumber = forkNumber;
		p.curblock = 0;
		p.lastblock = last_block;
		p.bbs = NIL;

		pgsr = pg_streaming_read_alloc(512, (uintptr_t) &p,
									   prewarm_buffer_next,
									   prewarm_buffer_release);

		for (block = first_block; block <= last_block; ++block)
		{
			Buffer		buf;

			CHECK_FOR_INTERRUPTS();

			buf = (Buffer) pg_streaming_read_get_next(pgsr);
			if (BufferIsValid(buf))
				ReleaseBuffer(buf);
			else
				elog(ERROR, "prefetch ended early");

			++blocks_done;
		}

		if (BufferIsValid(pg_streaming_read_get_next(pgsr)))
			elog(ERROR, "unexpected additional buffer");

		pg_streaming_read_free(pgsr);
	}
	else if (ptype == PREWARM_READ_AIO)
	{
		PgStreamingRead *pgsr;
		prefetch p;
		ListCell *lc;

		p.rel = rel;
		p.forkNumber = forkNumber;
		p.curblock = 0;
		p.lastblock = last_block;
		p.bbs = NIL;

		pgsr = pg_streaming_read_alloc(512, (uintptr_t) &p,
									   prewarm_smgr_next,
									   prewarm_smgr_release);

		for (block = first_block; block <= last_block; ++block)
		{
			PgAioBounceBuffer *bb;

			CHECK_FOR_INTERRUPTS();

			bb = (PgAioBounceBuffer *) pg_streaming_read_get_next(pgsr);
			if (bb == NULL)
				elog(ERROR, "prefetch ended early");

			p.bbs = lappend(p.bbs, (void *) bb);

			++blocks_done;
		}

		if (pg_streaming_read_get_next(pgsr) != 0)
			elog(ERROR, "unexpected additional buffer");

		pg_streaming_read_free(pgsr);

		foreach(lc, p.bbs)
		{
			pgaio_bounce_buffer_release(lfirst(lc));
		}
	}

	/* Close relation, release lock. */
	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(blocks_done);
}
