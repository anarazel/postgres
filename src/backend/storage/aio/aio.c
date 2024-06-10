/*-------------------------------------------------------------------------
 *
 * aio.c
 *    Asynchronous I/O subsytem.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/aio.h"


/* Options for io_method. */
const struct config_enum_entry io_method_options[] = {
	{"sync", IOMETHOD_SYNC, false},
	{NULL, 0, false}
};

int			io_method = DEFAULT_IO_METHOD;


void
assign_io_method(int newval, void *extra)
{
}
