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


#include "utils/guc_tables.h"


/* GUC related */
extern void assign_io_method(int newval, void *extra);


/* Enum for io_method GUC. */
typedef enum IoMethod
{
	IOMETHOD_SYNC = 0,
} IoMethod;


/* We'll default to synchronous execution. */
#define DEFAULT_IO_METHOD IOMETHOD_SYNC


/* GUCs */
extern const struct config_enum_entry io_method_options[];
extern int	io_method;


#endif							/* AIO_H */
