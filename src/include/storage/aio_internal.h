/*-------------------------------------------------------------------------
 *
 * aio_internal.h
 *	  Types and function declarations that need to be visible to aio_XXX.c
 *	  implementations, but are not part of the public interface of the
 *	  aio.c module.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_INTERNAL_H
#define AIO_INTERNAL_H

#include "lib/ilist.h"
#include "port/atomics.h"
#include "port/pg_iovec.h"
#include "storage/aio.h"
#include "storage/condition_variable.h"
#include "storage/lwlock.h"

#ifdef USE_LIBURING
#include <liburing.h>		/* for struct io_uring */
#endif

#ifdef USE_POSIX_AIO
#include <aio.h>			/* for struct aiocb */
#endif


/*
 * FIXME: This is just so large because merging happens when submitting
 * pending requests, rather than when staging them.
 */
#define PGAIO_SUBMIT_BATCH_SIZE 256
#define PGAIO_MAX_LOCAL_REAPED 128
#define PGAIO_MAX_COMBINE 16

#define PGAIO_NUM_CONTEXTS 8

/*
 * The type of AIO.
 *
 * PgAioOp specific behaviour should be implemented in aio_io.c.
 *
 *
 * To keep PgAioInProgress smaller try to tell the compiler to only use the
 * minimal space. We could alternatively just use a uint8, but then we'd need
 * casts in more places...
 */
typedef enum pg_attribute_packed_desired() PgAioOp
{
	/* intentionally the zero value, to help catch zeroed memory etc */
	PGAIO_OP_INVALID = 0,

	PGAIO_OP_READ,
	PGAIO_OP_WRITE,

	PGAIO_OP_FSYNC,

	PGAIO_OP_FLUSH_RANGE,

	PGAIO_OP_NOP,
} PgAioOp;

/*
 * The type of shared callback used for an AIO.
 *
 * PgAioSharedCallback specific behaviour should be implemented in aio_scb.c.
 */
typedef enum pg_attribute_packed_desired() PgAioSharedCallback
{
	/* intentionally the zero value, to help catch zeroed memory etc */
	PGAIO_SCB_INVALID = 0,

	PGAIO_SCB_READ_SB,
	PGAIO_SCB_READ_SMGR,

	PGAIO_SCB_WRITE_SB,
	PGAIO_SCB_WRITE_SMGR,
	PGAIO_SCB_WRITE_WAL,
	PGAIO_SCB_WRITE_RAW,

	PGAIO_SCB_FSYNC_RAW,
	PGAIO_SCB_FSYNC_WAL,

	PGAIO_SCB_FLUSH_RANGE_RAW,
	PGAIO_SCB_FLUSH_RANGE_SMGR,

	PGAIO_SCB_NOP,
} PgAioSharedCallback;

typedef enum PgAioInProgressFlags
{
	/* request in the ->unused list */
	PGAIOIP_UNUSED = 1 << 0,

	/*  */
	PGAIOIP_IDLE = 1 << 1,

	/*
	 * pgaio_io_prepare() was called, but not yet pgaio_io_stage()
	 * /pgaio_io_unprepare().
	 */
	PGAIOIP_PREP = 1 << 2,

	/*  */
	PGAIOIP_IN_PROGRESS = 1 << 3,

	/* somewhere */
	PGAIOIP_PENDING = 1 << 4,

	/* request in kernel / io-method */
	PGAIOIP_INFLIGHT = 1 << 5,

	/* request reaped */
	PGAIOIP_REAPED = 1 << 6,

	/* shared completion callback was called */
	PGAIOIP_SHARED_CALLBACK_CALLED = 1 << 7,

	/* completed */
	PGAIOIP_DONE = 1 << 8,

	PGAIOIP_FOREIGN_DONE = 1 << 9,

	PGAIOIP_RETRY = 1 << 10,

	/* request failed completely */
	PGAIOIP_HARD_FAILURE = 1 << 11,

	/* request failed partly, e.g. a short write */
	PGAIOIP_SOFT_FAILURE = 1 << 12,

	/* local completion callback was called */
	PGAIOIP_LOCAL_CALLBACK_CALLED = 1 << 13,
} PgAioInProgressFlags;

typedef uint16 PgAioIPFlags;

/*
 * Ensure that flags is written once, rather than potentially multiple times
 * (e.g. once and'ing it, and once or'ing it).
 */
#define WRITE_ONCE_F(flags) *(volatile PgAioIPFlags*) &(flags)

/* typedef in header */
struct PgAioBounceBuffer
{
	pg_atomic_uint32 refcount;
	dlist_node node;
	char *buffer;
};

