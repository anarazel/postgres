/*-------------------------------------------------------------------------
 *
 * aio_scb.c
 *	  Asynchronous I/O subsytem - shared callbacks.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/aio_scb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog_internal.h"
#include "lib/stringinfo.h"
#include "storage/aio_internal.h"
#include "storage/buf.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"


/*
 * Implementation of different AIO actions.
 *
 * To support EXEC_BACKEND environments, where we cannot rely on callback
 * addresses being equivalent across processes, PgAioInProgress does not point
 * directly to the type's PgAioActionCBs, but contains an index instead.
 */

/*
 * IO completion callback.
 */
typedef bool (*PgAioCompletedCB)(PgAioInProgress *io);

/*
 * IO retry/reopen callback.
 */
typedef void (*PgAioRetryCB)(PgAioInProgress *io);

/*
 * IO desc callback.
 */
typedef void (*PgAioDescCB)(PgAioInProgress *io, StringInfo s);

typedef struct PgAioActionCBs
{
	PgAioOp op;
	const char *name;
	PgAioRetryCB retry;
	PgAioCompletedCB complete;
	PgAioDescCB desc;
} PgAioActionCBs;


/* IO callback implementation  */
static void pgaio_invalid_desc(PgAioInProgress *io, StringInfo s);

static bool pgaio_nop_complete(PgAioInProgress *io);
static void pgaio_nop_desc(PgAioInProgress *io, StringInfo s);

static bool pgaio_fsync_raw_complete(PgAioInProgress *io);
static void pgaio_fsync_raw_desc(PgAioInProgress *io, StringInfo s);

static bool pgaio_fsync_wal_complete(PgAioInProgress *io);
static void pgaio_fsync_wal_retry(PgAioInProgress *io);
static void pgaio_fsync_wal_desc(PgAioInProgress *io, StringInfo s);

static bool pgaio_flush_range_complete(PgAioInProgress *io);
static void pgaio_flush_range_desc(PgAioInProgress *io, StringInfo s);
static void pgaio_flush_range_smgr_retry(PgAioInProgress *io);

static bool pgaio_read_sb_complete(PgAioInProgress *io);
static void pgaio_read_sb_retry(PgAioInProgress *io);
static void pgaio_read_sb_desc(PgAioInProgress *io, StringInfo s);

static bool pgaio_read_smgr_complete(PgAioInProgress *io);
static void pgaio_read_smgr_retry(PgAioInProgress *io);
static void pgaio_read_smgr_desc(PgAioInProgress *io, StringInfo s);

static bool pgaio_write_sb_complete(PgAioInProgress *io);
static void pgaio_write_sb_retry(PgAioInProgress *io);
static void pgaio_write_sb_desc(PgAioInProgress *io, StringInfo s);

static bool pgaio_write_smgr_complete(PgAioInProgress *io);
static void pgaio_write_smgr_retry(PgAioInProgress *io);
static void pgaio_write_smgr_desc(PgAioInProgress *io, StringInfo s);

static bool pgaio_write_wal_complete(PgAioInProgress *io);
static void pgaio_write_wal_retry(PgAioInProgress *io);
static void pgaio_write_wal_desc(PgAioInProgress *io, StringInfo s);

static bool pgaio_write_raw_complete(PgAioInProgress *io);
static void pgaio_write_raw_desc(PgAioInProgress *io, StringInfo s);


