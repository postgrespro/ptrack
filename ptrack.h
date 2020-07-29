/*-------------------------------------------------------------------------
 *
 * ptrack.h
 *	  header for ptrack map for tracking updates of relation's pages
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/ptrack.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PTRACK_H
#define PTRACK_H

#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/relfilenode.h"
#include "storage/smgr.h"
#include "utils/relcache.h"

/* Ptrack version as a string */
#define PTRACK_VERSION "2.1"
/* Ptrack version as a number */
#define PTRACK_VERSION_NUM 210

/*
 * Structure identifying block on the disk.
 */
typedef struct PtBlockId
{
	RelFileNode relnode;
	ForkNumber	forknum;
	BlockNumber blocknum;
}			PtBlockId;

/*
 * Context for ptrack_get_pagemapset set returning function.
 */
typedef struct PtScanCtx
{
	XLogRecPtr	lsn;
	PtBlockId	bid;
	uint32		relsize;
	char	   *relpath;
	List	   *filelist;
}			PtScanCtx;

/*
 * List item type for ptrack data files list.
 */
typedef struct PtrackFileList_i
{
	RelFileNode relnode;
	ForkNumber	forknum;
	int			segno;
	char	   *path;
}			PtrackFileList_i;

#endif							/* PTRACK_H */
