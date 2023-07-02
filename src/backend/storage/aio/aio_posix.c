/*-------------------------------------------------------------------------
 *
 * aio_posix.c
 *	  Routines for POSIX AIO.
 *
 * The POSIX AIO API is provided by the kernels of at least FreeBSD, NetBSD,
 * macOS, AIX and HP-UX.  It's also provided by user space-managed threads in
 * the runtime libraries of Linux (Glibc, Musl), illumos and Solaris.
 *
 * Uses the "exchange" mechanism to deal with the problem that results can
 * only be consumed by the process that submitted them.
 *
 * For macOS, the default kern.aio* settings are inadequate and must be
 * increased.  For AIX, shared_memory_type must be set to sysv because kernel
 * AIO cannot access mmap'd memory.  For Solaris and Linux, libc emulates POSIX
 * AIO with threads, which may not be better than io_method=worker.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/aio_posix.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <aio.h>
#include <fcntl.h>

#include "pgstat.h"
#include "miscadmin.h"
#include "storage/aio_internal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "utils/memutils.h"

/* AIO_LISTIO_MAX is missing on glibc systems.  16 is pretty conservative. */
#ifndef AIO_LISTIO_MAX
#define AIO_LISTIO_MAX 16
#endif

/* On systems with no O_DSYNC, just use the stronger O_SYNC. */
#ifdef O_DSYNC
#define PG_O_DSYNC O_DSYNC
#else
#define PG_O_DSYNC O_SYNC
#endif

/*
 * Decide which system interface to use to drain completions from the kernel,
 * if not explicitly specified already.
 */
#if !defined(USE_AIO_SUSPEND) && \
	!defined(USE_AIO_WAITCOMPLETE) && \
	!defined(USE_AIO_WAITN)
#if defined(HAVE_AIO_WAITCOMPLETE)
#define USE_AIO_WAITCOMPLETE
#elif defined(HAVE_AIO_WAITN)
#define USE_AIO_WAITN
#else
#define USE_AIO_SUSPEND
#endif
#endif

/*
 * A buffer to accumulate IOs for submission in a single lio_listio() call.
 */
typedef struct pgaio_posix_aio_listio_buffer
{
	int			nios;
	struct aiocb *cbs[AIO_LISTIO_MAX];
#if defined(LIO_READV) && defined(LIO_WRITEV)
	struct iovec iovecs[AIO_LISTIO_MAX][IOV_MAX];
#endif
}			pgaio_posix_aio_listio_buffer;

/*
 * If we're using aio_suspend(), we maintain an array of pointers to all active
 * aiocb structs (this isn't strictly necessary, but otherwise we'd have to
 * build the array from a linked list every time we drain).  We'll borrow the
 * aio_sigevent field to store the index in that array, so that we can maintain
 * it efficiently.
 */
#ifdef USE_AIO_SUSPEND
#define AIO_SUSPEND_ARRAY_INDEX(io) \
	((io)->io_method_data.posix_aio.iocb.aio_sigevent.sigev_value.sival_int)
static struct aiocb **pgaio_posix_aio_suspend_array;
static int	pgaio_posix_aio_suspend_array_size;
static void pgaio_posix_aio_suspend_array_init(struct PgAioInProgress *io);
static void pgaio_posix_aio_suspend_array_insert(struct PgAioInProgress *io);
static void pgaio_posix_aio_suspend_array_delete(struct PgAioInProgress *io);
#endif

/* Helper function declarations. */
static PgAioInProgress * io_for_iocb(struct aiocb *cb);
static struct aiocb *iocb_for_io(struct PgAioInProgress *io);
static void pgaio_posix_aio_submit_one(PgAioInProgress * io,
									   pgaio_posix_aio_listio_buffer * listio_buffer);
static int	pgaio_posix_aio_flush_listio(pgaio_posix_aio_listio_buffer * lb);
static int	pgaio_posix_aio_start_rw(PgAioInProgress * io,
									 pgaio_posix_aio_listio_buffer * lb);
