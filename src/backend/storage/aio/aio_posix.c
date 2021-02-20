/*-------------------------------------------------------------------------
 *
 * aio_posix.c
 *	  Routines for POSIX AIO.
 *
 * The kernel (or on some systems, a pool of threads) manages IOs submitted
 * with aio_XXX() and lio_listio(), and notifies us of completion with signals.
 * A signal handler pushes completion events into a shared completion queue
 * where they'll be processed by the next backend to "drain" the queue.  That
 * seems Rube Goldberg-esque, but avoids the risk of deadlock when processes
 * wait for IOs that were initiated by other processes at arbitrary times.
 * Other less portable but more efficient notification methods may be possible
 * in the future.
 *
 * This method of AIO is available in --with-posix-aio builds.  It is
 * available on many Unix-like operating systems, but the quality of
 * implementations varies considerably, leading to some workarounds and
 * special cases.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/aio_worker.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <aio.h>
#include <fcntl.h>

#include "pgstat.h"
#include "miscadmin.h"
#include "storage/aio_internal.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"

static sig_atomic_t pgaio_posix_set_latch_on_return = false;

/* AIO_LISTIO_MAX is missing on glibc systems.  16 is pretty conservative. */
#ifndef AIO_LISTIO_MAX
#define AIO_LISTIO_MAX 16
#endif

/*
 * Realtime signals are missing on macOS systems.  We have to work harder in
 * that case, tracking all IOs in an array that we can safely scan from a
 * signal handler.  Also, macOS's lio_listio() facility is not suitable due to
 * lack of per-IO notifications.
 */
#if defined(SIGRTMIN) && !defined(MISSING_POSIX_REALTIME_SIGNALS)
#define PGAIO_POSIX_SIGNO SIGRTMIN
#else
#define MISSING_POSIX_REALTIME_SIGNALS
#define MISSING_LIO_LISTIO_NOTIFICATIONS
#define PGAIO_POSIX_SIGNO SIGIO
volatile sig_atomic_t *my_inflight_io;
volatile sig_atomic_t my_inflight_io_count;
#endif

/*
 * Glibc's pthread-based fake AIO implementation can occasionally deadlock in
 * the signal handler, because its aio_error() and aio_return() functions are
 * not async-signal-safe (which they should be according to POSIX).
 * Specifically, they use pthead mutexes that can deadlock against the user
 * context while it's trying to submit.  We have no choice but to block the
 * signal while submitting.  Define POSIX_AIO_ASYNC_SIG_UNSAFE to test those
 * code paths on other systems.
 */
#if defined(__GLIBC__)
#define POSIX_AIO_ASYNC_SIG_UNSAFE
#endif
#ifdef POSIX_AIO_ASYNC_SIG_UNSAFE
static sigset_t pgaio_posix_sigmask;
#endif

static void pgaio_posix_signal_handler(int sig, siginfo_t *si, void *uap);

Size
AioPosixShmemSize(void)
{
	return squeue32_estimate(max_aio_in_progress);
}

void
AioPosixShmemInit(void)
{
	bool found;

	aio_ctl->posix_completion_queue = (squeue32 *)
		ShmemInitStruct("AioPosixCompletionQueue", AioPosixShmemSize(), &found);
	Assert(!found);
	squeue32_init(aio_ctl->posix_completion_queue, max_aio_in_progress);
}

void
pgaio_posix_postmaster_child_init_local(void)
{
	struct sigaction sa;

	/*
	 * Any system that supports POSIX AIO should also support sigaction() with
	 * a three-argument signal handler.  (macOS doesn't exactly support it
	 * correctly, but it still compiles).
	 */
	sa.sa_sigaction = pgaio_posix_signal_handler;
	sa.sa_flags = SA_RESTART | SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	if (sigaction(PGAIO_POSIX_SIGNO, &sa, NULL) < 0)
		elog(ERROR, "could not install signal handler: %m");

#ifdef MISSING_POSIX_REALTIME_SIGNALS
	my_inflight_io = malloc(max_aio_in_flight * sizeof(sig_atomic_t));
	for (size_t i = 0; i < max_aio_in_flight; ++i)
		my_inflight_io[i] = (sig_atomic_t) -1;
	my_inflight_io_count = 0;
#endif

#ifdef POSIX_AIO_ASYNC_SIG_UNSAFE
	sigemptyset(&pgaio_posix_sigmask);
	sigaddset(&pgaio_posix_sigmask, PGAIO_POSIX_SIGNO);
#endif
}

