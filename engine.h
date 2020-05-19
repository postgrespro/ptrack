/*-------------------------------------------------------------------------
 *
 * engine.h
 *	  header for ptrack map for tracking updates of relation's pages
 *
 *
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


/* Working copy of ptrack.map */
#define PTRACK_MMAP_PATH "global/ptrack.map.mmap"
/* Persistent copy of ptrack.map to restore after crash */
#define PTRACK_PATH "global/ptrack.map"
/* Used for atomical crash-safe update of ptrack.map */
#define PTRACK_PATH_TMP "global/ptrack.map.tmp"

#define PTRACK_BUF_SIZE 1000

/* Ptrack magic bytes */
#define PTRACK_MAGIC "ptk"
#define PTRACK_MAGIC_SIZE 4

/*
 * Header of ptrack map.
 */
typedef struct PtrackMapHdr
{
	/*
	 * Three magic bytes (+ \0) to be sure, that we are reading ptrack.map
	 * with a right PtrackMapHdr strucutre.
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

/* TODO: check MAXALIGN usage below */
/* Number of elements in ptrack map (LSN array)  */
#define PtrackContentNblocks \
		((ptrack_map_size - offsetof(PtrackMapHdr, entries) - sizeof(pg_crc32c)) / sizeof(pg_atomic_uint64))

/* Actual size of the ptrack map, that we are able to fit into ptrack_map_size */
#define PtrackActualSize \
		(offsetof(PtrackMapHdr, entries) + PtrackContentNblocks * sizeof(pg_atomic_uint64) + sizeof(pg_crc32c))

/* CRC32 value offset in order to directly access it in the mmap'ed memory chunk */
#define PtrackCrcOffset (PtrackActualSize - sizeof(pg_crc32c))

/* Map block address 'bid' to map slot */
#define BID_HASH_FUNC(bid) \
		(size_t)(DatumGetUInt64(hash_any_extended((unsigned char *)&bid, sizeof(bid), 0)) % PtrackContentNblocks)

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
extern void ptrackMapAttach(void);

extern void assign_ptrack_map_size(int newval, void *extra);

extern void ptrack_walkdir(const char *path, Oid tablespaceOid, Oid dbOid);
extern void ptrack_mark_block(RelFileNodeBackend smgr_rnode,
							  ForkNumber forkno, BlockNumber blkno);

#endif							/* PTRACK_ENGINE_H */