struct PgAioInProgress
{
	/* basic IO types supported by aio.c */
	PgAioOp op;

	/* shared callback for this IO, see io_action_cbs */
	PgAioSharedCallback scb;

	/* which AIO ring is this entry active for */
	uint8 ring;

	PgAioIPFlags flags;

	bool user_referenced;
	bool system_referenced;

	/* index into allProcs, or PG_UINT32_MAX for process local IO */
	uint32 owner_id;

	/* submitter can differ from owner on retry */
	uint32 submitter_id;

	/* the IOs result, depends on operation. E.g. the length of a read */
	int32 result;

	/*
	 * Single callback that can be registered on an IO to be called upon
	 * completion. Note that this is reset whenever an IO is recycled..
	 */
	PgAioOnCompletionLocalContext *on_completion_local;

	/*
	 * Membership in one of
	 * PgAioCtl->unused,
	 * PgAioPerBackend->unused,
	 * PgAioPerBackend->outstanding,
	 * PgAioPerBackend->issued,
	 */
	dlist_node owner_node;

	/*
	 * Membership in
	 * PgAioPerBackend->pending,
	 * PgAioPerBackend->reaped,
	 * local_recycle_requests
	 * PgAioPerBackend->foreign_completed,
	 * PgAioPerBackend->local_completed
	 */
	dlist_node io_node;

	dlist_node resowner_node;

	ConditionVariable cv;

	/* index into context->iovec, or -1 */
	int32 used_iovec;

	/*
	 * PGAIO_BB_INVALID, or index into aio_ctl->bounce_buffers
	 */
#define PGAIO_BB_INVALID PG_UINT32_MAX
	uint32 bb_idx;

	/*
	 * PGAIO_MERGE_INVALID if not merged, index into aio_ctl->in_progress_io
	 * otherwise.
	 */
#define PGAIO_MERGE_INVALID PG_UINT32_MAX
	uint32 merge_with_idx;

	uint64 generation;

	/*
	 * Data necessary for basic IO types (PgAioOp).
	 *
	 * NB: Note that fds in here may *not* be relied upon for re-issuing
	 * requests (e.g. for partial reads/writes) - the fd might be from another
	 * process, or closed since. That's not a problem for IOs waiting to be
	 * issued only because the queue is flushed when closing an fd.
	 */
	union {
		struct
		{
			int fd;
			uint32 nbytes;
			uint64 offset;
			uint32 already_done;
			void *bufdata;
		} read;

		struct
		{
			int fd;
			uint32 nbytes;
			uint64 offset;
			uint32 already_done;
			const void *bufdata;
		} write;

		struct
		{
			int fd;
			bool datasync;
		} fsync;

		struct
		{
			int fd;
			uint32 nbytes;
			uint64 offset;
		} flush_range;
	} op_data;

	union {
		struct
		{
			Buffer buffid;
			BackendId backend;
			int mode;
		} read_sb;

		struct
		{
			AioBufferTag tag;
		} read_smgr;

		struct
		{
			Buffer buffid;
			BackendId backend;
			bool release_lock;
		} write_sb;

		struct
		{
			AioBufferTag tag;
		} write_smgr;

		struct
		{
			uint32 write_no;
		} write_wal;

		struct
		{
			uint32 flush_no;
		} fsync_wal;

		struct
		{
			AioBufferTag tag;
		} flush_range_smgr;
	} scb_data;

	/*
	 * Data that is specific to different IO methods.
	 */
	union
	{
		struct
		{
			/*
			 * If worker mode ever needs any special data, it can go here.
			 * For now, dummy member to avoid an empty struct and union.
			 */
			int unused;
		} worker;
#ifdef USE_POSIX_AIO
		struct
		{
			struct aiocb iocb;
		} posix_aio;
#endif
	} io_method_data;
	union
	{
		struct
		{
			/* Index of head IO in merged chain of IOs, or self. */
			uint32 head_idx;

			/* Merged IO result from the kernel, or INT_MIN. */
			volatile int result;

			/* Atomic flag used to negotiate who processes the result. */
			pg_atomic_uint32 have_completer;
		} exchange;
	} interlock;
};

#ifdef USE_LIBURING
/*
 * An iovec that can represent the biggest possible iovec (due to combining)
 * we may need for a single IO submission.
 */
typedef struct PgAioIovec
{
	slist_node node;
	struct iovec iovec[PGAIO_MAX_COMBINE];
} PgAioIovec;
#endif