static const PgAioActionCBs io_action_cbs[] =
{
	[PGAIO_SCB_INVALID] =
	{
		.op = PGAIO_OP_INVALID,
		.name = "invalid",
		.desc = pgaio_invalid_desc,
	},

	[PGAIO_SCB_READ_SB] =
	{
		.op = PGAIO_OP_READ,
		.name = "sb",
		.retry = pgaio_read_sb_retry,
		.complete = pgaio_read_sb_complete,
		.desc = pgaio_read_sb_desc,
	},

	[PGAIO_SCB_READ_SMGR] =
	{
		.op = PGAIO_OP_READ,
		.name = "smgr",
		.retry = pgaio_read_smgr_retry,
		.complete = pgaio_read_smgr_complete,
		.desc = pgaio_read_smgr_desc,
	},

	[PGAIO_SCB_WRITE_SB] =
	{
		.op = PGAIO_OP_WRITE,
		.name = "sb",
		.retry = pgaio_write_sb_retry,
		.complete = pgaio_write_sb_complete,
		.desc = pgaio_write_sb_desc,
	},

	[PGAIO_SCB_WRITE_SMGR] =
	{
		.op = PGAIO_OP_WRITE,
		.name = "smgr",
		.retry = pgaio_write_smgr_retry,
		.complete = pgaio_write_smgr_complete,
		.desc = pgaio_write_smgr_desc,
	},

	[PGAIO_SCB_WRITE_WAL] =
	{
		.op = PGAIO_OP_WRITE,
		.name = "wal",
		.retry = pgaio_write_wal_retry,
		.complete = pgaio_write_wal_complete,
		.desc = pgaio_write_wal_desc,
	},

	[PGAIO_SCB_WRITE_RAW] =
	{
		.op = PGAIO_OP_WRITE,
		.name = "raw",
		.complete = pgaio_write_raw_complete,
		.desc = pgaio_write_raw_desc,
	},

	[PGAIO_SCB_FSYNC_RAW] =
	{
		.op = PGAIO_OP_FSYNC,
		.name = "raw",
		.complete = pgaio_fsync_raw_complete,
		.desc = pgaio_fsync_raw_desc,
	},

	[PGAIO_SCB_FSYNC_WAL] =
	{
		.op = PGAIO_OP_FSYNC,
		.name = "wal",
		.retry = pgaio_fsync_wal_retry,
		.complete = pgaio_fsync_wal_complete,
		.desc = pgaio_fsync_wal_desc,
	},

	[PGAIO_SCB_FLUSH_RANGE_RAW] =
	{
		.op = PGAIO_OP_FLUSH_RANGE,
		.name = "raw",
		.complete = pgaio_flush_range_complete,
		.desc = pgaio_flush_range_desc,
	},

	[PGAIO_SCB_FLUSH_RANGE_SMGR] =
	{
		.op = PGAIO_OP_FLUSH_RANGE,
		.name = "smgr",
		.retry = pgaio_flush_range_smgr_retry,
		.complete = pgaio_flush_range_complete,
		.desc = pgaio_flush_range_desc,
	},

	[PGAIO_SCB_NOP] =
	{
		.op = PGAIO_OP_NOP,
		.name = "nop",
		.complete = pgaio_nop_complete,
		.desc = pgaio_nop_desc,
	},
};


bool
pgaio_io_call_shared_complete(PgAioInProgress *io)
{
	Assert(io_action_cbs[io->scb].op == io->op);

#ifdef PGAIO_VERBOSE
	if (message_level_is_interesting(DEBUG3))
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(ErrorContext);
		StringInfoData s;

		initStringInfo(&s);

		pgaio_io_print(io, &s);

		ereport(DEBUG3,
				errmsg("completing %s",
					   s.data),
				errhidestmt(true),
				errhidecontext(true));
		pfree(s.data);
		MemoryContextSwitchTo(oldcontext);
	}
#endif

	return io_action_cbs[io->scb].complete(io);
}

void
pgaio_io_call_shared_retry(PgAioInProgress *io)
{
	PgAioRetryCB retry_cb = NULL;


	retry_cb = io_action_cbs[io->scb].retry;

	if (!retry_cb)
		elog(PANIC, "non-retryable aio being retried");

	retry_cb(io);
}

/*
 * Make sure the fd is valid in this process.
 */
void
pgaio_io_call_shared_open(PgAioInProgress *io)
{
	pgaio_io_call_shared_retry(io);
}

void
pgaio_io_call_shared_desc(PgAioInProgress *io, StringInfo s)
{
	io_action_cbs[io->scb].desc(io, s);
}

/*
 * Can any process perform this IO?
 */
bool
pgaio_io_has_shared_open(PgAioInProgress *io)
{
	return io_action_cbs[io->scb].retry;
}

PgAioOp
pgaio_shared_callback_op(PgAioSharedCallback scb)
{
	return io_action_cbs[scb].op;
}

const char *
pgaio_io_shared_callback_string(PgAioSharedCallback a)
{
	return io_action_cbs[a].name;
}