static inline void
pgaio_posix_presubmit(void)
{
#ifdef POSIX_AIO_ASYNC_SIG_UNSAFE
	sigprocmask(SIG_BLOCK, NULL, &pgaio_posix_sigmask);
#endif
}

static inline void
pgaio_posix_postsubmit(void)
{
#ifdef POSIX_AIO_ASYNC_SIG_UNSAFE
	sigprocmask(SIG_UNBLOCK, NULL, &pgaio_posix_sigmask);
#endif
}

/*
 * Set up the POSIX aiocb struct for an IO.
 */
static void
pgaio_posix_preflight(PgAioInProgress *io)
{
	size_t io_index = pgaio_io_id(io);

	io->submitter_id = my_aio_id;
	pg_atomic_add_fetch_u32(&my_aio->inflight_count, 1);

	/* Reguest a signal on completion. */
	memset(&io->posix_aiocb, 0, sizeof(io->posix_aiocb));
	io->posix_aiocb.aio_sigevent.sigev_notify = SIGEV_SIGNAL;
	io->posix_aiocb.aio_sigevent.sigev_signo = PGAIO_POSIX_SIGNO;

#ifndef MISSING_POSIX_REALTIME_SIGNALS
	/*
	 * On most systems, we can ask for the IO index to be passed to the signal
	 * handler.
	 */
	io->posix_aiocb.aio_sigevent.sigev_value.sival_int = io_index;
#else
	/*
	 * On macOS (or other systems when testing with
	 * MISSING_POSIX_REALTIME_SIGNALS defined), the signal handler will need to
	 * poll all outstanding IOs whenever a signal arrives, because the kernel
	 * isn't smart enough to pass through sigev_value.
	 *
	 * We do a search for a free slot in an array of sig_atomic_t, so that it's
	 * safe to do this while the signal handler can be removing values at any
	 * time ; it's a bit ugly, but we'll probably find a slot near the
	 * beginning.  sig_atomic_t can be signed or unsigned, but it must at least
	 * big enough to hold indexes into in_progress_io.
	 */
	StaticAssertStmt(sizeof(sig_atomic_t) >= sizeof(int),
					 "sig_atomic_t too small to hold an AIO index");

	for (size_t slot = 0; slot < max_aio_in_flight; ++slot)
	{
		if (my_inflight_io[slot] == (sig_atomic_t) -1)
		{
			my_inflight_io[slot] = io_index;
			io->inflight_slot = slot;

			/* Track the upper bound to limit the signal handler's search. */
			if (slot >= my_inflight_io_count)
				my_inflight_io_count = slot + 1;

			return;
		}
	}

	elog(PANIC, "too many IOs in flight");
#endif
}

/*
 * Called in user context when we fail to submit an IO, and in signal handler
 * context when an IO completes.
 */
static void
pgaio_posix_postflight(PgAioInProgress *io)
{
	Assert(io->submitter_id == my_aio_id);
	pg_atomic_fetch_sub_u32(&my_aio->inflight_count, 1);

#ifdef MISSING_POSIX_REALTIME_SIGNALS
	/* Poor old macOS can stop polling this one on every signal now. */
	my_inflight_io[io->inflight_slot] = (sig_atomic_t) -1;
#endif
}

/*
 * Check if an IO that was submitted by this process has completed, and if so,
 * retrieve the result and put the IO on the shared completion queue for
 * processing by any process.
 */