typedef struct PgAioContext
{
#ifdef USE_LIBURING
	LWLock submission_lock;
	LWLock completion_lock;

	struct io_uring io_uring_ring;

	/*
	 * For many versions of io_uring iovecs need to be in shared memory. The
	 * lists of available iovecs are split to be under the submission /
	 * completion locks - that allows to avoid additional lock acquisitions in
	 * the common cases.
	 */
	PgAioIovec *iovecs;

	/* locked by submission lock */
	slist_head unused_iovecs;
	uint32 unused_iovecs_count;

	/* locked by completion lock */
	slist_head reaped_iovecs;
	uint32 reaped_iovecs_count;
#else
	/* msvc doesn't like empty structs */
	int pro_forma_member;
#endif

	/* XXX: probably worth padding to a cacheline boundary here */
} PgAioContext;

#ifdef USE_LIBURING
typedef struct PgAioUringWaitRef
{
	PgAioInProgress *aio;
	struct PgAioContext *context;
	uint64 ref_generation;
} PgAioUringWaitRef;
#endif

/*
 * XXX: Really want a proclist like structure that works with integer
 * offsets. Given the limited number of IOs ever existing, using full pointers
 * is completely unnecessary.
 */

typedef struct PgAioPerBackend
{
	uint32 last_context;

	/*
	 * Local unused IOs. There's only a limited number of these. Used to
	 * reduce overhead of the central unused list.
	 *
	 * FIXME: Actually use.
	 *
	 * Could be singly linked list.
	 *
	 * PgAioInProgress->owner_node
	 */
	dlist_head unused;
	uint32 unused_count;

	/*
	 * IOs handed out to code within the backend.
	 *
	 * PgAioInProgress->owner_node
	 */
	dlist_head outstanding;
	uint32 outstanding_count;

	/*
	 * Requests waiting to be issued to the kernel. They are submitted to the
	 * kernel in batches, for efficiency (local merging of IOs, and better
	 * kernel side queue processing).
	 *
	 * Could be singly linked list.
	 *
	 * PgAioInProgress->io_node
	 */
	dlist_head pending;
	uint32 pending_count;

	/*
	 * Requests issued by backend that have not yet completed yet (but may be
	 * foreign_completed) and are still referenced by backend code (see
	 * issued_abandoned for those).
	 *
	 * PgAioInProgress->owner_node
	 */
	dlist_head issued;
	uint32 issued_count;

	/*
	 * Requests issued by backend that have not yet completed yet (but may be
	 * foreign_completed) and that are not referenced by backend code anymore (see
	 * issued for those).
	 *
	 * PgAioInProgress->owner_node
	 */
	dlist_head issued_abandoned;
	uint32 issued_abandoned_count;

	/*
	 * PgAioInProgress that are issued to the ringbuffer, and have not yet
	 * been processed (but they may have completed without the completions
	 * having been processed).
	 */
	pg_atomic_uint32 inflight_count;

	/*
	 * Requests where we've received a kernel completion, but haven't yet
	 * processed them.  This is needed to handle failing callbacks.
	 *
	 * Could be singly linked list.
	 *
	 * PgAioInProgress->io_node
	 */
	dlist_head reaped;

	/*
	 * IOs that were completed, but not yet recycled.
	 *
	 * PgAioInProgress->io_node
	 */
	dlist_head local_completed;
	uint32 local_completed_count;

	/*
	 * IOs where the completion was received in another backend.
	 *
	 * Could be singly linked list.
	 *
	 * PgAioInProgress->io_node
	 */
	slock_t foreign_completed_lock;
	uint32 foreign_completed_count;
	dlist_head foreign_completed;

	/*
	 * Stats.
	 */
	uint64 executed_total_count; /* un-merged */
	uint64 issued_total_count; /* merged */
	uint64 submissions_total_count; /* number of submission syscalls */
	uint64 foreign_completed_total_count;
	uint64 retry_total_count;

#ifdef USE_LIBURING
	PgAioUringWaitRef wr;
#endif
} PgAioPerBackend;

typedef struct PgAioCtl
{
	/* PgAioInProgress that are not used */
	dlist_head unused_ios;

	/*
	 * Number of PgAioInProgressIOs that are in use. This includes pending
	 * requests, as well as requests actually issues to the queue.
	 *
	 * Protected by SharedAIOCtlLock.
	 */
	uint32 used_count;

	/*
	 * Protected by SharedAIOCtlLock.
	 */
	dlist_head reaped_uncompleted;

	PgAioBounceBuffer *bounce_buffers;
	dlist_head unused_bounce_buffers;
	uint32 unused_bounce_buffers_count;

	int backend_state_count;
	PgAioPerBackend *backend_state;

	uint32 num_contexts;
	PgAioContext *contexts;

	PgAioInProgress in_progress_io[FLEXIBLE_ARRAY_MEMBER];
} PgAioCtl;