static void pgaio_posix_aio_process_completion(PgAioInProgress * io,
											   int result,
											   bool in_interrupt_handler);
static int	pgaio_posix_aio_drain(PgAioContext *context, bool block, bool call_shared);
static int	pgaio_posix_aio_drain_internal(bool block, bool in_interrupt_handler);


/* Module startup and shutdown. */

static void
pgaio_posix_aio_postmaster_child_init_local(void)
{
#ifdef USE_AIO_SUSPEND
	/* Workspace for aio_suspend(). */
	if (pgaio_posix_aio_suspend_array == NULL)
		pgaio_posix_aio_suspend_array =
			MemoryContextAlloc(TopMemoryContext,
							   sizeof(struct aiocb *) * max_aio_in_flight);
#endif
}

static void
pgaio_posix_aio_postmaster_before_child_exit(void)
{
	/*
	 * XXX Remove this code, and let pgaio_postmaster_before_child_exit() take
	 * care of this, once it is fixed to understand IOs that we retried.  See
	 * also pgaio_posix_aio_closing_fd().  This is needed temporarily to get
	 * tests to pass on macOS and Solaris.
	 */
	START_CRIT_SECTION();
	pgaio_exchange_disable_interrupt();
	while (pg_atomic_read_u32(&my_aio->inflight_count) > 0)
		pgaio_posix_aio_drain_internal(true /* block */,
									   false /* in_interrupt_handler */);
	pgaio_exchange_enable_interrupt();
	pgaio_complete_ios(false);
	END_CRIT_SECTION();
}

/* Functions for submitting IOs to the kernel. */

/*
 * Submit a given number of pending IOs to the kernel, and optionally drain
 * any results that have arrived, without waiting.
 */
static int
pgaio_posix_aio_submit(int max_submit, bool drain)
{
	PgAioInProgress *ios[PGAIO_SUBMIT_BATCH_SIZE];
	int			nios = 0;
	pgaio_posix_aio_listio_buffer listio_buffer = {0};

	/*
	 * Disable the interrupt handler while we're interacting with AIO APIs
	 * (it's supposed to be async signal stuff, but clearly isn't on some
	 * platforms), and possibly modifying our suspend array.
	 */
	START_CRIT_SECTION();
	pgaio_exchange_disable_interrupt();

	while (!dlist_is_empty(&my_aio->pending))
	{
		dlist_node *node;
		PgAioInProgress *io;

		if (nios == max_submit)
			break;

		node = dlist_pop_head_node(&my_aio->pending);
		io = dlist_container(PgAioInProgress, io_node, node);

		pgaio_io_prepare_submit(io, 0);

		my_aio->submissions_total_count++;

		pgaio_posix_aio_submit_one(io, &listio_buffer);

		ios[nios] = io;
		++nios;
	}
	pgaio_posix_aio_flush_listio(&listio_buffer);

	pgaio_exchange_enable_interrupt();
	END_CRIT_SECTION();

	/*
	 * Others might have been waiting for this IO. Because it wasn't marked as
	 * in-flight until now, they might be waiting for the CV. Wake'em up.
	 */
	pgaio_broadcast_ios(ios, nios);

	/* callbacks will be called later by pgaio_submit() */
	if (drain)
		pgaio_drain(NULL,
					 /* block = */ false,
					 /* call_shared = */ false,
					 /* call_local = */ false);

	return nios;
}

/*
 * Resubmit an IO that was only partially completed (for example, a short
 * read) or that the kernel told us to retry.
 */