/* --------------------------------------------------------------------------------
 * IO start routines (see PgAioSharedCallback for a list)
 * --------------------------------------------------------------------------------
 */

void
pgaio_io_start_read_sb(PgAioInProgress *io, struct SMgrRelationData* smgr, ForkNumber forknum,
					   BlockNumber blocknum, char *bufdata, int buffid, int mode)
{
	Assert(BufferIsValid(buffid));

	pgaio_io_prepare(io, PGAIO_OP_READ);

	smgrstartread(io, smgr, forknum, blocknum, bufdata);

	io->scb_data.read_sb.buffid = buffid;
	io->scb_data.read_sb.backend = smgr->smgr_rlocator.backend;
	io->scb_data.read_sb.mode = mode;

	ReadBufferPrepRead(io, buffid);

	pgaio_io_stage(io, PGAIO_SCB_READ_SB);
}

void
pgaio_io_start_read_smgr(PgAioInProgress *io, struct SMgrRelationData* smgr, ForkNumber forknum,
						 BlockNumber blocknum, char *bufdata)
{
	pgaio_io_prepare(io, PGAIO_OP_READ);

	smgrstartread(io, smgr, forknum, blocknum, bufdata);

	io->scb_data.read_smgr.tag = (AioBufferTag){
		.rlocator = smgr->smgr_rlocator,
		.forkNum = forknum,
		.blockNum = blocknum
	};

	pgaio_io_stage(io, PGAIO_SCB_READ_SMGR);
}

void
pgaio_io_start_write_sb(PgAioInProgress *io,
						struct SMgrRelationData* smgr, ForkNumber forknum, BlockNumber blocknum,
						char *bufdata, int buffid, bool skipFsync, bool release_lock)
{
	pgaio_io_prepare(io, PGAIO_OP_WRITE);

	smgrstartwrite(io, smgr, forknum, blocknum, bufdata, skipFsync);

	io->scb_data.write_sb.buffid = buffid;
	io->scb_data.write_sb.backend = smgr->smgr_rlocator.backend;
	io->scb_data.write_sb.release_lock = release_lock;

	ReadBufferPrepWrite(io, buffid, release_lock);

	pgaio_io_stage(io, PGAIO_SCB_WRITE_SB);
}

void
pgaio_io_start_write_smgr(PgAioInProgress *io,
						  struct SMgrRelationData* smgr, ForkNumber forknum, BlockNumber blocknum,
						  char *bufdata, bool skipFsync)
{
	pgaio_io_prepare(io, PGAIO_OP_WRITE);

	smgrstartwrite(io, smgr, forknum, blocknum, bufdata, skipFsync);

	io->scb_data.write_smgr.tag = (AioBufferTag){
		.rlocator = smgr->smgr_rlocator,
		.forkNum = forknum,
		.blockNum = blocknum
	};

	pgaio_io_stage(io, PGAIO_SCB_WRITE_SMGR);
}

void
pgaio_io_start_write_wal(PgAioInProgress *io, int fd, uint32 offset, uint32 nbytes, char *bufdata, uint32 write_no)
{
	pgaio_io_prepare(io, PGAIO_OP_WRITE);

	pgaio_io_prep_write(io, fd, bufdata, offset, nbytes);

	io->scb_data.write_wal.write_no = write_no;

	pgaio_io_stage(io, PGAIO_SCB_WRITE_WAL);
}

void
pgaio_io_start_write_raw(PgAioInProgress *io, int fd, uint64 offset, uint32 nbytes, char *bufdata)
{
	pgaio_io_prepare(io, PGAIO_OP_WRITE);

	pgaio_io_prep_write(io, fd, bufdata, offset, nbytes);

	pgaio_io_stage(io, PGAIO_SCB_WRITE_RAW);
}

void
pgaio_io_start_fsync_raw(PgAioInProgress *io, int fd, bool datasync)
{
	pgaio_io_prepare(io, PGAIO_OP_FSYNC);

	pgaio_io_prep_fsync(io, fd, datasync);

	pgaio_io_stage(io, PGAIO_SCB_FSYNC_RAW);
}

