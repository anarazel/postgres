/*-------------------------------------------------------------------------
 *
 * aio_internal.h
 *    aio_internal
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_INTERNAL_H
#define AIO_INTERNAL_H


#include "lib/ilist.h"
#include "port/pg_iovec.h"
#include "storage/aio.h"
#include "storage/condition_variable.h"


#define PGAIO_VERBOSE


/* AFIXME */
#define PGAIO_SUBMIT_BATCH_SIZE 32



typedef enum PgAioHandleState
{
	/* not in use */
	AHS_IDLE = 0,

	/* returned by pgaio_io_get() */
	AHS_HANDED_OUT,

	/* pgaio_io_start_*() has been called, but IO hasn't been submitted yet */
	AHS_DEFINED,

	/* subjects prepare() callback has been called */
	AHS_PREPARED,

	/* IO is being executed */
	AHS_IN_FLIGHT,

	/* IO finished, but result has not yet been processed */
	AHS_REAPED,

	/* IO completed, shared completion has been called */
	AHS_COMPLETED_SHARED,

	/* IO completed, local completion has been called */
	AHS_COMPLETED_LOCAL,
} PgAioHandleState;


struct ResourceOwnerData;

/* typedef is in public header */
struct PgAioHandle
{
	PgAioHandleState state:8;

	/* what are we operating on */
	PgAioSubjectID subject:8;

	/* which operation */
	PgAioOp		op:8;

	/* bitfield of PgAioHandleFlags */
	uint8		flags;

	uint8		num_shared_callbacks;

	/* using the proper type here would use more space */
	uint8		shared_callbacks[AIO_MAX_SHARED_CALLBACKS];

	uint8		iovec_data_len;

	/* XXX: could be optimized out with some pointer math */
	int32		owner_procno;

	/* FIXME: remove in favor of distilled_result */
	/* raw result of the IO operation */
	int32		result;

	/* index into PgAioCtl->iovecs */
	uint32		iovec_off;

	/*
	 * List of bounce_buffers owned by IO. It would suffice to use an index
	 * based linked list here.
	 */
	slist_head	bounce_buffers;

	/**
	 * In which list the handle is registered, depends on the state:
	 * - IDLE, in per-backend list
	 * - HANDED_OUT - not in a list
	 * - DEFINED - in per-backend staged list
	 * - PREPARED - in per-backend staged list
	 * - IN_FLIGHT - not in any list
	 * - REAPED - in per-reap context list
	 * - COMPLETED_SHARED - not in any list
	 * - COMPLETED_LOCAL - not in any list
	 *
	 * XXX: It probably make sense to optimize this out to save on per-io
	 * memory at the cost of per-backend memory.
	 **/
	dlist_node	node;

	struct ResourceOwnerData *resowner;
	dlist_node	resowner_node;

	/* incremented every time the IO handle is reused */
	uint64		generation;

	ConditionVariable cv;

	/* result of shared callback, passed to issuer callback */
	PgAioResult distilled_result;

	PgAioReturn *report_return;

	PgAioOpData op_data;

	/*
	 * Data necessary for shared completions. Needs to be sufficient to allow
	 * another backend to retry an IO.
	 */
	PgAioSubjectData scb_data;
};


struct PgAioBounceBuffer
{
	slist_node	node;
	struct ResourceOwnerData *resowner;
	dlist_node	resowner_node;
	char	   *buffer;
};


typedef struct PgAioPerBackend
{
	/* index into PgAioCtl->io_handles */
	uint32		io_handle_off;

	/* index into PgAioCtl->bounce_buffers */
	uint32		bounce_buffers_off;

	/* IO Handles that currently are not used */
	dclist_head idle_ios;

	/*
	 * Only one IO may be returned by pgaio_io_get()/pgaio_io_get() without
	 * having been either defined (by actually associating it with IO) or by
	 * released (with pgaio_io_release()). This restriction is necessary to
	 * guarantee that we always can acquire an IO. ->handed_out_io is used to
	 * enforce that rule.
	 */
	PgAioHandle *handed_out_io;

	/*
	 * IOs that are defined, but not yet submitted.
	 */
	uint16		num_staged_ios;
	PgAioHandle *staged_ios[PGAIO_SUBMIT_BATCH_SIZE];

	/* Bounce Buffers that currently are not used */
	slist_head	idle_bbs;

	/* see handed_out_io */
	PgAioBounceBuffer *handed_out_bb;
} PgAioPerBackend;


