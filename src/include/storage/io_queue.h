/*-------------------------------------------------------------------------
 *
 * io_queue.h
 *	  Mechanism for tracking many IOs
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/io_queue.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef IO_QUEUE_H
#define IO_QUEUE_H

#include "storage/bufmgr.h"

struct IOQueue;
typedef struct IOQueue IOQueue;

struct PgAioHandleRef;

extern IOQueue *io_queue_create(int depth, int flags);
extern void io_queue_track(IOQueue *ioq, const struct PgAioHandleRef *ior);
extern void io_queue_wait_one(IOQueue *ioq);
extern void io_queue_wait_all(IOQueue *ioq);
extern bool io_queue_is_empty(IOQueue *ioq);
extern void io_queue_reserve(IOQueue *ioq);
extern struct PgAioHandle *io_queue_get_io(IOQueue *ioq);
extern void io_queue_free(IOQueue *ioq);

#endif							/* IO_QUEUE_H */
