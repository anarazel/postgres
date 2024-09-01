/*-------------------------------------------------------------------------
 *
 * aio.h
 *    Main AIO interface
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_H
#define AIO_H


#include "storage/aio_ref.h"
#include "storage/procnumber.h"
#include "utils/guc_tables.h"


typedef struct PgAioHandle PgAioHandle;

typedef enum PgAioOp
{
	/* intentionally the zero value, to help catch zeroed memory etc */
	PGAIO_OP_INVALID = 0,

	PGAIO_OP_READ,
	PGAIO_OP_WRITE,

	PGAIO_OP_FSYNC,

	PGAIO_OP_FLUSH_RANGE,

	PGAIO_OP_NOP,

	/**
	 * Eventually we'll additionally want at least:
	 * - send
	 * - recv
	 * - accept
	 **/
} PgAioOp;

#define PGAIO_OP_COUNT	(PGAIO_OP_NOP + 1)


/*
 * On what is IO being performed.
 *
 * PgAioSharedCallback specific behaviour should be implemented in
 * aio_subject.c.
 */
typedef enum PgAioSubjectID
{
	/* intentionally the zero value, to help catch zeroed memory etc */
	ASI_INVALID = 0,
	ASI_SMGR,
} PgAioSubjectID;

#define ASI_COUNT (ASI_SMGR + 1)

/*
 * Flags for an IO that can be set with pgaio_io_set_flag().
 */
typedef enum PgAioHandleFlags
{
	AHF_REFERENCES_LOCAL = 1 << 0,
} PgAioHandleFlags;


/*
 * IDs for callbacks that can be registered on an IO.
 *
 * Callbacks are identified by an ID rather than a function pointer. There are
 * two main reasons:

 * 1) Memory within PgAioHandle is precious, due to the number of PgAioHandle
 *    structs in pre-allocated shared memory.

 * 2) Due to EXEC_BACKEND function pointers are not necessarily stable between
 *    different backends, therefore function pointers cannot directly be in
 *    shared memory.
 *
 * Without 2), we could fairly easily allow to add new callbacks, by filling a
 * ID->pointer mapping table on demand. In the presence of 2 that's still
 * doable, but harder, because every process has to re-register the pointers
 * so that a local ID->"backend local pointer" mapping can be maintained.
 */
typedef enum PgAioHandleSharedCallbackID
{
	ASC_MD_READV,
	ASC_MD_WRITEV,

	ASC_SHARED_BUFFER_READ,
	ASC_SHARED_BUFFER_WRITE,

	ASC_LOCAL_BUFFER_READ,
	ASC_LOCAL_BUFFER_WRITE,
} PgAioHandleSharedCallbackID;


/*
 * Data necessary for basic IO types (PgAioOp).
 *
 * NB: Note that the FDs in here may *not* be relied upon for re-issuing
 * requests (e.g. for partial reads/writes) - the FD might be from another
 * process, or closed since. That's not a problem for IOs waiting to be issued
 * only because the queue is flushed when closing an FD.
 */
typedef union
{
	struct
	{
		int			fd;
		uint16		iov_length;
		uint64		offset;
	}			read;

	struct
	{
		int			fd;
		uint16		iov_length;
		uint64		offset;
	}			write;

	struct
	{
		int			fd;
		bool		datasync;
	}			fsync;

	struct
	{
		int			fd;
		uint32		nbytes;
		uint64		offset;
	}			flush_range;
} PgAioOpData;


/* XXX: Perhaps it's worth moving this to a dedicated file? */
#include "storage/block.h"
#include "storage/relfilelocator.h"

typedef union PgAioSubjectData
{
	struct
	{
		RelFileLocator rlocator;	/* physical relation identifier */
		BlockNumber blockNum;	/* blknum relative to begin of reln */
		int			nblocks;
		ForkNumber	forkNum:8;	/* don't waste 4 byte for four values */
		bool		is_temp;	/* proc can be inferred by owning AIO */
		bool		release_lock;
		int8		mode;
	}			smgr;

	/* just as an example placeholder for later */
	struct
	{
		uint32		queue_id;
	}			wal;
} PgAioSubjectData;



typedef enum PgAioResultStatus
{
	ARS_UNKNOWN,
	ARS_OK,
	ARS_PARTIAL,
	ARS_ERROR,
} PgAioResultStatus;

typedef struct PgAioResult
{
	PgAioHandleSharedCallbackID id:8;
	PgAioResultStatus status:2;
	uint32		error_data:22;
	int32		result;
} PgAioResult;

typedef struct PgAioReturn
{
	PgAioResult result;
	PgAioSubjectData subject_data;
} PgAioReturn;


typedef struct PgAioSubjectInfo
{
	void		(*reopen) (PgAioHandle *ioh);

#ifdef NOT_YET
	char	   *(*describe_identity) (PgAioHandle *ioh);
#endif

	const char *name;
} PgAioSubjectInfo;


typedef PgAioResult (*PgAioHandleSharedCallbackComplete) (PgAioHandle *ioh, PgAioResult prior_result);
typedef void (*PgAioHandleSharedCallbackPrepare) (PgAioHandle *ioh);
typedef void (*PgAioHandleSharedCallbackError) (PgAioResult result, const PgAioSubjectData *subject_data, int elevel);

typedef struct PgAioHandleSharedCallbacks
{
	PgAioHandleSharedCallbackPrepare prepare;
	PgAioHandleSharedCallbackComplete complete;
	PgAioHandleSharedCallbackError error;
} PgAioHandleSharedCallbacks;



