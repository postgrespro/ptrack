/*
 * engine.c
 *		Block level incremental backup engine core
 *
 * Copyright (c) 2019-2022, Postgres Professional
 *
 * IDENTIFICATION
 *	  ptrack/engine.c
 *
 * INTERFACE ROUTINES (PostgreSQL side)
 *	  ptrackMapInit()          --- allocate new shared ptrack_map
 *	  ptrackCleanFiles()       --- remove ptrack files
 *	  assign_ptrack_map_size() --- ptrack_map_size GUC assign callback
 *	  ptrack_walkdir()         --- walk directory and mark all blocks of all
 *	                               data files in ptrack_map
 *	  ptrack_mark_block()      --- mark single page in ptrack_map
 *
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#ifndef WIN32
#include <sys/mman.h>
#endif

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xlog.h"
#if PG_VERSION_NUM >= 150000
#include "access/xlogrecovery.h"
#include "storage/fd.h"
#endif
#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "port/pg_crc32c.h"
#ifdef PGPRO_EE
/* For file_is_in_cfs_tablespace() only. */
#include "common/cfs_common.h"
#endif
#include "storage/copydir.h"
#if PG_VERSION_NUM >= 120000
#include "storage/md.h"
#include "storage/sync.h"
#endif
#include "storage/reinit.h"
#include "storage/smgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

#include "ptrack.h"
#include "engine.h"

/*
 * Check that path is accessible by us and return true if it is
 * not a directory.
 */
static bool
ptrack_file_exists(const char *path)
{
	struct stat st;

	Assert(path != NULL);

	if (stat(path, &st) == 0)
		return S_ISDIR(st.st_mode) ? false : true;
	else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", path)));

	return false;
}

/*
 * Write a piece of ptrack map to file and update CRC32 value.
 */
