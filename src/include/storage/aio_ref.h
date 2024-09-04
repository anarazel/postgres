/*-------------------------------------------------------------------------
 *
 * aio_ref.h Definition of PgAioHandleRef, which sometimes needs to be used in
 *    headers.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio_ref.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_REF_H
#define AIO_REF_H

typedef struct PgAioHandleRef
{
	uint32		aio_index;
	uint32		generation_upper;
	uint32		generation_lower;
} PgAioHandleRef;

#endif							/* AIO_REF_H */
