/*-------------------------------------------------------------------------
 *
 * aio_io.c
 *	  Asynchronous I/O subsytem - implementation of individual IOs.
 *
 * This should contain enum PgAioOp specific handling.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/aio_io.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "storage/aio_internal.h"
#include "storage/fd.h"
#include "storage/shmem.h"
#include "pgstat.h"
#include "port/pg_iovec.h"


/* --------------------------------------------------------------------------------
 * IO initialization routines for the basic IO types (see PgAioOp).
 * --------------------------------------------------------------------------------
 */

void
pgaio_io_prep_read(PgAioInProgress *io, int fd, void *bufdata, uint64 offset, uint32 nbytes)
{
	Assert(io->op == PGAIO_OP_READ);
	Assert(io->flags == PGAIOIP_PREP);
	Assert(ShmemAddrIsValid(bufdata));

	io->op_data.read.fd = fd;
	io->op_data.read.offset = offset;
	io->op_data.read.nbytes = nbytes;
	io->op_data.read.bufdata = bufdata;
	io->op_data.read.already_done = 0;
}

void
pgaio_io_prep_write(PgAioInProgress *io, int fd, const void *bufdata, uint64 offset, uint32 nbytes)
{
	Assert(io->op == PGAIO_OP_WRITE);
	Assert(io->flags == PGAIOIP_PREP);
	Assert(ShmemAddrIsValid(bufdata));

	io->op_data.write.fd = fd;
	io->op_data.write.offset = offset;
	io->op_data.write.nbytes = nbytes;
	io->op_data.write.bufdata = bufdata;
	io->op_data.write.already_done = 0;
}

void
pgaio_io_prep_fsync(PgAioInProgress *io, int fd, bool datasync)
{
	Assert(io->op == PGAIO_OP_FSYNC);
	Assert(io->flags == PGAIOIP_PREP);

	io->op_data.fsync.fd = fd;
	io->op_data.fsync.datasync = datasync;
}

void
pgaio_io_prep_flush_range(PgAioInProgress *io, int fd, uint64 offset, uint32 nbytes)
{
	Assert(io->op == PGAIO_OP_FLUSH_RANGE);
	Assert(io->flags == PGAIOIP_PREP);

	io->op_data.flush_range.fd = fd;
	io->op_data.flush_range.offset = offset;
	io->op_data.flush_range.nbytes = nbytes;
}

void
pgaio_io_prep_nop(PgAioInProgress *io)
{
	Assert(io->op == PGAIO_OP_WRITE);
	Assert(io->flags == PGAIOIP_PREP);
}


/* --------------------------------------------------------------------------------
 * Internal functions dealing with the different types of PgAioOp.
 * --------------------------------------------------------------------------------
 */

static bool
pgaio_can_be_combined(PgAioInProgress *last, PgAioInProgress *cur)
{
	if (last->op != cur->op)
		return false;

	/* could be relaxed, but unlikely to be ever useful? */
	if (last->scb != cur->scb)
		return false;

	if (last->flags & PGAIOIP_RETRY ||
		cur->flags & PGAIOIP_RETRY)
		return false;

	switch (last->op)
	{
		case PGAIO_OP_READ:
			if (last->op_data.read.fd != cur->op_data.read.fd)
				return false;
			if ((last->op_data.read.offset + last->op_data.read.nbytes) != cur->op_data.read.offset)
				return false;
			if (!pgaio_can_scatter_gather() &&
				(((char *) last->op_data.read.bufdata + last->op_data.read.nbytes) != cur->op_data.read.bufdata))
				return false;
			return true;

		case PGAIO_OP_WRITE:
			if (last->op_data.write.fd != cur->op_data.write.fd)
				return false;
			if ((last->op_data.write.offset + last->op_data.write.nbytes) != cur->op_data.write.offset)
				return false;
			if (!pgaio_can_scatter_gather() &&
				(((char *) last->op_data.write.bufdata + last->op_data.write.nbytes) != cur->op_data.write.bufdata))
				return false;
			return true;

		case PGAIO_OP_FSYNC:
		case PGAIO_OP_FLUSH_RANGE:
		case PGAIO_OP_NOP:
			return false;
			break;
		case PGAIO_OP_INVALID:
			elog(ERROR, "unexpected");
			break;
	}

	pg_unreachable();
}