/*
 * The set of callbacks that each IO method must implement.
 */
typedef struct IoMethodOps
{
	size_t (*shmem_size)(void);
	void (*shmem_init)(bool first_time);

	void (*postmaster_child_init_local)(void);
	void (*postmaster_before_child_exit)(void);

	int (*submit)(int max_submit, bool drain);
	void (*retry)(PgAioInProgress *io);
	void (*wait_one)(PgAioContext *context,
					 PgAioInProgress *io,
					 uint64 ref_generation,
					 uint32 wait_event_info);
	int (*drain)(PgAioContext *context, bool block, bool call_shared);
	void (*drain_in_interrupt_handler)(void);

	void (*closing_fd)(int fd);

	bool can_scatter_gather_direct;
	bool can_scatter_gather_buffered;
} IoMethodOps;

/* Declarations for the tables of function pointers exposed by each IO method. */
extern const IoMethodOps pgaio_worker_ops;
#ifdef USE_LIBURING
extern const IoMethodOps pgaio_uring_ops;
#endif
#ifdef USE_POSIX_AIO
extern const IoMethodOps pgaio_posix_aio_ops;
#endif

/* global list of in-progress IO */
extern PgAioCtl *aio_ctl;

/* current backend's per-backend state */
extern PgAioPerBackend *my_aio;
extern int my_aio_id;



/* Declarations for functions in aio.c that are visible to aio_XXX.c. */
extern void pgaio_io_prepare_submit(PgAioInProgress *io, uint32 ring);
extern int pgaio_drain(PgAioContext *context, bool block, bool call_shared, bool call_local);
extern void pgaio_drain_in_interrupt_handler(void);
extern void pgaio_complete_ios(bool in_error);
extern void pgaio_broadcast_ios(PgAioInProgress **ios, int nios);
extern void pgaio_io_prepare(PgAioInProgress *io, PgAioOp op);
extern void pgaio_io_stage(PgAioInProgress *io, PgAioSharedCallback scb);
extern void pgaio_io_unprepare(PgAioInProgress *io, PgAioOp op);
extern void pgaio_io_flag_string(PgAioIPFlags flags, struct StringInfoData *s);
static inline bool pgaio_io_recycled(PgAioInProgress *io, uint64 ref_generation, PgAioIPFlags *flags);

extern bool pgaio_can_scatter_gather(void);

/* Declarations for aio_exchange.c */
extern void pgaio_exchange_shmem_init(bool first_time);
extern void pgaio_exchange_submit_one(PgAioInProgress *io);
extern void pgaio_exchange_process_completion(PgAioInProgress * io,
											  int raw_result,
											  bool in_interrupt_handler);
extern void pgaio_exchange_wait_one(PgAioContext *context,
									PgAioInProgress * io,
									uint64 ref_generation,
									uint32 wait_event_info);
extern void pgaio_exchange_disable_interrupt(void);
extern void pgaio_exchange_enable_interrupt(void);

/* Declarations for aio_io.c */
extern void pgaio_combine_pending(void);
extern void pgaio_process_io_completion(PgAioInProgress *io, int result);
extern int pgaio_fill_iov(struct iovec *iovs, const PgAioInProgress *io);
extern void pgaio_do_synchronously(PgAioInProgress *io);
extern const char *pgaio_io_operation_string(PgAioOp op);

/* Declarations for aio_scb.c */
extern bool pgaio_io_call_shared_complete(PgAioInProgress *io);
extern void pgaio_io_call_shared_retry(PgAioInProgress *io);
extern void pgaio_io_call_shared_open(PgAioInProgress *io);
extern void pgaio_io_call_shared_desc(PgAioInProgress *io, struct StringInfoData *s);
extern bool pgaio_io_has_shared_open(PgAioInProgress *io);
extern PgAioOp pgaio_shared_callback_op(PgAioSharedCallback scb);
extern const char * pgaio_io_shared_callback_string(PgAioSharedCallback a);


/*
 * Check if an IO has been recycled IO as indicated by ref_generation. Updates
 * *flags to the current flags.
 */
static inline bool
pgaio_io_recycled(PgAioInProgress *io, uint64 ref_generation, PgAioIPFlags *flags)
{
	/*
	 * Load the current flags before a the generation check. Combined with
	 * write barriers when increasing the generation that ensures that we'll
	 * detect the cases where we read the flags for an IO
	 */
	*flags = io->flags;

	pg_read_barrier();

	if (io->generation != ref_generation)
		return true;

	return false;
}

#endif