static void
pgaio_posix_aio_io_retry(PgAioInProgress * io)
{
	pgaio_posix_aio_listio_buffer listio_buffer = {0};

	WRITE_ONCE_F(io->flags) |= PGAIOIP_INFLIGHT;

	if (io->result == -EAGAIN)
	{
		/*
		 * The kernel said it's out of resources.  Wait for at least one IO
		 * submitted by this backend to complete before we continue (if there
		 * is one), because otherwise we'd have a busy retry loop.  This
		 * probably means the kernel needs some tuning.
		 *
		 * XXX If the problem is caused by other backends, we don't try to
		 * wait for them to drain.  Hopefully they will do that.
		 */
		pgaio_posix_aio_drain(NULL, true, false);
	}

	/* See comments in pgaio_posix_aio_submit(). */
	START_CRIT_SECTION();
	pgaio_exchange_disable_interrupt();

	pgaio_posix_aio_submit_one(io, &listio_buffer);
	pgaio_posix_aio_flush_listio(&listio_buffer);

	pgaio_exchange_enable_interrupt();
	END_CRIT_SECTION();

	ConditionVariableBroadcast(&io->cv);
}

/*
 * Queue up an IO request for the kernel in listio_buffer where possible, and
 * otherwise tell the kernel to initiate the IO immediately.
 */
static void
pgaio_posix_aio_submit_one(PgAioInProgress * io,
						   pgaio_posix_aio_listio_buffer * listio_buffer)
{
	struct aiocb *iocb = iocb_for_io(io);
	int			rc = -1;

	pg_atomic_fetch_add_u32(&my_aio->inflight_count, 1);

	pgaio_exchange_submit_one(io);

	/* Populate the POSIX AIO iocb. */
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_sigevent.sigev_notify = SIGEV_NONE;
#ifdef USE_AIO_SUSPEND
	pgaio_posix_aio_suspend_array_init(io);
#endif

	switch (io->op)
	{
		case PGAIO_OP_READ:
			iocb->aio_fildes = io->op_data.read.fd;
			iocb->aio_offset = io->op_data.read.offset +
				io->op_data.read.already_done;
			rc = pgaio_posix_aio_start_rw(io, listio_buffer);
			break;
		case PGAIO_OP_WRITE:
			iocb->aio_fildes = io->op_data.write.fd;
			iocb->aio_offset = io->op_data.write.offset +
				io->op_data.write.already_done;
			rc = pgaio_posix_aio_start_rw(io, listio_buffer);
			break;
		case PGAIO_OP_FSYNC:
			iocb->aio_fildes = io->op_data.fsync.fd;
#ifdef USE_AIO_SUSPEND
			pgaio_posix_aio_suspend_array_insert(io);
#endif
			rc = aio_fsync(io->op_data.fsync.datasync ? PG_O_DSYNC : O_SYNC,
						   iocb);
			break;
		case PGAIO_OP_FLUSH_RANGE:

			/*
			 * This is supposed to represent Linux's sync_file_range(), which
			 * initiates writeback for only a certain range of a file.
			 * Initiating fdatasync() seems close to the intended behavior.
			 * XXX this is a bad idea.  Should we just do nothing instead?
			 */
			iocb->aio_fildes = io->op_data.flush_range.fd;
#ifdef USE_AIO_SUSPEND
			pgaio_posix_aio_suspend_array_insert(io);
#endif
			rc = aio_fsync(PG_O_DSYNC, iocb);
			break;
		case PGAIO_OP_NOP:
			rc = 0;
			break;
		case PGAIO_OP_INVALID:
			rc = -1;
			errno = EOPNOTSUPP;
			break;
	}

	/* If we failed to submit, then try to reap immediately. */
	if (rc < 0)
		pgaio_posix_aio_process_completion(io, -errno, false);
}

/*
 * Submit all IOs that have accumlated in "lb" to the kernel in a single
 * lio_listio() call.
 */
