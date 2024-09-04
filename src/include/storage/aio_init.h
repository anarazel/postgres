/*-------------------------------------------------------------------------
 *
 * aio_init.h
 *    AIO initialization - kept separate as initialization sites don't need to
 *    know about AIO itself and AIO users don't need to know about initialization.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio_init.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_INIT_H
#define AIO_INIT_H


extern Size AioShmemSize(void);
extern void AioShmemInit(void);

extern void pgaio_postmaster_init(void);
extern void pgaio_postmaster_child_init_local(void);
extern void pgaio_postmaster_child_init(void);

extern bool pgaio_workers_enabled(void);

#endif							/* AIO_INIT_H */