static void
ptrack_write_chunk(int fd, pg_crc32c *crc, char *chunk, size_t size)
{
	COMP_CRC32C(*crc, (char *) chunk, size);

	if (write(fd, chunk, size) != size)
	{
		/* If write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", PTRACK_PATH_TMP)));
	}
}

/*
 * Determines whether given file path is a path to a cfm file.
 */
bool
is_cfm_file_path(const char *filepath) {
	ssize_t len = strlen(filepath);

	// For this length checks we assume that the filename is at least
	// 1 character longer than the corresponding extension ".cfm":
	// strlen(".cfm") == 4 therefore we assume that the filename can't be
	// shorter than 5 bytes, for example: "5.cfm".
	return strlen(filepath) >= 5 && strcmp(&filepath[len-4], ".cfm") == 0;
}

#if CFS_SUPPORT
/*
 * Determines the relation file size specified by fullpath as if it
 * was not compressed.
 */
off_t
get_cfs_relation_file_decompressed_size(RelFileNodeBackend rnode, const char *fullpath, ForkNumber forknum) {
	File     fd;
	off_t    size;

#if PG_VERSION_NUM >= 120000
	int      compressor;
	compressor = md_get_compressor_internal(nodeOf(rnode), rnode.backend, forknum);
	fd = PathNameOpenFile(fullpath, O_RDWR | PG_BINARY, compressor);
#else
	fd = PathNameOpenFile(fullpath, O_RDWR | PG_BINARY | PG_COMPRESSION);
#endif

	if(fd < 0)
		return (off_t)-1;

#if PG_VERSION_NUM >= 120000
	size = FileSize(fd);
#else
	size = FileSeek(fd, 0, SEEK_END);

	if (size < 0)
		return (off_t) -1;
#endif

	FileClose(fd);

	return size;
}
#endif

/*
 * Delete ptrack files when ptrack is disabled.
 *
 * This is performed by postmaster at start,
 * so that there are no concurrent delete issues.
 */
void
ptrackCleanFiles(void)
{
	char		ptrack_path[MAXPGPATH];
	char		ptrack_path_tmp[MAXPGPATH];

	sprintf(ptrack_path, "%s/%s", DataDir, PTRACK_PATH);
	sprintf(ptrack_path_tmp, "%s/%s", DataDir, PTRACK_PATH_TMP);

	elog(DEBUG1, "ptrack: clean map files");

	if (ptrack_file_exists(ptrack_path_tmp))
		durable_unlink(ptrack_path_tmp, LOG);

	if (ptrack_file_exists(ptrack_path))
		durable_unlink(ptrack_path, LOG);
}

/*
 * Read ptrack map file into shared memory pointed by ptrack_map.
 * This function is called only at startup,
 * so data is read directly (without synchronization).
 */
static bool
ptrackMapReadFromFile(const char *ptrack_path)
{
	elog(DEBUG1, "ptrack read map");

	/* Do actual file read */
	{
		int			ptrack_fd;
		size_t		readed;

		ptrack_fd = BasicOpenFile(ptrack_path, O_RDWR | PG_BINARY);

		if (ptrack_fd < 0)
			elog(ERROR, "ptrack read map: failed to open map file \"%s\": %m", ptrack_path);

		readed = 0;
		do
		{
			ssize_t last_readed;

			/*
			 * Try to read as much as possible
			 * (linux guaranteed only 0x7ffff000 bytes in one read
			 * operation, see read(2))
			 */
			last_readed = read(ptrack_fd, (char *) ptrack_map + readed, PtrackActualSize - readed);

			if (last_readed > 0)
			{
				readed += last_readed;
			}
			else if (last_readed == 0)
			{
				/*
				 * We don't try to read more that PtrackActualSize and
				 * file size was already checked in ptrackMapInit()
				 */
				elog(ERROR, "ptrack read map: unexpected end of file while reading map file \"%s\", expected to read %zu, but read only %zu bytes",
							ptrack_path, (size_t)PtrackActualSize, readed);
			}
			else if (last_readed < 0 && errno != EINTR)
			{
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("ptrack read map: could not read map file \"%s\": %m", ptrack_path)));
				close(ptrack_fd);
				return false;
			}
		} while (readed < PtrackActualSize);

		close(ptrack_fd);
	}

	/* Check PTRACK_MAGIC */
	if (strcmp(ptrack_map->magic, PTRACK_MAGIC) != 0)
	{
		elog(WARNING, "ptrack read map: wrong map format of file \"%s\"", ptrack_path);
		return false;
	}

	/* Check ptrack version inside old ptrack map */
	if (ptrack_map->version_num != PTRACK_MAP_FILE_VERSION_NUM)
	{
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("ptrack read map: map format version %d in the file \"%s\" is incompatible with file format of extension %d",
						ptrack_map->version_num, ptrack_path, PTRACK_MAP_FILE_VERSION_NUM),
				 errdetail("Deleting file \"%s\" and reinitializing ptrack map.", ptrack_path)));
		return false;
	}

	/* Check CRC */
	{
		pg_crc32c	crc;
		pg_crc32c  *file_crc;

		INIT_CRC32C(crc);
		COMP_CRC32C(crc, (char *) ptrack_map, PtrackCrcOffset);
		FIN_CRC32C(crc);

		file_crc = (pg_crc32c *) ((char *) ptrack_map + PtrackCrcOffset);

		/*
		 * Read ptrack map values without atomics during initialization, since
		 * postmaster is the only user right now.
		 */
		elog(DEBUG1, "ptrack read map: crc %u, file_crc %u, init_lsn %X/%X",
			 crc, *file_crc, (uint32) (ptrack_map->init_lsn.value >> 32), (uint32) ptrack_map->init_lsn.value);

		if (!EQ_CRC32C(*file_crc, crc))
		{
			ereport(WARNING,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("ptrack read map: incorrect checksum of file \"%s\"", ptrack_path),
					 errdetail("Deleting file \"%s\" and reinitializing ptrack map.", ptrack_path)));
			return false;
		}
	}

	return true;
}

/*
 * Read PTRACK_PATH file into already allocated shared memory, check header and checksum
 * or create new file, if there was no PTRACK_PATH file on disk.
 */