typedef struct PgAioBounceBuffer PgAioBounceBuffer;


/*
 * How many callbacks can be registered for one IO handle. Currently we only
 * need two, but it's not hard to imagine needing a few more.
 */
#define AIO_MAX_SHARED_CALLBACKS	4



/* AIO API */


/* --------------------------------------------------------------------------------
 * IO Handles
 * --------------------------------------------------------------------------------
 */

struct ResourceOwnerData;
extern PgAioHandle *pgaio_io_get(struct ResourceOwnerData *resowner, PgAioReturn *ret);
extern PgAioHandle *pgaio_io_get_nb(struct ResourceOwnerData *resowner, PgAioReturn *ret);

extern void pgaio_io_release(PgAioHandle *ioh);
extern void pgaio_io_release_resowner(dlist_node *ioh_node, bool on_error);

extern void pgaio_io_get_ref(PgAioHandle *ioh, PgAioHandleRef *ior);

extern void pgaio_io_set_subject(PgAioHandle *ioh, PgAioSubjectID subjid);
extern void pgaio_io_set_flag(PgAioHandle *ioh, PgAioHandleFlags flag);

extern void pgaio_io_add_shared_cb(PgAioHandle *ioh, PgAioHandleSharedCallbackID cbid);

extern void pgaio_io_set_io_data_32(PgAioHandle *ioh, uint32 *data, uint8 len);
extern void pgaio_io_set_io_data_64(PgAioHandle *ioh, uint64 *data, uint8 len);
extern uint64 *pgaio_io_get_io_data(PgAioHandle *ioh, uint8 *len);

extern void pgaio_io_prepare(PgAioHandle *ioh, PgAioOp op);

extern int	pgaio_io_get_id(PgAioHandle *ioh);
struct iovec;
extern int	pgaio_io_get_iovec(PgAioHandle *ioh, struct iovec **iov);
extern bool pgaio_io_has_subject(PgAioHandle *ioh);

extern PgAioSubjectData *pgaio_io_get_subject_data(PgAioHandle *ioh);
extern PgAioOpData *pgaio_io_get_op_data(PgAioHandle *ioh);
extern ProcNumber pgaio_io_get_owner(PgAioHandle *ioh);



/* --------------------------------------------------------------------------------
 * IO References
 * --------------------------------------------------------------------------------
 */

extern void pgaio_io_ref_clear(PgAioHandleRef *ior);
extern bool pgaio_io_ref_valid(PgAioHandleRef *ior);
extern int	pgaio_io_ref_get_id(PgAioHandleRef *ior);


extern void pgaio_io_ref_wait(PgAioHandleRef *ior);
extern bool pgaio_io_ref_check_done(PgAioHandleRef *ior);



/* --------------------------------------------------------------------------------
 * IO Result
 * --------------------------------------------------------------------------------
 */

extern void pgaio_result_log(PgAioResult result, const PgAioSubjectData *subject_data,
							 int elevel);



/* --------------------------------------------------------------------------------
 * Bounce Buffers
 * --------------------------------------------------------------------------------
 */

extern PgAioBounceBuffer *pgaio_bounce_buffer_get(void);
extern void pgaio_io_assoc_bounce_buffer(PgAioHandle *ioh, PgAioBounceBuffer *bb);
extern uint32 pgaio_bounce_buffer_id(PgAioBounceBuffer *bb);
extern void pgaio_bounce_buffer_release(PgAioBounceBuffer *bb);
extern char *pgaio_bounce_buffer_buffer(PgAioBounceBuffer *bb);
extern void pgaio_bounce_buffer_release_resowner(dlist_node *bb_node, bool on_error);



/* --------------------------------------------------------------------------------
 * Actions on multiple IOs.
 * --------------------------------------------------------------------------------
 */

extern void pgaio_submit_staged(void);
extern bool pgaio_have_staged(void);



/* --------------------------------------------------------------------------------
 * Low level IO preparation routines
 *
 * These will often be called by code lowest level of initiating an
 * IO. E.g. bufmgr.c may initiate IO for a buffer, but pgaio_io_prep_readv()
 * will be called from within fd.c.
 *
 * Implemented in aio_io.c
 * --------------------------------------------------------------------------------
 */

extern void pgaio_io_prep_readv(PgAioHandle *ioh,
								int fd, int iovcnt, uint64 offset);

extern void pgaio_io_prep_writev(PgAioHandle *ioh,
								 int fd, int iovcnt, uint64 offset);



/* --------------------------------------------------------------------------------
 * Other
 * --------------------------------------------------------------------------------
 */

extern void pgaio_closing_fd(int fd);
extern void pgaio_at_xact_end(bool is_subxact, bool is_commit);
extern void pgaio_at_error(void);


/* GUC related */
extern void assign_io_method(int newval, void *extra);


/* Enum for io_method GUC. */
typedef enum IoMethod
{
	IOMETHOD_SYNC = 0,
	IOMETHOD_WORKER,
	IOMETHOD_IO_URING,
} IoMethod;


/* We'll default to bgworker. */
#define DEFAULT_IO_METHOD IOMETHOD_WORKER


/* GUCs */
extern const struct config_enum_entry io_method_options[];
extern int	io_method;
extern int	io_max_concurrency;
extern int	io_bounce_buffers;


#endif							/* AIO_H */
