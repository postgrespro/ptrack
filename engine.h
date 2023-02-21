/*-------------------------------------------------------------------------
 *
 * engine.h
 *	  header for ptrack map for tracking updates of relation's pages
 *
 *
 * Copyright (c) 2019-2022, Postgres Professional
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ptrack/engine.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PTRACK_ENGINE_H
#define PTRACK_ENGINE_H

/*  #include "access/xlogdefs.h" */
/*  #include "port/atomics.h" */
/*  #include "storage/block.h" */
/*  #include "storage/buf.h" */
/*  #include "storage/relfilenode.h" */
/*  #include "storage/smgr.h" */
/*  #include "utils/relcache.h" */
#include "access/hash.h"

/* Persistent copy of ptrack.map to restore after crash */
#define PTRACK_PATH "global/ptrack.map"
/* Used for atomical crash-safe update of ptrack.map */
#define PTRACK_PATH_TMP "global/ptrack.map.tmp"

/*
 * 8k of 64 bit LSNs is 64 KB, which looks like a reasonable
 * buffer size for disk writes.  On fast NVMe SSD it gives
 * around 20% increase in ptrack checkpoint speed compared
 * to PTRACK_BUF_SIZE == 1000, i.e. 8 KB writes.
 * (PTRACK_BUS_SIZE is a count of pg_atomic_uint64)
 *
 * NOTE: but POSIX defines _POSIX_SSIZE_MAX as 32767 (bytes)
 */
#define PTRACK_BUF_SIZE ((uint64) 8000)

/* Ptrack magic bytes */
#define PTRACK_MAGIC "ptk"
#define PTRACK_MAGIC_SIZE 4

/* CFS support macro */
#if defined(PGPRO_EE) && PG_VERSION_NUM >= 110000
#define CFS_SUPPORT 1
#endif

/*
 * Header of ptrack map.
 */
typedef struct PtrackMapHdr
{
	/*
	 * Three magic bytes (+ \0) to be sure, that we are reading ptrack.map
	 * with a right PtrackMapHdr structure.
	 */
	char		magic[PTRACK_MAGIC_SIZE];

	/*
	 * Value of PTRACK_VERSION_NUM at the time of map initialization.
	 */
	uint32		version_num;

	/* LSN of the moment, when map was last enabled. */
	pg_atomic_uint64 init_lsn;

	/* Followed by the actual map of LSNs */
	pg_atomic_uint64 entries[FLEXIBLE_ARRAY_MEMBER];

	/*
	 * At the end of the map CRC of type pg_crc32c is stored.
	 */
}			PtrackMapHdr;

typedef PtrackMapHdr * PtrackMap;

/* Number of elements in ptrack map (LSN array)  */
#define PtrackContentNblocks \
		((ptrack_map_size - offsetof(PtrackMapHdr, entries) - sizeof(pg_crc32c)) / sizeof(pg_atomic_uint64))

/* Actual size of the ptrack map, that we are able to fit into ptrack_map_size */
#define PtrackActualSize \
		(offsetof(PtrackMapHdr, entries) + PtrackContentNblocks * sizeof(pg_atomic_uint64) + sizeof(pg_crc32c))

/* CRC32 value offset in order to directly access it in the shared memory chunk */
#define PtrackCrcOffset (PtrackActualSize - sizeof(pg_crc32c))

/* Block address 'bid' to hash.  To get slot position in map should be divided
 * with '% PtrackContentNblocks' */
#define BID_HASH_FUNC(bid) \
		(DatumGetUInt64(hash_any_extended((unsigned char *)&bid, sizeof(bid), 0)))

/*
 * Per process pointer to shared ptrack_map
 */
extern PtrackMap ptrack_map;

/*
 * Size of ptrack map in bytes
 * TODO: to be protected by PtrackResizeLock?
 */
extern uint64 ptrack_map_size;
extern int	ptrack_map_size_tmp;

extern void ptrackCheckpoint(void);
extern void ptrackMapInit(void);
extern void ptrackCleanFiles(void);

extern void assign_ptrack_map_size(int newval, void *extra);

extern void ptrack_walkdir(const char *path, Oid tablespaceOid, Oid dbOid);
extern void ptrack_mark_block(RelFileNodeBackend smgr_rnode,
							  ForkNumber forkno, BlockNumber blkno);

extern bool is_cfm_file_path(const char *path);
#ifdef PGPRO_EE
extern off_t    get_cfs_relation_file_decompressed_size(RelFileNodeBackend rnode,
					const char *fullpath, ForkNumber forknum);
#endif

#endif							/* PTRACK_ENGINE_H */