void
ptrackMapInit(void)
{
	char		ptrack_path[MAXPGPATH];
	struct stat stat_buf;
	bool		is_new_map = true;

	elog(DEBUG1, "ptrack init");

	if (ptrack_map_size == 0)
		return;

	sprintf(ptrack_path, "%s/%s", DataDir, PTRACK_PATH);

	if (stat(ptrack_path, &stat_buf) == 0)
	{
		elog(DEBUG3, "ptrack init: map \"%s\" detected, trying to load", ptrack_path);
		if (stat_buf.st_size != PtrackActualSize)
		{
			elog(WARNING, "ptrack init: unexpected \"%s\" file size %zu != " UINT64_FORMAT ", deleting",
				 ptrack_path, (Size) stat_buf.st_size, PtrackActualSize);
			durable_unlink(ptrack_path, LOG);
		}
		else if (ptrackMapReadFromFile(ptrack_path))
		{
			is_new_map = false;
		}
		else
		{
			/*
			 * ptrackMapReadFromFile failed
			 * this can be crc mismatch, version mismatch and other errors
			 * We treat it as non fatal and create new map in memory,
			 * that will be written on disk on checkpoint
			 */
			elog(WARNING, "ptrack init: broken map file \"%s\", deleting",
				 ptrack_path);
			durable_unlink(ptrack_path, LOG);
		}
	}

	/*
	 * Initialyze memory for new map
	 */
	if (is_new_map)
	{
		memcpy(ptrack_map->magic, PTRACK_MAGIC, PTRACK_MAGIC_SIZE);
		ptrack_map->version_num = PTRACK_MAP_FILE_VERSION_NUM;
		ptrack_map->init_lsn.value = InvalidXLogRecPtr;
		/*
		 * Fill entries with InvalidXLogRecPtr
		 * (InvalidXLogRecPtr is actually 0)
		 */
		memset(ptrack_map->entries, 0, PtrackContentNblocks * sizeof(pg_atomic_uint64));
		/*
		 * Last part of memory representation of ptrack_map (crc) is actually unused
		 * so leave it as it is
		 */
	}
}

/*
 * Write content of ptrack_map to file.
 */