static void
pgaio_io_merge(PgAioInProgress *into, PgAioInProgress *tomerge)
{
	ereport(DEBUG3,
			errmsg("merging %zu to %zu",
				   tomerge - aio_ctl->in_progress_io,
				   into - aio_ctl->in_progress_io),
			errhidestmt(true),
			errhidecontext(true));
	into->merge_with_idx = tomerge - aio_ctl->in_progress_io;
}

void
pgaio_combine_pending(void)
{
	dlist_iter iter;
	PgAioInProgress *last = NULL;
	int combined = 1;

	Assert(my_aio->pending_count > 1);

	dlist_foreach(iter, &my_aio->pending)
	{
		PgAioInProgress *cur = dlist_container(PgAioInProgress, io_node, iter.cur);

		Assert(cur->merge_with_idx == PGAIO_MERGE_INVALID);

		if (last == NULL)
		{
			last = cur;
			continue;
		}

		if (pgaio_can_be_combined(last, cur))
		{
			combined++;

			pgaio_io_merge(last, cur);
		}
		else
		{
			combined = 1;
		}

		if (combined >= PGAIO_MAX_COMBINE)
		{
			ereport(DEBUG3,
					errmsg("max combine at %d", combined),
					errhidestmt(true),
					errhidecontext(true));
			last = NULL;
			combined = 1;
		}
		else
			last = cur;
	}
}

/* helper pgaio_process_io_completion combining the read/write cases */
static void
pgaio_process_io_rw_completion(PgAioInProgress *myio,
							   bool first,
							   uint32 nbytes,
							   uint64 offset,
							   uint32 *already_done,
							   int result,
							   int *running_result_p,
							   int *new_result,
							   PgAioIPFlags *new_flags)
{
	int running_result = *running_result_p;
	int remaining_bytes;

	Assert(first || *already_done == 0);
	Assert(*already_done < nbytes);

	remaining_bytes = nbytes - *already_done;

	if (result <= 0)
	{
		/* merged request failed, report failure on all */
		*new_result = result;

		if (result == -EAGAIN || result == -EINTR)
			*new_flags |= PGAIOIP_SOFT_FAILURE;
		else
			*new_flags |= PGAIOIP_HARD_FAILURE;

		ereport(DEBUG2,
				errcode_for_file_access(),
				errmsg("aio %zd: failed to %s: %s of %u-%u bytes at offset %llu+%u: %s",
					   myio - aio_ctl->in_progress_io,
					   pgaio_io_operation_string(myio->op),
					   pgaio_io_shared_callback_string(myio->scb),
					   nbytes, *already_done,
					   (long long unsigned) offset, *already_done,
					   strerror(-*new_result)));
	}
	else if (running_result == 0)
	{
		Assert(!first);
		*new_result = -EAGAIN;
		*new_flags |= PGAIOIP_SOFT_FAILURE;
	}
	else if (running_result >= remaining_bytes)
	{
		/* request completely satisfied */
		*already_done += remaining_bytes;
		running_result -= remaining_bytes;
		*new_result = *already_done;
	}
	else
	{
		/*
		 * This is actually pretty common and harmless, happens e.g. when one
		 * parts of the read are in the kernel page cache, but others
		 * aren't. So don't issue WARNING/ERROR, but just retry.
		 *
		 * While it can happen with single BLCKSZ reads (since they're bigger
		 * than typical page sizes), it's made much more likely by us
		 * combining reads.
		 *
		 * For writes it can happen e.g. in case of memory pressure.
		 */

		ereport(DEBUG3,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("aio %zd: partial %s:%s of %u-%u bytes at offset %llu+%u: read only %u of %u",
						myio - aio_ctl->in_progress_io,
						pgaio_io_operation_string(myio->op),
						pgaio_io_shared_callback_string(myio->scb),
						nbytes, *already_done,
						(long long unsigned) offset, *already_done,
						running_result,
						nbytes - *already_done)));

		Assert(running_result < remaining_bytes);
		*already_done += running_result;
		*new_result = *already_done;
		running_result = 0;

		*new_flags |= PGAIOIP_SOFT_FAILURE;
	}

	*running_result_p = running_result;
}

