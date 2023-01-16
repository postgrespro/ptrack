/*-------------------------------------------------------------------------
 *
 * ptrack.h
 *	  header for ptrack map for tracking updates of relation's pages
 *
 *
 * Copyright (c) 2019-2022, Postgres Professional
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
#if PG_VERSION_NUM >= 160000
#include "storage/relfilelocator.h"
#else
#include "storage/relfilenode.h"
#endif
#include "storage/smgr.h"
#include "utils/relcache.h"

/* Ptrack version as a string */
#define PTRACK_VERSION "2.4"
/* Ptrack version as a number */
#define PTRACK_VERSION_NUM 240
/* Last ptrack version that changed map file format */
#define PTRACK_MAP_FILE_VERSION_NUM 220

#if PG_VERSION_NUM >= 160000
#define RelFileNode			RelFileLocator
#define RelFileNodeBackend	RelFileLocatorBackend
#define nodeDb(node)		(node).dbOid
#define nodeSpc(node)		(node).spcOid
#define nodeRel(node)		(node).relNumber
#define nodeOf(ndbck)		(ndbck).locator
#else
#define nodeDb(node)		(node).dbNode
#define nodeSpc(node)		(node).spcNode
#define nodeRel(node)		(node).relNode
#define nodeOf(ndbck)		(ndbck).node
#endif

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
#ifdef PGPRO_EE
	bool is_cfs_compressed;
#endif
}			PtrackFileList_i;

#endif							/* PTRACK_H */