void
ptrackCheckpoint(void)
{
	int			ptrack_tmp_fd;
	pg_crc32c	crc;
	char		ptrack_path[MAXPGPATH];
	char		ptrack_path_tmp[MAXPGPATH];
	XLogRecPtr	init_lsn;
	pg_atomic_uint64 buf[PTRACK_BUF_SIZE];
	struct stat stat_buf;
	uint64		i = 0;
	uint64		j = 0;

	elog(DEBUG1, "ptrack checkpoint");

	/*
	 * Set the buffer to all zeros for sanity.  Otherwise, if atomics
	 * simulation via spinlocks is used (e.g. with --disable-atomics) we could
	 * write garbage into the sema field of pg_atomic_uint64, which will cause
	 * spinlocks to stuck after restart.
	 */
	MemSet(buf, 0, sizeof(buf));

	/* Delete ptrack_map and all related files, if ptrack was switched off */
	if (ptrack_map_size == 0)
	{
		return;
	}
	else if (ptrack_map == NULL)
		elog(ERROR, "ptrack checkpoint: map is not loaded at checkpoint time");

	sprintf(ptrack_path_tmp, "%s/%s", DataDir, PTRACK_PATH_TMP);
	sprintf(ptrack_path, "%s/%s", DataDir, PTRACK_PATH);

	elog(DEBUG1, "ptrack checkpoint: started");

	/* Map content is protected with CRC */
	INIT_CRC32C(crc);

	ptrack_tmp_fd = BasicOpenFile(ptrack_path_tmp,
								  O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY);

	if (ptrack_tmp_fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("ptrack checkpoint: could not create file \"%s\": %m", ptrack_path_tmp)));

	/*
	 * We are writing ptrack map values to file, but we want to simply map it
	 * into the memory with mmap after a crash/restart. That way, we have to
	 * write values taking into account all paddings/alignments.
	 *
	 * Write both magic and version_num at once.
	 */

	/*
	 * Previously we read from the field magic, now we read from the beginning
	 * of the structure PtrackMapHdr. Make sure nothing has changed since then.
	 */
	StaticAssertStmt(
		offsetof(PtrackMapHdr, magic) == 0,
		"old write format for PtrackMapHdr.magic and PtrackMapHdr.version_num "
		"is not upward-compatible");

	ptrack_write_chunk(ptrack_tmp_fd, &crc, (char *) ptrack_map,
					   offsetof(PtrackMapHdr, init_lsn));

	init_lsn = pg_atomic_read_u64(&ptrack_map->init_lsn);

	/* Set init_lsn during checkpoint if it is not set yet */
	if (init_lsn == InvalidXLogRecPtr)
	{
		XLogRecPtr	new_init_lsn;

		if (RecoveryInProgress())
			new_init_lsn = GetXLogReplayRecPtr(NULL);
		else
			new_init_lsn = GetXLogInsertRecPtr();

		pg_atomic_write_u64(&ptrack_map->init_lsn, new_init_lsn);
		init_lsn = new_init_lsn;
	}

	/* Put init_lsn in the same buffer */
	buf[j].value = init_lsn;
	j++;

	/*
	 * Iterate over ptrack map actual content and sync it to file.  It's
	 * essential to read each element atomically to avoid partial reads, since
	 * map can be updated concurrently without any lock.
	 */
	while (i < PtrackContentNblocks)
	{
		XLogRecPtr	lsn;

		/*
		 * We store LSN values as pg_atomic_uint64 in the ptrack map, but
		 * pg_atomic_read_u64() returns uint64.  That way, we have to put this
		 * lsn into the buffer array of pg_atomic_uint64's.  We are the only
		 * one who write into this buffer, so we do it without locks.
		 *
		 * TODO: is it safe and can we do any better?
		 */
		lsn = pg_atomic_read_u64(&ptrack_map->entries[i]);
		buf[j].value = lsn;

		i++;
		j++;

		if (j == PTRACK_BUF_SIZE)
		{
			size_t		writesz = sizeof(buf); /* Up to ~2 GB for buffer size seems
												* to be more than enough, so never
												* going to overflow. */

			/*
			 * We should not have any alignment issues here, since sizeof()
			 * takes into account all paddings for us.
			 */
			ptrack_write_chunk(ptrack_tmp_fd, &crc, (char *) buf, writesz);
			elog(DEBUG5, "ptrack checkpoint: i " UINT64_FORMAT ", j " UINT64_FORMAT ", writesz %zu PtrackContentNblocks " UINT64_FORMAT,
				 i, j, writesz, (uint64) PtrackContentNblocks);

			j = 0;
		}
	}

	/* Write if anything left */
	if ((i + 1) % PTRACK_BUF_SIZE != 0)
	{
		size_t		writesz = sizeof(pg_atomic_uint64) * j;

		ptrack_write_chunk(ptrack_tmp_fd, &crc, (char *) buf, writesz);
		elog(DEBUG5, "ptrack checkpoint: final i " UINT64_FORMAT ", j " UINT64_FORMAT ", writesz %zu PtrackContentNblocks " UINT64_FORMAT,
			 i, j, writesz, (uint64) PtrackContentNblocks);
	}

	FIN_CRC32C(crc);

	if (write(ptrack_tmp_fd, &crc, sizeof(crc)) != sizeof(crc))
	{
		/* If write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("ptrack checkpoint: could not write file \"%s\": %m", ptrack_path_tmp)));
	}

	if (pg_fsync(ptrack_tmp_fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("ptrack checkpoint: could not fsync file \"%s\": %m", ptrack_path_tmp)));

	if (close(ptrack_tmp_fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("ptrack checkpoint: could not close file \"%s\": %m", ptrack_path_tmp)));

	/* And finally replace old file with the new one */
	durable_rename(ptrack_path_tmp, ptrack_path, ERROR);

	/* Sanity check */
	if (stat(ptrack_path, &stat_buf) == 0 &&
		stat_buf.st_size != PtrackActualSize)
	{
		elog(ERROR, "ptrack checkpoint: stat_buf.st_size != ptrack_map_size %zu != " UINT64_FORMAT,
			 (Size) stat_buf.st_size, PtrackActualSize);
	}
	elog(DEBUG1, "ptrack checkpoint: completed");
}