void
pgaio_process_io_completion(PgAioInProgress *io, int result)
{
	int running_result;
	PgAioInProgress *cur = io;
	bool first = true;

	Assert(io->flags & PGAIOIP_INFLIGHT);
	Assert(io->system_referenced);
	Assert(io->result == 0 || io->merge_with_idx == PGAIO_MERGE_INVALID);

	/*
	 * To test the retry logic artificially, define
	 * AIO_CHAOS_{READ,WRITE,FSYNC}_{SHORT,EAGAIN} to x to inject failures with
	 * probability 1/x.
	 */
#if 0
#define AIO_CHAOS_READ_EAGAIN 10
#define AIO_CHAOS_WRITE_EAGAIN 10
#define AIO_CHAOS_FSYNC_EAGAIN 10
#endif

#define R(x) ((random() % (x)) == 0)
#if defined(AIO_CHAOS_READ_SHORT)
	if (io->op == PGAIO_OP_READ && result > 4096 && R(AIO_CHAOS_READ_SHORT))
		result = 4096;
#endif
#if defined(AIO_CHAOS_WRITE_SHORT)
	if (io->op == PGAIO_OP_WRITE && result > 4096 && R(AIO_CHAOS_WRITE_SHORT))
		result = 4096;
#endif
	if (!(io->flags & PGAIOIP_RETRY))
	{
#if defined(AIO_CHAOS_READ_EAGAIN)
		if (io->op == PGAIO_OP_READ && R(AIO_CHAOS_READ_EAGAIN))
			result = -EAGAIN;
#endif
#if defined(AIO_CHAOS_WRITE_EAGAIN)
		if (io->op == PGAIO_OP_WRITE && R(AIO_CHAOS_WRITE_EAGAIN))
			result = -EAGAIN;
#endif
#if defined(AIO_CHAOS_FSYNC_EAGAIN)
		if (io->op == PGAIO_OP_FSYNC && R(AIO_CHAOS_FSYNC_EAGAIN))
			result = -EAGAIN;
#endif
	}
#undef R

	/* treat 0 length write as ENOSPC */
	if (result == 0 && io->op == PGAIO_OP_WRITE)
		running_result = result = -ENOSPC;
	else
		running_result = result;

	while (cur)
	{
		PgAioInProgress *next;
		PgAioIPFlags new_flags;

		new_flags = cur->flags;

		Assert(new_flags & PGAIOIP_INFLIGHT);
		Assert(cur->system_referenced);
		Assert(cur->op == io->op);
		Assert(cur->scb == io->scb);

		if (cur->merge_with_idx != PGAIO_MERGE_INVALID)
		{
			next = &aio_ctl->in_progress_io[cur->merge_with_idx];
			cur->merge_with_idx = PGAIO_MERGE_INVALID;
		}
		else
			next = NULL;

		switch (cur->op)
		{
			case PGAIO_OP_READ:
				pgaio_process_io_rw_completion(cur,
											   first,
											   cur->op_data.read.nbytes,
											   cur->op_data.read.offset,
											   &cur->op_data.read.already_done,
											   result,
											   &running_result,
											   &cur->result,
											   &new_flags);
				break;

			case PGAIO_OP_WRITE:
				pgaio_process_io_rw_completion(cur,
											   first,
											   cur->op_data.write.nbytes,
											   cur->op_data.write.offset,
											   &cur->op_data.write.already_done,
											   result,
											   &running_result,
											   &cur->result,
											   &new_flags);
				break;

			case PGAIO_OP_FSYNC:
			case PGAIO_OP_FLUSH_RANGE:
			case PGAIO_OP_NOP:
				cur->result = result;
				if (result == -EAGAIN || result == -EINTR)
					new_flags |= PGAIOIP_SOFT_FAILURE;
				else if (result < 0)
					new_flags |= PGAIOIP_HARD_FAILURE;
				break;

			case PGAIO_OP_INVALID:
				pg_unreachable();
				elog(ERROR, "invalid");
				break;
		}

		new_flags &= ~PGAIOIP_INFLIGHT;
		new_flags |= PGAIOIP_REAPED;

		WRITE_ONCE_F(cur->flags) = new_flags;

		dlist_push_tail(&my_aio->reaped, &cur->io_node);

		cur = next;
		first = false;
	}

}

/*
 * Extract iov_base and iov_len from a single IO.
 */