static int
pgaio_posix_aio_flush_listio(pgaio_posix_aio_listio_buffer * lb)
{
	int			rc;

	if (lb->nios == 0)
		return 0;

#ifdef USE_AIO_SUSPEND
	for (int i = 0; i < lb->nios; ++i)
		pgaio_posix_aio_suspend_array_insert(io_for_iocb(lb->cbs[i]));
#endif

	/* Try to initiate all of these IOs with one system call. */
	rc = lio_listio(LIO_NOWAIT, lb->cbs, lb->nios, NULL);
	if (rc < 0)
	{
		if (errno == EAGAIN || errno == EINTR || errno == EIO)
		{
			int			listio_errno = errno;

			/*
			 * POSIX says that for these three errors only, some of the IOs
			 * may have been queued.  We have to figure out which ones.
			 */
			for (int i = 0; i < lb->nios; ++i)
			{
				PgAioInProgress *io;
				int			error;

				io = io_for_iocb(lb->cbs[i]);
				error = aio_error(iocb_for_io(io));
				if (error == EINPROGRESS || error == 0)
					continue;	/* submitted or already finished */

				/*
				 * If we failed to submit, we may see -1 here and the reason
				 * in errno.
				 */
				if (error < 0)
					error = errno;
				if (error == EINVAL)
				{
					/*
					 * We failed to initiate this IO, so the kernel doesn't
					 * even recognize the iocb.  We have to report the EAGAIN
					 * etc that we received from lio_listio() rather than
					 * EINVAL so that pgaio_process_io_completion treats it as
					 * a soft failure.
					 */
					error = listio_errno;
				}
				pgaio_posix_aio_process_completion(io, -error, false);
			}
		}
		else
		{
			int			error = errno;

			/*
			 * The only other error documented by POSIX is EINVAL.  Whether we
			 * get that or something undocumented, replicate the error we got
			 * from lio_listio() into all the IOs.
			 */
			for (int i = 0; i < lb->nios; ++i)
			{
				PgAioInProgress *io;

				io = io_for_iocb(lb->cbs[i]);
				pgaio_posix_aio_process_completion(io, -error, false);
			}
		}
	}
	lb->nios = 0;

	return 0;
}

/*
 * Add one IO to the buffer "lb", flushing if it's already full.
 */
static int
pgaio_posix_aio_add_listio(pgaio_posix_aio_listio_buffer * lb, PgAioInProgress * io)
{
	struct aiocb *iocb;
	int index;

	if (lb->nios == AIO_LISTIO_MAX)
	{
		int			rc;

		rc = pgaio_posix_aio_flush_listio(lb);
		if (rc < 0)
			return rc;
	}

	index = lb->nios;
	iocb = iocb_for_io(io);
	lb->cbs[index] = iocb;

#if defined(LIO_READV) && defined(LIO_WRITEV)
	if (iocb->aio_lio_opcode == LIO_READV || iocb->aio_lio_opcode == LIO_WRITEV) {
		/* Copy the iovecs into a new buffer with the right lifetime. */
		if (iocb->aio_iovcnt > IOV_MAX)
			elog(ERROR, "too many iovecs");
		memcpy(&lb->iovecs[index][0],
			   unvolatize(void *, iocb->aio_iov),
			   sizeof(struct iovec) * iocb->aio_iovcnt);
		iocb->aio_iov = &lb->iovecs[index][0];
	}
#endif

	++lb->nios;

	return 0;
}

/*
 * Start a read or write, or add it to "lb" to be started along with others,
 * depending on available system calls on this system.
 *
 * Assumes that aio_filedes and aio_offset are already set.
 */
static int
pgaio_posix_aio_start_rw(PgAioInProgress * io,
						 pgaio_posix_aio_listio_buffer * lb)
{
	struct aiocb *cb = iocb_for_io(io);
	struct iovec iov[IOV_MAX];
	int			iovcnt PG_USED_FOR_ASSERTS_ONLY;

	iovcnt = pgaio_fill_iov(iov, io);

	if (iovcnt > 1)
	{
#if defined(LIO_READV) && defined(LIO_WRITEV)
		/* FreeBSD supports async readv/writev in an lio batch. */
		cb->aio_iov = iov;	/* this will be copied */
		cb->aio_iovcnt = iovcnt;
		cb->aio_lio_opcode = io->op == PGAIO_OP_WRITE ? LIO_WRITEV : LIO_READV;
		return pgaio_posix_aio_add_listio(lb, io);
#endif

		/* pgaio_can_scatter_gather() should not have allowed this. */
		elog(ERROR, "unexpected vector read/write");
		pg_unreachable();
	}
	else
	{
		/*
		 * Standard POSIX AIO doesn't have scatter/gather.  This IO might still
		 * have been merged from multiple IOs that access adjacent regions of a
		 * file, but only if the memory is also adjacent.
		 */
		Assert(iovcnt == 1);
		cb->aio_buf = iov[0].iov_base;
		cb->aio_nbytes = iov[0].iov_len;
		cb->aio_lio_opcode = io->op == PGAIO_OP_WRITE ? LIO_WRITE : LIO_READ;
		return pgaio_posix_aio_add_listio(lb, io);
	}
}