void
assign_ptrack_map_size(int newval, void *extra)
{
	elog(DEBUG1, "assign_ptrack_map_size: MyProc %d newval %d ptrack_map_size " UINT64_FORMAT,
		 MyProcPid, newval, ptrack_map_size);

	/* Delete ptrack_map and all related files, if ptrack was switched off. */
	if (newval == 0)
	{
		ptrack_map_size = 0;
		return;
	}

	if (newval != 0 && !XLogIsNeeded())
		ereport(ERROR,
				(errmsg("assign_ptrack_map_size: cannot use ptrack if wal_level is minimal"),
				 errdetail("Set wal_level to \"replica\" or higher, or turn off ptrack with \"ptrack.map_size=0\"")));

	if (DataDir != NULL &&
		!IsBootstrapProcessingMode() &&
		!InitializingParallelWorker)
	{
		/* Cast to uint64 in order to avoid int32 overflow */
		ptrack_map_size = (uint64) 1024 * 1024 * newval;

		elog(DEBUG1, "assign_ptrack_map_size: ptrack_map_size set to " UINT64_FORMAT,
			 ptrack_map_size);
	}
}

/*
 * Mark all blocks of the file in ptrack_map.
 * For use in functions that copy directories bypassing buffer manager.
 */
static void
#if CFS_SUPPORT
ptrack_mark_file(Oid dbOid, Oid tablespaceOid,
				 const char *filepath, const char *filename, bool is_cfs)
#else
ptrack_mark_file(Oid dbOid, Oid tablespaceOid,
				 const char *filepath, const char *filename)
#endif
{
	RelFileNodeBackend rnode;
	ForkNumber	forknum;
	BlockNumber blkno,
				nblocks = 0;
	struct stat stat_buf;
	int			oidchars;
	char		oidbuf[OIDCHARS + 1];
#if CFS_SUPPORT
	off_t       rel_size;
#endif

	/* Do not track temporary relations */
	if (looks_like_temp_rel_name(filename))
		return;

	/* Mark of non-temporary relation */
	rnode.backend = InvalidBackendId;

	nodeDb(nodeOf(rnode)) = dbOid;
	nodeSpc(nodeOf(rnode)) = tablespaceOid;

	if (!parse_filename_for_nontemp_relation(filename, &oidchars, &forknum))
		return;

	memcpy(oidbuf, filename, oidchars);
	oidbuf[oidchars] = '\0';
	nodeRel(nodeOf(rnode)) = atooid(oidbuf);

#if CFS_SUPPORT
	// if current tablespace is cfs-compressed and md_get_compressor_internal
	// returns the type of the compressing algorithm for filepath, then it
	// needs to be de-compressed to obtain its size
	if(is_cfs && md_get_compressor_internal(nodeOf(rnode), rnode.backend, forknum) != 0) {
		rel_size = get_cfs_relation_file_decompressed_size(rnode, filepath, forknum);

		if(rel_size == (off_t)-1) {
			elog(WARNING, "ptrack: could not open cfs-compressed relation file: %s", filepath);
			return;
		}

		nblocks = rel_size / BLCKSZ;
	} else
#endif
	/* Compute number of blocks based on file size */
	if (stat(filepath, &stat_buf) == 0)
		nblocks = stat_buf.st_size / BLCKSZ;

	elog(DEBUG1, "ptrack_mark_file %s, nblocks %u rnode db %u spc %u rel %u, forknum %d",
		 filepath, nblocks, nodeDb(nodeOf(rnode)), nodeSpc(nodeOf(rnode)), nodeRel(nodeOf(rnode)), forknum);

	for (blkno = 0; blkno < nblocks; blkno++)
		ptrack_mark_block(rnode, forknum, blkno);
}

/*
 * Mark all files in the given directory in ptrack_map.
 * For use in functions that copy directories bypassing buffer manager.
 */
