/*-------------------------------------------------------------------------
 *
 * io_queue.h
 *	  Mechanism for tracking many IOs
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/io_queue.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef IO_QUEUE_H
#define IO_QUEUE_H

#include "storage/aio_types.h"

struct IOQueue;
typedef struct IOQueue IOQueue;

struct PgAioWaitRef;

extern IOQueue *io_queue_create(int depth, int flags);
extern void io_queue_track(IOQueue *ioq, const PgAioWaitRef *iow);
extern void io_queue_wait_one(IOQueue *ioq);
extern void io_queue_wait_all(IOQueue *ioq);
extern bool io_queue_is_empty(IOQueue *ioq);
extern void io_queue_reserve(IOQueue *ioq);
extern PgAioHandle *io_queue_acquire_io(IOQueue *ioq);
extern void io_queue_free(IOQueue *ioq);

#endif							/* IO_QUEUE_H */