/* Functions for waiting for IOs to complete. */

/*
 * Drain completion events from the kernel, reaping them if possible.  If
 * block is true, wait for at least one to complete, unless there are none in
 * flight.
 */
static int
pgaio_posix_aio_drain(PgAioContext *context, bool block, bool call_shared)
{
	int			ndrained;

	START_CRIT_SECTION();
	pgaio_exchange_disable_interrupt();
	ndrained = pgaio_posix_aio_drain_internal(block,
											  false /* in_interrupt_handler */);
	pgaio_exchange_enable_interrupt();

	if (call_shared)
		pgaio_complete_ios(false);
	END_CRIT_SECTION();

	return ndrained;
}

static void
pgaio_posix_aio_drain_in_interrupt_handler(void)
{
	START_CRIT_SECTION();
	pgaio_exchange_disable_interrupt();
	pgaio_posix_aio_drain_internal(false /* block */,
								   true /* in_interrupt_handler */);
	pgaio_exchange_enable_interrupt();
	END_CRIT_SECTION();
}

/*
 * If in_interrupt_handler is true, results from the kernel are written into
 * shared memory for processing later or by another backend.
 */
static int
pgaio_posix_aio_drain_internal(bool block, bool in_interrupt_handler)
{
	struct timespec timeout = {0, 0};
	int			ndrained = 0;

#ifdef USE_AIO_SUSPEND
	/* This is the least efficient, but most standard option. */
	for (;;)
	{
		struct aiocb *iocb;
		ssize_t		rc;

		if (pgaio_posix_aio_suspend_array_size == 0)
			break;		/* skip if array is empty */
		rc = aio_suspend((const struct aiocb *const *) pgaio_posix_aio_suspend_array,
						 pgaio_posix_aio_suspend_array_size,
						 ndrained == 0 && block ? NULL : &timeout);
		if (rc < 0)
		{
			if (errno == EAGAIN)
				break;			/* all IOs in progress */
			if (errno != EINTR)
				elog(ERROR, "could not wait for IO completion: %m");
			continue;			/* signaled; retry */
		}
		/* Something has completed, but what? */
		for (int i = 0; i < pgaio_posix_aio_suspend_array_size;)
		{
			int			error;

			/* Still running? */
			iocb = pgaio_posix_aio_suspend_array[i];
			error = aio_error(iocb);
			if (error < 0)
				error = errno;
			if (error == EINPROGRESS)
			{
				++i;
				continue;
			}

			/* Consume the result. */
			rc = aio_return(iocb);

			pgaio_posix_aio_process_completion(io_for_iocb(iocb),
											   error == 0 ? rc : -error,
											   in_interrupt_handler);
			ndrained++;

			/* Don't advance i; another item was moved into element i. */
		}
		break;
	}
#endif
#ifdef USE_AIO_WAITCOMPLETE
	/* FreeBSD can drain like a queue, one at a time. */
	for (;;)
	{
		struct aiocb *iocb;
		ssize_t		result;

		if (pg_atomic_read_u32(&my_aio->inflight_count) == 0)
			break;		/* skip if we know nothing is in flight */
		result = aio_waitcomplete(&iocb,
								  ndrained == 0 && block ? NULL : &timeout);
		if (result < 0 && iocb == NULL)
		{
			if (errno == EAGAIN)
				break;			/* no IOs in progress? */
			else if (errno == EINPROGRESS)
				break;			/* all IOs in progress, !block */
			else if (errno != EINTR)
				elog(ERROR, "could not wait for IO completion: %m");
			continue;			/* signaled; retry */
		}
		pgaio_posix_aio_process_completion(io_for_iocb(iocb),
										   result < 0 ? -errno : result,
										   in_interrupt_handler);
		ndrained++;
	}
#endif
#ifdef USE_AIO_WAITN
	/* Solaris-family systems can drain like a queue, N at a time. */
	for (;;)
	{
		struct aiocb *iocbs[128];
		unsigned int nwait = 1;

		if (pg_atomic_read_u32(&my_aio->inflight_count) == 0)
			break;		/* skip if we know nothing is in flight */
		if (aio_waitn(iocbs, lengthof(iocbs), &nwait, block ? NULL : &timeout) < 0)
		{
			if (errno == ETIME)
				break;
			if (errno != EINTR)
				elog(ERROR, "could not wait for IO completion: %m");
			/* If interruped by a signal, restart only if nothing consumed. */
			if (nwait == 0)
				continue;
		}
		for (int i = 0; i < nwait; ++i)
		{
			struct aiocb *iocb = iocbs[i];
			int			result;
			int			error;

			error = aio_error(iocb);
			if (error < 0)
				error = errno;
			result = aio_return(iocb);
			pgaio_posix_aio_process_completion(io_for_iocb(iocb),
											   error == 0 ? result : -error,
											   in_interrupt_handler);
			ndrained++;
		}
		break;
	}
#endif

	return ndrained;
}