void
ptrack_walkdir(const char *path, Oid tablespaceOid, Oid dbOid)
{
	DIR		   *dir;
	struct dirent *de;
#if CFS_SUPPORT
	bool        is_cfs;
#endif

	/* Do not walk during bootstrap and if ptrack is disabled */
	if (ptrack_map_size == 0
		|| DataDir == NULL
		|| IsBootstrapProcessingMode()
		|| InitializingParallelWorker)
		return;

#if CFS_SUPPORT
	is_cfs = file_is_in_cfs_tablespace(path);
#endif

	dir = AllocateDir(path);

	while ((de = ReadDirExtended(dir, path, LOG)) != NULL)
	{
		char		subpath[MAXPGPATH * 2];
		struct stat fst;
		int			sret;

		CHECK_FOR_INTERRUPTS();

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);

		sret = lstat(subpath, &fst);

		if (sret < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("ptrack_walkdir: could not stat file \"%s\": %m", subpath)));
			continue;
		}

		if (S_ISREG(fst.st_mode))
#if CFS_SUPPORT
			ptrack_mark_file(dbOid, tablespaceOid, subpath, de->d_name, is_cfs);
#else
			ptrack_mark_file(dbOid, tablespaceOid, subpath, de->d_name);
#endif
	}

	FreeDir(dir);				/* we ignore any error here */
}

/*
 * Mark modified block in ptrack_map.
 */
void
ptrack_mark_block(RelFileNodeBackend smgr_rnode,
				  ForkNumber forknum, BlockNumber blocknum)
{
	PtBlockId	bid;
	uint64		hash;
	size_t		slot1;
	size_t		slot2;
	XLogRecPtr	new_lsn;
	/*
	 * We use pg_atomic_uint64 here only for alignment purposes, because
	 * pg_atomic_uint64 is forcedly aligned on 8 bytes during the MSVC build.
	 */
	pg_atomic_uint64	old_lsn;
	pg_atomic_uint64	old_init_lsn;

	if (ptrack_map_size == 0
		|| ptrack_map == NULL
		|| smgr_rnode.backend != InvalidBackendId) /* do not track temporary
													* relations */
		return;

	bid.relnode = nodeOf(smgr_rnode);
	bid.forknum = forknum;
	bid.blocknum = blocknum;

	hash = BID_HASH_FUNC(bid);
	slot1 = (size_t)(hash % PtrackContentNblocks);
	slot2 = (size_t)(((hash << 32) | (hash >> 32)) % PtrackContentNblocks);

	if (RecoveryInProgress())
		new_lsn = GetXLogReplayRecPtr(NULL);
	else
		new_lsn = GetXLogInsertRecPtr();

	/* Atomically assign new init LSN value */
	old_init_lsn.value = pg_atomic_read_u64(&ptrack_map->init_lsn);
	if (old_init_lsn.value == InvalidXLogRecPtr)
	{
		elog(DEBUG1, "ptrack_mark_block: init_lsn " UINT64_FORMAT " <- " UINT64_FORMAT, old_init_lsn.value, new_lsn);

		while (old_init_lsn.value < new_lsn &&
			   !pg_atomic_compare_exchange_u64(&ptrack_map->init_lsn, (uint64 *) &old_init_lsn.value, new_lsn));
	}

	/* Atomically assign new LSN value to the first slot */
	old_lsn.value = pg_atomic_read_u64(&ptrack_map->entries[slot1]);
	elog(DEBUG3, "ptrack_mark_block: map[%zu]=" UINT64_FORMAT " <- " UINT64_FORMAT, slot1, old_lsn.value, new_lsn);
	while (old_lsn.value < new_lsn &&
		   !pg_atomic_compare_exchange_u64(&ptrack_map->entries[slot1], (uint64 *) &old_lsn.value, new_lsn));

	/* And to the second */
	old_lsn.value = pg_atomic_read_u64(&ptrack_map->entries[slot2]);
	elog(DEBUG3, "ptrack_mark_block: map[%zu]=" UINT64_FORMAT " <- " UINT64_FORMAT, slot2, old_lsn.value, new_lsn);
	while (old_lsn.value < new_lsn &&
		   !pg_atomic_compare_exchange_u64(&ptrack_map->entries[slot2], (uint64 *) &old_lsn.value, new_lsn));
}