void
pgaio_io_start_fsync_wal(PgAioInProgress *io, int fd, bool datasync_only, uint32 flush_no)
{
	pgaio_io_prepare(io, PGAIO_OP_FSYNC);

	pgaio_io_prep_fsync(io, fd, datasync_only);

	io->scb_data.fsync_wal.flush_no = flush_no;

	pgaio_io_stage(io, PGAIO_SCB_FSYNC_WAL);
}

void
pgaio_io_start_flush_range_raw(PgAioInProgress *io, int fd, uint64 offset, uint32 nbytes)
{
	pgaio_io_prepare(io, PGAIO_OP_FLUSH_RANGE);

	pgaio_io_prep_flush_range(io, fd, offset, nbytes);

	pgaio_io_stage(io, PGAIO_SCB_FLUSH_RANGE_RAW);
}

BlockNumber
pgaio_io_start_flush_range_smgr(PgAioInProgress *io, struct SMgrRelationData* smgr, ForkNumber forknum,
								BlockNumber blocknum, BlockNumber nblocks)
{
	BlockNumber ret;

	pgaio_io_prepare(io, PGAIO_OP_FLUSH_RANGE);

	ret = smgrstartwriteback(io, smgr, forknum, blocknum, nblocks);

	/*
	 * This is allowed to fail, e.g. because the relation has been truncated
	 * since writeback request has been scheduled.
	 */
	if (ret != InvalidBlockNumber)
	{
		io->scb_data.flush_range_smgr.tag = (AioBufferTag){
			.rlocator = smgr->smgr_rlocator,
			.forkNum = forknum,
			.blockNum = blocknum
		};
		pgaio_io_stage(io, PGAIO_SCB_FLUSH_RANGE_SMGR);
	}
	else
	{
		pgaio_io_unprepare(io, PGAIO_OP_FLUSH_RANGE);
	}

	return ret;
}

void
pgaio_io_start_nop(PgAioInProgress *io)
{
	pgaio_io_prepare(io, PGAIO_OP_NOP);

	pgaio_io_prep_nop(io);

	pgaio_io_stage(io, PGAIO_SCB_NOP);
}


/* --------------------------------------------------------------------------------
 * shared IO implementation (see PgAioSharedCallback for a list)
 * --------------------------------------------------------------------------------
 */

static void
pgaio_invalid_desc(PgAioInProgress *io, StringInfo s)
{
}

static bool
pgaio_nop_complete(PgAioInProgress *io)
{
#ifdef PGAIO_VERBOSE
	elog(DEBUG3, "completed nop");
#endif

	return true;
}

static void
pgaio_nop_desc(PgAioInProgress *io, StringInfo s)
{
}

static bool
pgaio_fsync_raw_complete(PgAioInProgress *io)
{
	/* can't retry automatically, don't know which file etc */
	return true;
}

static void
pgaio_fsync_raw_desc(PgAioInProgress *io, StringInfo s)
{
	appendStringInfo(s, "fd: %d, datasync: %d",
					 io->op_data.fsync.fd,
					 io->op_data.fsync.datasync);
}

static void
pgaio_fsync_wal_retry(PgAioInProgress *io)
{
	io->op_data.fsync.fd = XLogFileForFlushNo(io->scb_data.fsync_wal.flush_no);
}

static bool
pgaio_fsync_wal_complete(PgAioInProgress *io)
{
	if (io->flags & PGAIOIP_SOFT_FAILURE)
		return false;

	if (io->result != 0)
		elog(PANIC, "fsync_wal needs better error handling");

	XLogFlushComplete(io, io->scb_data.fsync_wal.flush_no);
	return true;
}

static void
pgaio_fsync_wal_desc(PgAioInProgress *io, StringInfo s)
{
	appendStringInfo(s, "flush_no: %d, fd: %d, datasync: %d",
					 io->scb_data.fsync_wal.flush_no,
					 io->op_data.fsync.fd,
					 io->op_data.fsync.datasync);
}

static bool
pgaio_flush_range_complete(PgAioInProgress *io)
{
	return true;
}

static void
pgaio_flush_range_desc(PgAioInProgress *io, StringInfo s)
{
	appendStringInfo(s, "fd: %d, offset: %llu, nbytes: %llu",
					 io->op_data.flush_range.fd,
					 (unsigned long long) io->op_data.flush_range.offset,
					 (unsigned long long) io->op_data.flush_range.nbytes);
}

