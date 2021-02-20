/*-------------------------------------------------------------------------
 *
 * md.h
 *	  magnetic disk storage manager public interface declarations.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/md.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MD_H
#define MD_H

#include "storage/block.h"
#include "storage/relfilenode.h"
#include "storage/smgr.h"
#include "storage/sync.h"

/* md storage manager functionality */
extern void mdinit(void);
extern void mdopen(SMgrRelation reln);
extern void mdclose(SMgrRelation reln, ForkNumber forknum);
extern void mdrelease(void);
extern void mdcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern bool mdexists(SMgrRelation reln, ForkNumber forknum);
extern void mdunlink(RelFileNodeBackend rnode, ForkNumber forknum, bool isRedo);
extern void mdextend(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum, char *buffer, bool skipFsync);
extern BlockNumber mdzeroextend(SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum, int nblocks, bool skipFsync);
extern bool mdprefetch(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum);
extern void mdread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				   char *buffer);
extern void mdstartread(struct PgAioInProgress *,
						SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
						char *buffer);
extern void mdstartwrite(struct PgAioInProgress *,
						 SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
						 char *buffer, bool skipFsync);
extern void mdwrite(SMgrRelation reln, ForkNumber forknum,
					BlockNumber blocknum, char *buffer, bool skipFsync);
extern void mdwriteback(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber mdstartwriteback(struct PgAioInProgress *,
									SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber mdnblocks(SMgrRelation reln, ForkNumber forknum);
extern void mdtruncate(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber nblocks);
extern void mdimmedsync(SMgrRelation reln, ForkNumber forknum);
extern int mdfd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint32 *off);

extern void ForgetDatabaseSyncRequests(Oid dbid);
extern void DropRelationFiles(RelFileNode *delrels, int ndelrels, bool isRedo);

/* md sync callbacks */
extern void mdsyncfiletag(struct PgStreamingWrite *pgsw, InflightSyncEntry *entry);
extern int	mdunlinkfiletag(const FileTag *ftag, char *path);
extern bool mdfiletagmatches(const FileTag *ftag, const FileTag *candidate);

#endif							/* MD_H */