static void
pgaio_posix_poll_aiocb(PgAioInProgress *io)
{
	ssize_t	return_status;
	int		error_status;

#ifdef PG_HAVE_ATOMIC_U64_SIMULATION
	/* Spinlocks are not safe in this context. */
#error "Cannot use squeue32 from signal handler without atomics"
#endif

	Assert(io->submitter_id == my_aio_id);

	/* Check if the IO has completed and has an error status. */
	error_status = aio_error(&io->posix_aiocb);
	if (error_status == 0)
	{
		/* The IO succeeded.  Retrieve the return status. */
		io->posix_returned_generation = io->generation;
		return_status = aio_return(&io->posix_aiocb);
		if (return_status < 0)
			elog(PANIC, "aio_return() failed with error status 0: %m");
		io->result = return_status;
		pgaio_posix_postflight(io);
	}
	else if (error_status < 0)
	{
		/*
		 * On macOS, we might poll an aiocb that is about to be submitted, or
		 * that we recently failed to submit, so it's unknown to the kernel.
		 * On other systems EINVAL would imply that we've received a spurious
		 * signal.
		 */
		if (errno != EINVAL)
			elog(PANIC, "aio_error() failed: %m");
		return;
	}
	else if (error_status == EINPROGRESS)
	{
		/*
		 * The IO is still running.  Expected only on macOS.  On other systems
		 * it would imply a spurious signal.
		 */
		return;
	}
	else
	{
		/* The IO failed.  Ignore the return value. */
		io->posix_returned_generation = io->generation;
		aio_return(&io->posix_aiocb);

		/* Set the error using a negative result value. */
		io->result = -error_status;
		pgaio_posix_postflight(io);
	}

	/*
	 * Push this into the shared completion queue so that any backend can
	 * handle it.  This is async-signal-safe, since it's based on atomic
	 * operations.
	 *
	 * XXX Explain theory about why the queue must have enough room for a newly
	 * completed IO.
	 */
	if (!squeue32_enqueue(aio_ctl->posix_completion_queue, pgaio_io_id(io)))
		elog(PANIC, "shared completion queue unexpectedly full");

	/*
	 * Set our latch if requested to do so by pgaio_posix_wait_one(), to close
	 * a race.
	 */
	if (pgaio_posix_set_latch_on_return)
		SetLatch(MyLatch);
}

/*
 * Whenever an IO initated by this process completes, we receive a signal.
 */
static void
pgaio_posix_signal_handler(int sig, siginfo_t *si, void *uap)
{
	int		save_errno = errno;
	PgAioInProgress *io;

#ifndef MISSING_POSIX_REALTIME_SIGNALS
	/* Defend against spurious signals. */
	if (si->si_value.sival_int >= 0 &&
		si->si_value.sival_int < max_aio_in_progress)
	{
		io = &aio_ctl->in_progress_io[si->si_value.sival_int];
		pgaio_posix_poll_aiocb(io);
	}
#else
	/*
	 * Without realtime signals and signal queuing, unhandled signals are
	 * merged and don't carry an si_value that we can use to point to our IO
	 * object.  This is going to require O(n^2) checks in the number of
	 * concurrent IOs.  Oh well.
	 */
	size_t count = my_inflight_io_count;

	for (size_t i = 0; i < count; ++i)
	{
		int index = my_inflight_io[i];

		if (index != (sig_atomic_t) -1)
		{
			io = &aio_ctl->in_progress_io[index];
			pgaio_posix_poll_aiocb(io);
		}
	}
#endif

	errno = save_errno;
}

int
pgaio_posix_drain(PgAioContext *context)
{
	uint32 io_index;
	int ndrained = 0;

	/* Reap as many completed IOs as we can without waiting. */
	while (squeue32_dequeue(aio_ctl->posix_completion_queue, &io_index))
	{
		PgAioInProgress *io = &aio_ctl->in_progress_io[io_index];
		int merged_result;

		merged_result = io->result;
		io->result = 0;

		pgaio_process_io_completion(io, merged_result);
		++ndrained;
	}

	return ndrained;
}

typedef struct pgaio_posix_listio_buffer
{
	int nios;
	struct aiocb *cbs[AIO_LISTIO_MAX];
} pgaio_posix_listio_buffer;

#ifndef MISSING_LIO_LISTIO_NOTIFICATIONS
static PgAioInProgress *
pgaio_io_for_posix_aiocb(struct aiocb *cb)
{
	return (PgAioInProgress *)
		(((char *) cb) - offsetof(PgAioInProgress, posix_aiocb));
}
#endif