static void
pgaio_flush_range_smgr_retry(PgAioInProgress *io)
{
	uint32 off;
	AioBufferTag *tag = &io->scb_data.flush_range_smgr.tag;
	SMgrRelation reln = smgropen(tag->rlocator.locator,
								 tag->rlocator.backend);

	io->op_data.flush_range.fd = smgrfd(reln, tag->forkNum, tag->blockNum, &off);
	Assert(off == io->op_data.flush_range.offset);
}

static void
pgaio_read_sb_retry(PgAioInProgress *io)
{
	BufferDesc *bufHdr = NULL;
	bool		islocal;
	BufferTag	tag;
	Buffer		buffid = io->scb_data.read_sb.buffid;
	SMgrRelation reln;
	uint32		off;

	islocal = BufferIsLocal(buffid);

	if (islocal)
		bufHdr = GetLocalBufferDescriptor(-buffid - 1);
	else
		bufHdr = GetBufferDescriptor(buffid - 1);

	tag = bufHdr->tag;

	reln = smgropen(BufTagGetRelFileLocator(&tag), io->scb_data.read_sb.backend);
	io->op_data.read.fd = smgrfd(reln, BufTagGetForkNum(&tag), tag.blockNum, &off);

	Assert(off == io->op_data.read.offset);
}

static bool
pgaio_read_sb_complete(PgAioInProgress *io)
{
	if (io->flags & PGAIOIP_SOFT_FAILURE)
		return false;

	ReadBufferCompleteRead(io->scb_data.read_sb.buffid,
						   io->op_data.read.bufdata,
						   io->scb_data.read_sb.mode,
						   io->flags & PGAIOIP_HARD_FAILURE);
	return true;
}

static void
pgaio_read_sb_desc(PgAioInProgress *io, StringInfo s)
{
	appendStringInfo(s, "fd: %d, mode: %d, offset: %llu, nbytes: %u, already_done: %u, buffid: %u",
					 io->op_data.read.fd,
					 io->scb_data.read_sb.mode,
					 (long long unsigned) io->op_data.read.offset,
					 io->op_data.read.nbytes,
					 io->op_data.read.already_done,
					 io->scb_data.read_sb.buffid);
}

static void
pgaio_read_smgr_retry(PgAioInProgress *io)
{
	uint32 off;
	AioBufferTag *tag = &io->scb_data.read_smgr.tag;
	SMgrRelation reln = smgropen(tag->rlocator.locator,
								 tag->rlocator.backend);

	io->op_data.read.fd = smgrfd(reln, tag->forkNum, tag->blockNum, &off);
	Assert(off == io->op_data.read.offset);
}

static bool
pgaio_read_smgr_complete(PgAioInProgress *io)
{
	if (io->flags & PGAIOIP_SOFT_FAILURE)
		return false;

	ReadBufferCompleteRawRead(&io->scb_data.read_smgr.tag,
							  io->op_data.read.bufdata,
							  io->flags & PGAIOIP_HARD_FAILURE);

	return true;
}

static void
pgaio_read_smgr_desc(PgAioInProgress *io, StringInfo s)
{
	/* FIXME: proper path, critical section safe */
	appendStringInfo(s, "fd: %d, offset: %llu, nbytes: %u, already_done: %u, rel: %u, buf: %p",
					 io->op_data.read.fd,
					 (long long unsigned) io->op_data.read.offset,
					 io->op_data.read.nbytes,
					 io->op_data.read.already_done,
					 io->scb_data.read_smgr.tag.rlocator.locator.relNumber,
					 io->op_data.read.bufdata);
}

static void
pgaio_write_sb_retry(PgAioInProgress *io)
{
	BufferDesc *bufHdr = NULL;
	bool		islocal;
	BufferTag	tag;
	Buffer		buffid = io->scb_data.read_sb.buffid;
	SMgrRelation reln;
	uint32		off;

	islocal = BufferIsLocal(buffid);

	if (islocal)
		bufHdr = GetLocalBufferDescriptor(-buffid - 1);
	else
		bufHdr = GetBufferDescriptor(buffid - 1);

	tag = bufHdr->tag;
	reln = smgropen(BufTagGetRelFileLocator(&tag), io->scb_data.read_sb.backend);
	io->op_data.write.fd = smgrfd(reln, tag.forkNum, tag.blockNum, &off);

	Assert(off == io->op_data.write.offset);
}