typedef struct PgAioCtl
{
	int			backend_state_count;
	PgAioPerBackend *backend_state;

	/*
	 * Array of iovec structs. Each iovec is owned by a specific backend. The
	 * allocation is in PgAioCtl to allow the maximum number of iovecs for
	 * individual IOs to be configurable with PGC_POSTMASTER GUC.
	 */
	uint64		iovec_count;
	struct iovec *iovecs;

	/*
	 * For, e.g., an IO covering multiple buffers in shared / temp buffers, we
	 * need to get Buffer IDs during completion to be able to change the
	 * BufferDesc state accordingly. This space can be used to store e.g.
	 * Buffer IDs.  Note that the actual iovec might be shorter than this,
	 * because we combine neighboring pages into one larger iovec entry.
	 */
	uint64	   *iovecs_data;

	/*
	 * To perform AIO on buffers that are not located in shared memory (either
	 * because they are not in shared memory or because we need to operate on
	 * a copy, as e.g. the case for writes when checksums are in use)
	 */
	uint64		bounce_buffers_count;
	PgAioBounceBuffer *bounce_buffers;
	char	   *bounce_buffers_data;

	uint64		io_handle_count;
	PgAioHandle *io_handles;
} PgAioCtl;



/*
 * The set of callbacks that each IO method must implement.
 */
typedef struct IoMethodOps
{
	/* initialization */
	size_t		(*shmem_size) (void);
	void		(*shmem_init) (bool first_time);

	void		(*postmaster_init) (void);
	void		(*postmaster_child_init_local) (void);
	void		(*postmaster_child_init) (void);

	/* teardown */
	void		(*postmaster_before_child_exit) (void);

	/* handling of IOs */
	bool		(*needs_synchronous_execution) (PgAioHandle *ioh);
	int			(*submit) (uint16 num_staged_ios, PgAioHandle **staged_ios);

	void		(*wait_one) (PgAioHandle *ioh,
							 uint64 ref_generation);

	/* properties */
	bool		can_scatter_gather_direct;
	bool		can_scatter_gather_buffered;
} IoMethodOps;


extern bool pgaio_io_was_recycled(PgAioHandle *ioh, uint64 ref_generation, PgAioHandleState *state);

extern void pgaio_io_prepare_subject(PgAioHandle *ioh);
extern void pgaio_io_process_completion_subject(PgAioHandle *ioh);
extern void pgaio_io_process_completion(PgAioHandle *ioh, int result);
extern void pgaio_io_prepare_submit(PgAioHandle *ioh);

extern bool pgaio_io_needs_synchronous_execution(PgAioHandle *ioh);
extern void pgaio_io_perform_synchronously(PgAioHandle *ioh);

extern bool pgaio_io_can_reopen(PgAioHandle *ioh);
extern void pgaio_io_reopen(PgAioHandle *ioh);

extern const char *pgaio_io_get_subject_name(PgAioHandle *ioh);
extern const char *pgaio_io_get_op_name(PgAioHandle *ioh);
extern const char *pgaio_io_get_state_name(PgAioHandle *ioh);


/* Declarations for the tables of function pointers exposed by each IO method. */
extern const IoMethodOps pgaio_sync_ops;

extern const IoMethodOps *pgaio_impl;
extern PgAioCtl *aio_ctl;
extern PgAioPerBackend *my_aio;



#endif							/* AIO_INTERNAL_H */