static int
pgaio_posix_flush_listio(pgaio_posix_listio_buffer *lb)
{
#ifndef MISSING_LIO_LISTIO_NOTIFICATIONS
	uint64 io_generation[AIO_LISTIO_MAX];
	int rc;

	if (lb->nios == 0)
		return 0;

	/* Record the generation of all IOs we're going to submit to the kernel. */
	for (int i = 0; i < lb->nios; ++i)
		io_generation[i] = pgaio_io_for_posix_aiocb(lb->cbs[i])->generation;

	/* Try to initiate all of these IOs in one system call. */
	rc = lio_listio(LIO_NOWAIT, lb->cbs, lb->nios, NULL);
	if (rc < 0)
	{
		if (errno == EAGAIN || errno == EINTR || errno == EIO)
		{
			int listio_errno = errno;

			/*
			 * POSIX says that for these three errors only, some of the IOs may
			 * have been queued.  Any that were queued will also be processed
			 * by the signal handler (and then could be concurrently recycled
			 * by another process), and any that weren't will need to be
			 * processed by us.
			 */
			for (int i = 0; i < lb->nios; ++i)
			{
				PgAioInProgress *io;
				int			error_status;

				io = pgaio_io_for_posix_aiocb(lb->cbs[i]);
				error_status = aio_error(&io->posix_aiocb);
				if (error_status == 0 || error_status == EINPROGRESS)
				{
					/*
					 * Successfully submitted an IO.  The signal handler will
					 * handle the rest.  It's also possible that the aiocb has
					 * already been recycled for another request, and that's
					 * what we're seeig here, but that still means that the
					 * request we're interested in was successfully initated.
					 */
					continue;
				}
				else if (error_status == EINVAL)
				{
					if (io->posix_returned_generation >= io_generation[i])
					{
						/*
						 * Successfully submitted an IO, and the signal handler
						 * has already called aio_return(), and by now the
						 * aiocb has been recycled for some other request.
						 */
						 continue;
					}
					/*
					 * We failed to initiate this IO, so the kernel doesn't
					 * recognize the aiocb.  It seems more cromulent to report
					 * the EAGAIN etc that we received from lio_listio() rather
					 * than EINVAL, so let's do that.
					 */
					error_status = listio_errno;
				}
				pgaio_process_io_completion(pgaio_io_for_posix_aiocb(lb->cbs[i]),
											-error_status);
				pgaio_posix_postflight(pgaio_io_for_posix_aiocb(lb->cbs[i]));
			}
		}
		else
		{
			int error_status = errno;

			/*
			 * The only other error documented by POSIX is EINVAL.  Whether we
			 * get that or something undocumented, replicate the error we got
			 * from lio_listio() into all the IOs.
			 */
			for (int i = 0; i < lb->nios; ++i)
			{
				pgaio_process_io_completion(pgaio_io_for_posix_aiocb(lb->cbs[i]),
											-error_status);
				pgaio_posix_postflight(pgaio_io_for_posix_aiocb(lb->cbs[i]));
			}
		}
	}
	lb->nios = 0;
#endif

	return 0;
}

#ifndef MISSING_LIO_LISTIO_NOTIFICATIONS
static int
pgaio_posix_add_listio(pgaio_posix_listio_buffer *lb, PgAioInProgress *io)
{
	if (lb->nios == AIO_LISTIO_MAX)
	{
		int rc;

		rc = pgaio_posix_flush_listio(lb);
		if (rc < 0)
			return rc;
	}
	lb->cbs[lb->nios] = &io->posix_aiocb;
	++lb->nios;

	return 0;
}
#endif

/*
 * Assumes that io->posix_aiocb is cleared, but has the aio_filedes and
 * aio_offset already set.
 */