static bool
pgaio_write_sb_complete(PgAioInProgress *io)
{
	if (io->flags & PGAIOIP_SOFT_FAILURE)
		return false;

	ReadBufferCompleteWrite(io->scb_data.write_sb.buffid,
							io->scb_data.write_sb.release_lock,
							io->flags & PGAIOIP_HARD_FAILURE);
	return true;
}

static void
pgaio_write_sb_desc(PgAioInProgress *io, StringInfo s)
{
	appendStringInfo(s, "fd: %d, offset: %llu, nbytes: %u, already_done: %u, release_lock: %d, buffid: %u",
					 io->op_data.write.fd,
					 (unsigned long long) io->op_data.write.offset,
					 io->op_data.write.nbytes,
					 io->op_data.write.already_done,
					 io->scb_data.write_sb.release_lock,
					 io->scb_data.write_sb.buffid);
}


static void
pgaio_write_smgr_retry(PgAioInProgress *io)
{
	uint32 off;
	AioBufferTag *tag = &io->scb_data.write_smgr.tag;
	SMgrRelation reln = smgropen(tag->rlocator.locator,
								 tag->rlocator.backend);

	io->op_data.read.fd = smgrfd(reln, tag->forkNum, tag->blockNum, &off);
	Assert(off == io->op_data.read.offset);
}

static bool
pgaio_write_smgr_complete(PgAioInProgress *io)
{
	if (io->flags & PGAIOIP_SOFT_FAILURE)
		return false;

	if (!(io->flags & PGAIOIP_HARD_FAILURE))
		Assert(io->op_data.write.already_done == BLCKSZ);

	return true;
}

static void
pgaio_write_smgr_desc(PgAioInProgress *io, StringInfo s)
{
	appendStringInfo(s, "fd: %d, offset: %llu, nbytes: %u, already_done: %u, data: %p",
					 io->op_data.write.fd,
					 (unsigned long long) io->op_data.write.offset,
					 io->op_data.write.nbytes,
					 io->op_data.write.already_done,
					 io->op_data.write.bufdata);
}

static void
pgaio_write_wal_retry(PgAioInProgress *io)
{
	io->op_data.write.fd = XLogFileForWriteNo(io->scb_data.write_wal.write_no);
}

static bool
pgaio_write_wal_complete(PgAioInProgress *io)
{
	if (io->flags & PGAIOIP_SOFT_FAILURE)
		return false;

	if (io->flags & PGAIOIP_HARD_FAILURE)
	{
		if (io->result <= 0)
		{
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to log file: %s",
							strerror(-io->result))));
		}
		else
		{
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to log file: wrote only %d of %d bytes",
							io->result, (io->op_data.write.nbytes - io->op_data.write.already_done))));
		}
	}


	XLogWriteComplete(io, io->scb_data.write_wal.write_no);

	return true;
}

static void
pgaio_write_wal_desc(PgAioInProgress *io, StringInfo s)
{
	appendStringInfo(s, "write_no: %d, fd: %d, offset: %llu, nbytes: %u, already_done: %u, bufdata: %p",
					 io->scb_data.write_wal.write_no,
					 io->op_data.write.fd,
					 (unsigned long long) io->op_data.write.offset,
					 io->op_data.write.nbytes,
					 io->op_data.write.already_done,
					 io->op_data.write.bufdata);
}

static bool
pgaio_write_raw_complete(PgAioInProgress *io)
{
	/* can't retry automatically, don't know which file etc */
	return true;
}

static void
pgaio_write_raw_desc(PgAioInProgress *io, StringInfo s)
{
	appendStringInfo(s, "fd: %d, offset: %llu, nbytes: %u, already_done: %u, bufdata: %p",
					 io->op_data.write.fd,
					 (unsigned long long) io->op_data.write.offset,
					 io->op_data.write.nbytes,
					 io->op_data.write.already_done,
					 io->op_data.write.bufdata);
}
