/*-------------------------------------------------------------------------
 *
 * walsummarizer.h
 *
 * Header file for background WAL summarization process.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/include/postmaster/walsummarizer.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WALSUMMARIZER_H
#define WALSUMMARIZER_H

#include "access/xlogdefs.h"

extern bool summarize_wal;
extern int	wal_summary_keep_time;

extern Size WalSummarizerShmemSize(void);
extern void WalSummarizerShmemInit(void);
extern void WalSummarizerMain(void) pg_attribute_noreturn();

extern XLogRecPtr GetOldestUnsummarizedLSN(TimeLineID *tli,
										   bool *lsn_is_exact);
extern void SetWalSummarizerLatch(void);
extern XLogRecPtr WaitForWalSummarization(XLogRecPtr lsn, long timeout);

#endif