static void
pgaio_fill_one_iov(struct iovec *iov, const PgAioInProgress *io, bool first)
{
	switch (io->op)
	{
		case PGAIO_OP_WRITE:
			Assert(first || io->op_data.write.already_done == 0);
			iov->iov_base = (char *) io->op_data.write.bufdata + io->op_data.write.already_done;
			iov->iov_len = io->op_data.write.nbytes - io->op_data.write.already_done;
			break;
		case PGAIO_OP_READ:
			Assert(first || io->op_data.read.already_done == 0);
			iov->iov_base = (char *) io->op_data.read.bufdata + io->op_data.read.already_done;
			iov->iov_len = io->op_data.read.nbytes - io->op_data.read.already_done;
			break;
		default:
			elog(ERROR, "unexpected IO type while populating iovec");
	}
}

/*
 * Populate an array of iovec objects with the address ranges from a chain of
 * merged IOs.  Return the number of iovecs (which may be smaller than the
 * number of IOs).
 */
int
pgaio_fill_iov(struct iovec *iovs, const PgAioInProgress *io)
{
	struct iovec *iov;

	/* Fill in the first one. */
	iov = &iovs[0];
	pgaio_fill_one_iov(iov, io, true);

	/*
	 * We have a chain of IOs that were linked together because they access
	 * contiguous regions of a file.  As a micro-optimization we'll also
	 * consolidate iovecs that access contiguous memory.
	 */
	while (io->merge_with_idx != PGAIO_MERGE_INVALID)
	{
		struct iovec *next = iov + 1;

		io = &aio_ctl->in_progress_io[io->merge_with_idx];

		pgaio_fill_one_iov(next, io, false);
		if ((char *) iov->iov_base + iov->iov_len == next->iov_base)
			iov->iov_len += next->iov_len;
		else
			++iov;
	}

	return iov + 1 - iovs;
}

/*
 * Run an IO with a traditional blocking system call.  This is used by worker
 * mode to simulate AIO in worker processes, but it can also be used in other
 * modes if there's only a single IO to be submitted and we know we'll wait
 * for it anyway, saving some overheads.  XXX make that statement true
 */
void
pgaio_do_synchronously(PgAioInProgress *io)
{
	ssize_t result = 0;
	struct iovec iov[IOV_MAX];
	int iovcnt = 0;

	Assert(io->flags & PGAIOIP_INFLIGHT);

	/* Perform IO. */
	switch (io->op)
	{
		case PGAIO_OP_READ:
			iovcnt = pgaio_fill_iov(iov, io);
			pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_READ);
			result = pg_preadv(io->op_data.read.fd, iov, iovcnt,
							   io->op_data.read.offset + io->op_data.read.already_done);
			pgstat_report_wait_end();
			break;
		case PGAIO_OP_WRITE:
			iovcnt = pgaio_fill_iov(iov, io);
			pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
			result = pg_pwritev(io->op_data.write.fd, iov, iovcnt,
								io->op_data.write.offset + io->op_data.write.already_done);
			pgstat_report_wait_end();
			break;
		case PGAIO_OP_FSYNC:
			pgstat_report_wait_start(WAIT_EVENT_WAL_SYNC);
			if (io->op_data.fsync.datasync)
				result = pg_fdatasync(io->op_data.fsync.fd);
			else
				result = pg_fsync(io->op_data.fsync.fd);
			pgstat_report_wait_end();
			break;
		case PGAIO_OP_FLUSH_RANGE:
			pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_FLUSH);
			pg_flush_data(io->op_data.flush_range.fd,
						  io->op_data.flush_range.offset,
						  io->op_data.flush_range.nbytes);
			/* never errors */
			/* XXX previously we would PANIC on errors here */
			result = 0;
			pgstat_report_wait_end();
			break;
		case PGAIO_OP_NOP:
			result = 0;
			break;
		case PGAIO_OP_INVALID:
			result = -1;
			errno = EOPNOTSUPP;
			break;
	}

	pgaio_process_io_completion(io, result < 0 ? -errno : result);
}

const char *
pgaio_io_operation_string(PgAioOp op)
{
	switch (op)
	{
		case PGAIO_OP_INVALID:
			return "invalid";
		case PGAIO_OP_READ:
			return "read";
		case PGAIO_OP_WRITE:
			return "write";
		case PGAIO_OP_FSYNC:
			return "fsync";
		case PGAIO_OP_FLUSH_RANGE:
			return "flush_range";
		case PGAIO_OP_NOP:
			return "nop";
	}
	return "";
}