static void
pgaio_posix_aio_process_completion(PgAioInProgress * io,
								   int raw_result,
								   bool in_interrupt_handler)
{
	/* XXX Hrmph.   How to avoid doing this in interrupt handler? */
	pg_atomic_fetch_sub_u32(&my_aio->inflight_count, 1);

#ifdef USE_AIO_SUSPEND
	pgaio_exchange_disable_interrupt();
	pgaio_posix_aio_suspend_array_delete(io);
	pgaio_exchange_enable_interrupt();
#endif

	pgaio_exchange_process_completion(io, raw_result, in_interrupt_handler);
}

/*
 * Given an aiocb, return the associated PgAioInProgress.
 */
static PgAioInProgress *
io_for_iocb(struct aiocb *cb)
{
	return (PgAioInProgress *)
		(((char *) cb) - offsetof(PgAioInProgress,
								  io_method_data.posix_aio.iocb));
}

static struct aiocb *
iocb_for_io(PgAioInProgress * io)
{
	return &io->io_method_data.posix_aio.iocb;
}

#ifdef USE_AIO_SUSPEND

/*
 * Initialize state to say that the IO is not in the aio_suspend array yet.
 */
static void
pgaio_posix_aio_suspend_array_init(PgAioInProgress * io)
{
	AIO_SUSPEND_ARRAY_INDEX(io) = -1;
}

/*
 * Add IO to aio_suspend array so that drain() can wait for it.
 */
static void
pgaio_posix_aio_suspend_array_insert(PgAioInProgress * io)
{
	int i;

	if (pgaio_posix_aio_suspend_array_size == max_aio_in_flight)
		elog(PANIC, "too many IOs in flight");

	i = pgaio_posix_aio_suspend_array_size++;
	AIO_SUSPEND_ARRAY_INDEX(io) = i;
	pgaio_posix_aio_suspend_array[i] = iocb_for_io(io);
}

/*
 * Forget about an IO that has completed.
 */