static int
pgaio_posix_start_rw(PgAioInProgress *io, pgaio_posix_listio_buffer *lb,
					 int lio_opcode)
{
	struct aiocb *cb = &io->posix_aiocb;
	struct iovec iov[IOV_MAX];
	int iovcnt PG_USED_FOR_ASSERTS_ONLY;

	iovcnt = pgaio_fill_iov(iov, io);

#if defined(HAVE_AIO_READV) && defined(HAVE_AIO_WRITEV)
	if (iovcnt > 1)
	{
		/*
		 * We can't do scatter/gather in a listio on any known OS, but it's
		 * better to use FreeBSD's nonstandard separate system calls than pass
		 * up the opportunity for scatter/gather IO.  Note that this case
		 * should only be reachable if pgaio_can_scatter_gather() returned
		 * true.
		 */
		cb->aio_iov = iov;
		cb->aio_iovcnt = iovcnt;
		return lio_opcode == LIO_WRITE ? aio_writev(cb) : aio_readv(cb);
	}
	else
#endif
	{
		Assert(iovcnt == 1);

		/*
		 * This might be a single PG IO, or a chain of reads into contiguous
		 * memory, so that it takes only a single iovec.  We'll batch it up
		 * with other such single iovec requests, if lio_listio(2) works the
		 * way we want it to on this platform.
		 */
		cb->aio_buf = iov[0].iov_base;
		cb->aio_nbytes = iov[0].iov_len;
		cb->aio_lio_opcode = lio_opcode;

#ifdef MISSING_LIO_LISTIO_NOTIFICATIONS
		/*
		 * macOS doesn't send nofications for individual aiocbs subnmitted with
		 * lio_listio(2).  We could use it anyway, and depend on a notification
		 * sent when the whole batch has completed, but that adds a lot of
		 * latency.  Let's just submit individual requests on that OS.
		 */
		return lio_opcode == LIO_WRITE ? aio_write(cb) : aio_read(cb);
#else
		return pgaio_posix_add_listio(lb, io);
#endif
	}
}

/* On systems with no O_DSYNC, just use the stronger O_SYNC. */
#ifdef O_DSYNC
#define PG_O_DSYNC O_DSYNC
#else
#define PG_O_DSYNC O_SYNC
#endif

/*
 * Queue up an IO request for the kernel in listio_buffer where possible, and
 * otherwise tell the kernel to initiate the IO immediately.
 *
 * Assumes that io->posix_aiocb has aio_sigevent already set.
 */
static int
pgaio_posix_submit_one(PgAioInProgress *io,
					   pgaio_posix_listio_buffer *listio_buffer)
{
	int rc = -1;

	switch (io->op)
	{
		case PGAIO_OP_READ:
			io->posix_aiocb.aio_fildes = io->op_data.read.fd;
			io->posix_aiocb.aio_offset = io->op_data.read.offset +
				io->op_data.read.already_done;
			rc = pgaio_posix_start_rw(io, listio_buffer, LIO_READ);
			break;
		case PGAIO_OP_WRITE:
			io->posix_aiocb.aio_fildes = io->op_data.write.fd;
			io->posix_aiocb.aio_offset = io->op_data.write.offset +
				io->op_data.write.already_done;
			rc = pgaio_posix_start_rw(io, listio_buffer, LIO_WRITE);
			break;
		case PGAIO_OP_FSYNC:
			io->posix_aiocb.aio_fildes = io->op_data.fsync.fd;
			rc = aio_fsync(io->op_data.fsync.datasync ? PG_O_DSYNC : O_SYNC,
						   &io->posix_aiocb);
			break;
		case PGAIO_OP_FLUSH_RANGE:
			/*
			 * This is supposed to represent Linux's sync_file_range(),
			 * which initiates writeback for only a certain range of a
			 * file.  Initiating fdatasync() seems close to the intended
			 * behavior.  XXX this is a bad idea
			 */
			io->posix_aiocb.aio_fildes = io->op_data.flush_range.fd;
			rc = aio_fsync(PG_O_DSYNC, &io->posix_aiocb);
			break;
		case PGAIO_OP_NOP:
			rc = 0;
			break;
		case PGAIO_OP_INVALID:
			rc = -1;
			errno = EOPNOTSUPP;
			break;
	}

	return rc;
}

