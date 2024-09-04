/*-------------------------------------------------------------------------
 *
 * aio_io.c
 *    Asynchronous I/O subsytem.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio_io.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/fd.h"
#include "utils/wait_event.h"


static void
pgaio_io_before_prep(PgAioHandle *ioh)
{
	Assert(ioh->state == AHS_HANDED_OUT);
	Assert(pgaio_io_has_subject(ioh));
}

const char *
pgaio_io_get_op_name(PgAioHandle *ioh)
{
	Assert(ioh->op >= 0 && ioh->op < PGAIO_OP_COUNT);

	switch (ioh->op)
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

	pg_unreachable();
}

void
pgaio_io_prep_readv(PgAioHandle *ioh,
					int fd, int iovcnt, uint64 offset)
{
	pgaio_io_before_prep(ioh);

	ioh->op_data.read.fd = fd;
	ioh->op_data.read.offset = offset;
	ioh->op_data.read.iov_length = iovcnt;

	pgaio_io_prepare(ioh, PGAIO_OP_READ);
}

void
pgaio_io_prep_writev(PgAioHandle *ioh,
					 int fd, int iovcnt, uint64 offset)
{
	pgaio_io_before_prep(ioh);

	ioh->op_data.write.fd = fd;
	ioh->op_data.write.offset = offset;
	ioh->op_data.write.iov_length = iovcnt;

	pgaio_io_prepare(ioh, PGAIO_OP_WRITE);
}


extern void
pgaio_io_perform_synchronously(PgAioHandle *ioh)
{
	ssize_t		result = 0;
	struct iovec *iov = &aio_ctl->iovecs[ioh->iovec_off];

	/* Perform IO. */
	switch (ioh->op)
	{
		case PGAIO_OP_READ:
			pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_READ);
			result = pg_preadv(ioh->op_data.read.fd, iov,
							   ioh->op_data.read.iov_length,
							   ioh->op_data.read.offset);
			pgstat_report_wait_end();
			break;
		case PGAIO_OP_WRITE:
			pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
			result = pg_pwritev(ioh->op_data.write.fd, iov,
								ioh->op_data.write.iov_length,
								ioh->op_data.write.offset);
			pgstat_report_wait_end();
			break;
		default:
			elog(ERROR, "not yet");
	}

	ioh->result = result < 0 ? -errno : result;

	pgaio_io_process_completion(ioh, ioh->result);
}