static void
pgaio_posix_aio_suspend_array_delete(PgAioInProgress * io)
{
	int			highest_index = pgaio_posix_aio_suspend_array_size - 1;
	int			i = AIO_SUSPEND_ARRAY_INDEX(io);
	PgAioInProgress *migrant;

	/* Nothing to do if not in the array. */
	if (i == -1)
		return;

	AIO_SUSPEND_ARRAY_INDEX(io) = -1;

	if (i != highest_index)
	{
		/* Migrate the highest entry into the new empty slot, to avoid gaps. */
		migrant = io_for_iocb(pgaio_posix_aio_suspend_array[highest_index]);
		AIO_SUSPEND_ARRAY_INDEX(migrant) = i;
		pgaio_posix_aio_suspend_array[i] = iocb_for_io(migrant);
	}
	Assert(pgaio_posix_aio_suspend_array_size > 0);
	--pgaio_posix_aio_suspend_array_size;
}

#endif

/*
 * POSIX leaves it unspecified whether the OS cancels IOs when you close the
 * underlying descriptor.  Some do (macOS, Solaris), some don't (FreeBSD) and
 * one starts mixing up unrelated fds (glibc).  Therefore, drain everything
 * from this fd, unless we're on a system known not to need it.  For now that
 * corresponds to the systems using aio_suspend() or aio_waitn().
 */
static void
pgaio_posix_aio_closing_fd(int fd)
{
#ifdef USE_AIO_SUSPEND
	struct aiocb *iocb;
	bool waiting;

	START_CRIT_SECTION();
	pgaio_exchange_disable_interrupt();
	for (;;)
	{
		waiting = false;

		for (int i = 0; i < pgaio_posix_aio_suspend_array_size; ++i)
		{
			iocb = pgaio_posix_aio_suspend_array[i];
			if (iocb->aio_fildes == fd)
			{
				waiting = true;
				break;
			}
		}

		if (!waiting)
			break;

		/*
		 * Since this function might run any time we have vfd pressure, we
		 * might not be able to run pgaio_complete_ios() safely.  That means
		 * that we can't "reap" the IO.  If we did that, it might sit on our
		 * reaped list just before we wait for something else, creating
		 * arbitrary deadlock risks.  Therefore, we'll drain the result from
		 * the kernel, but allow any backend to reap it, using the
		 * "in_interrupt_handler" mode.
		 *
		 * XXX This usage suggests that parameter might need a better name
		 */
		pgaio_posix_aio_drain_internal(true /* block */,
									   true /* in_interrupt_handler */);
	}
	pgaio_exchange_enable_interrupt();
	END_CRIT_SECTION();
#endif
#ifdef USE_AIO_WAITN
	/*
	 * XXX TODO until we have a good way to iterate over all submittted IOs (ie
	 * that works for retries) looking for the fd, we'll just be
	 * pessimistic here and wait for all.  That's because we don't have the
	 * array that we maintain only for aio_suspend builds.
	 */
	START_CRIT_SECTION();
	pgaio_exchange_disable_interrupt();
	while (pg_atomic_read_u32(&my_aio->inflight_count) > 0)
		pgaio_posix_aio_drain_internal(true /* block */,
									   true /* in_interrupt_handler */);
	pgaio_exchange_enable_interrupt();
	END_CRIT_SECTION();
#endif
}

const IoMethodOps pgaio_posix_aio_ops = {
	.shmem_init = pgaio_exchange_shmem_init,
	.postmaster_child_init_local = pgaio_posix_aio_postmaster_child_init_local,
	.postmaster_before_child_exit = pgaio_posix_aio_postmaster_before_child_exit,

	.submit = pgaio_posix_aio_submit,
	.retry = pgaio_posix_aio_io_retry,
	.wait_one = pgaio_exchange_wait_one,
	.drain = pgaio_posix_aio_drain,
	.drain_in_interrupt_handler = pgaio_posix_aio_drain_in_interrupt_handler,
	.closing_fd = pgaio_posix_aio_closing_fd,

#if defined(LIO_READV) && defined(LIO_WRITEV)
	.can_scatter_gather_direct = true,
	.can_scatter_gather_buffered = true
#endif
};