int
pgaio_posix_submit(int max_submit, bool drain)
{
	PgAioInProgress *ios[PGAIO_SUBMIT_BATCH_SIZE];
	int nios = 0;
	pgaio_posix_listio_buffer listio_buffer = {0};

	pgaio_posix_presubmit();
	while (!dlist_is_empty(&my_aio->pending))
	{
		dlist_node *node;
		PgAioInProgress *io;
		int rc;

		if (nios == max_submit)
			break;

		node = dlist_pop_head_node(&my_aio->pending);
		io = dlist_container(PgAioInProgress, io_node, node);

		pgaio_io_prepare_submit(io, 0);

		pgaio_posix_preflight(io);

		my_aio->submissions_total_count++;

		rc = pgaio_posix_submit_one(io, &listio_buffer);

		/*
		 * If we got an error while submitting, then we can process the I/O
		 * immediately.  Errors reporting via lio_listio() batch submission
		 * have been handled already, so this is where we report errors for the
		 * other system calls.
		 */
		if (rc < 0)
		{
			pgaio_process_io_completion(io, -errno);
			pgaio_posix_postflight(io);
		}

		ios[nios] = io;
		++nios;
	}
	pgaio_posix_flush_listio(&listio_buffer);
	pgaio_posix_postsubmit();

	/* XXXX copied from uring submit */
	/*
	 * Others might have been waiting for this IO. Because it wasn't
	 * marked as in-flight until now, they might be waiting for the
	 * CV. Wake'em up.
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

void
pgaio_posix_io_retry(PgAioInProgress *io)
{
	pgaio_posix_listio_buffer listio_buffer = {0};
	int rc;

	/*
	 * XXX If the failure was EAGAIN, should we apply back pressure by waiting
	 * for one IO to complete?  We don't know what kind of limit we've hit,
	 * and this amounts to a busy retry loop.  Should we probe the kernel
	 * limits and try to avoid it?
	 */

	/* Close race with pgaio_posix_wait_one(). */
	if (io->submitter_id != my_aio_id)
	{
		/*
		 * AFIXME: this is broken, if somebody waits for the IO it'll signal
		 * being done!
		 */
		io->generation++;
		pg_write_barrier();
		SetLatch(&ProcGlobal->allProcs[io->submitter_id].procLatch);
	}

	pgaio_posix_preflight(io);

	WRITE_ONCE_F(io->flags) |= PGAIOIP_INFLIGHT;

	pgaio_posix_presubmit();
	rc = pgaio_posix_submit_one(io, &listio_buffer);
	if (rc < 0)
	{
		pgaio_process_io_completion(io, -errno);
		pgaio_posix_postflight(io);
	}
	pgaio_posix_flush_listio(&listio_buffer);
	pgaio_posix_postsubmit();

	ConditionVariableBroadcast(&io->cv);
}

void
pgaio_posix_wait_one(PgAioInProgress *io, uint64 ref_generation)
{
	bool submitted_by_me;

	/*
	 * Check if this process submitted the IO.  This is a dirty read of
	 * io->submitter_id, but we'll detect concurrent changes with a generation
	 * check and a latch.
	 */
	submitted_by_me = io->submitter_id == my_aio_id;

	/*
	 * If the PGAIO_POSIX_SIGNO handler runs between our checks and
	 * WaitLatch(), we need a race-free way to wake up and check the flags
	 * agaon.  So ask it to set our latch.
	 */
	if (submitted_by_me)
		pgaio_posix_set_latch_on_return = true;

	for (;;)
	{
		PgAioIPFlags flags;

		pgaio_drain(0,
					/* block = */ false,
					/* call_shared = */ true,
					/* call local = */ false);

		if (pgaio_io_recycled(io, ref_generation, &flags) ||
			!(flags & PGAIOIP_INFLIGHT))
			break;

		if (submitted_by_me)
		{
			/*
			 * XXX It might be nice to use aio_suspend() here, and not need the
			 * latch, but it's hard to coordinate with the signal handler.
			 * Behavior is undefined for aio_suspend() on an aiocb that has
			 * already returned.  Need to try to make that work again.
			 */
			WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, -1,
					  WAIT_EVENT_AIO_IO_COMPLETE_ONE_LOCAL);
			ResetLatch(MyLatch);
		}
		else
		{
			/*
			 * XXX Improve signalling here.
			 */
			ConditionVariableTimedSleep(&io->cv, 100,
										WAIT_EVENT_AIO_IO_COMPLETE_ONE);
		}
	}

	if (submitted_by_me)
		pgaio_posix_set_latch_on_return = false;

	CHECK_FOR_INTERRUPTS();
}
