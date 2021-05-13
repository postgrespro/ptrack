/*
 * engine.c
 *		Block level incremental backup engine core
 *
 * Copyright (c) 2019-2020, Postgres Professional
 *
 * IDENTIFICATION
 *	  ptrack/engine.c
 *
 * INTERFACE ROUTINES (PostgreSQL side)
 *	  ptrackMapInit()          --- allocate new shared ptrack_map
 *	  ptrackMapAttach()        --- attach to the existing ptrack_map
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
#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "port/pg_crc32c.h"
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

	AssertArg(path != NULL);

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
 * Delete ptrack file and free the memory when ptrack is disabled.
 *
 * This is performed by postmaster at start or by checkpointer,
 * so that there are no concurrent delete issues.
 */
static void
ptrackCleanFilesAndMap(void)
{
	char		ptrack_path[MAXPGPATH];
	char		ptrack_mmap_path[MAXPGPATH];
	char		ptrack_path_tmp[MAXPGPATH];

	sprintf(ptrack_path, "%s/%s", DataDir, PTRACK_PATH);
	sprintf(ptrack_mmap_path, "%s/%s", DataDir, PTRACK_MMAP_PATH);
	sprintf(ptrack_path_tmp, "%s/%s", DataDir, PTRACK_PATH_TMP);

	elog(DEBUG1, "ptrack: clean files and map");

	if (ptrack_file_exists(ptrack_path_tmp))
		durable_unlink(ptrack_path_tmp, LOG);

	if (ptrack_file_exists(ptrack_path))
		durable_unlink(ptrack_path, LOG);

	if (ptrack_map != NULL)
	{
#ifdef WIN32
		if (!UnmapViewOfFile(ptrack_map))
#else
		if (!munmap(ptrack_map, sizeof(ptrack_map)))
#endif
			elog(LOG, "could not unmap ptrack_map");

		ptrack_map = NULL;
	}

	if (ptrack_file_exists(ptrack_mmap_path))
		durable_unlink(ptrack_mmap_path, LOG);
}

/*
 * Copy PTRACK_PATH file to special temporary file PTRACK_MMAP_PATH used for mapping,
 * or create new file, if there was no PTRACK_PATH file on disk.
 *
 * Map the content of PTRACK_MMAP_PATH file into memory structure 'ptrack_map' using mmap.
 */
void
ptrackMapInit(void)
{
	int			ptrack_fd;
	pg_crc32c	crc;
	pg_crc32c  *file_crc;
	char		ptrack_path[MAXPGPATH];
	char		ptrack_mmap_path[MAXPGPATH];
	struct stat stat_buf;
	bool		is_new_map = true;

	elog(DEBUG1, "ptrack init");

	/* We do it at server start, so the map must be not allocated yet. */
	Assert(ptrack_map == NULL);

	if (ptrack_map_size == 0)
		return;

	sprintf(ptrack_path, "%s/%s", DataDir, PTRACK_PATH);
	sprintf(ptrack_mmap_path, "%s/%s", DataDir, PTRACK_MMAP_PATH);

ptrack_map_reinit:

	/* Remove old PTRACK_MMAP_PATH file, if exists */
	if (ptrack_file_exists(ptrack_mmap_path))
		durable_unlink(ptrack_mmap_path, LOG);

	if (stat(ptrack_path, &stat_buf) == 0 &&
		stat_buf.st_size != PtrackActualSize)
	{
		elog(WARNING, "ptrack init: unexpected \"%s\" file size %zu != " UINT64_FORMAT ", deleting",
			 ptrack_path, (Size) stat_buf.st_size, PtrackActualSize);
		durable_unlink(ptrack_path, LOG);
	}

	/*
	 * If on-disk PTRACK_PATH file is present and has expected size, copy it
	 * to read and restore state.
	 */
	if (stat(ptrack_path, &stat_buf) == 0)
	{
		copy_file(ptrack_path, ptrack_mmap_path);
		is_new_map = false;		/* flag to check map file format and checksum */
		ptrack_fd = BasicOpenFile(ptrack_mmap_path, O_RDWR | PG_BINARY);
	}
	else
		/* Create new file for PTRACK_MMAP_PATH */
		ptrack_fd = BasicOpenFile(ptrack_mmap_path, O_RDWR | O_CREAT | PG_BINARY);

	if (ptrack_fd < 0)
		elog(ERROR, "ptrack init: failed to open map file \"%s\": %m", ptrack_mmap_path);

#ifdef WIN32
	{
		HANDLE		mh = CreateFileMapping((HANDLE) _get_osfhandle(ptrack_fd),
										   NULL,
										   PAGE_READWRITE,
										   0,
										   (DWORD) PtrackActualSize,
										   NULL);

		if (mh == NULL)
			elog(ERROR, "ptrack init: failed to create file mapping: %m");

		ptrack_map = (PtrackMap) MapViewOfFile(mh, FILE_MAP_ALL_ACCESS, 0, 0, 0);
		if (ptrack_map == NULL)
		{
			CloseHandle(mh);
			elog(ERROR, "ptrack init: failed to mmap ptrack file: %m");
		}
	}
#else
	if (ftruncate(ptrack_fd, PtrackActualSize) < 0)
		elog(ERROR, "ptrack init: failed to truncate file: %m");

	ptrack_map = (PtrackMap) mmap(NULL, PtrackActualSize,
								  PROT_READ | PROT_WRITE, MAP_SHARED,
								  ptrack_fd, 0);
	if (ptrack_map == MAP_FAILED)
		elog(ERROR, "ptrack init: failed to mmap file: %m");
#endif

	if (!is_new_map)
	{
		XLogRecPtr	init_lsn;

		/* Check PTRACK_MAGIC */
		if (strcmp(ptrack_map->magic, PTRACK_MAGIC) != 0)
			elog(ERROR, "ptrack init: wrong map format of file \"%s\"", ptrack_path);

		/* Check ptrack version inside old ptrack map */
		if (ptrack_map->version_num != PTRACK_VERSION_NUM)
		{
			ereport(WARNING,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("ptrack init: map format version %d in the file \"%s\" is incompatible with loaded version %d",
							ptrack_map->version_num, ptrack_path, PTRACK_VERSION_NUM),
					 errdetail("Deleting file \"%s\" and reinitializing ptrack map.", ptrack_path)));

			/* Clean up everything and try again */
			ptrackCleanFilesAndMap();

			is_new_map = true;
			goto ptrack_map_reinit;
		}

		/* Check CRC */
		INIT_CRC32C(crc);
		COMP_CRC32C(crc, (char *) ptrack_map, PtrackCrcOffset);
		FIN_CRC32C(crc);

		file_crc = (pg_crc32c *) ((char *) ptrack_map + PtrackCrcOffset);

		/*
		 * Read ptrack map values without atomics during initialization, since
		 * postmaster is the only user right now.
		 */
		init_lsn = ptrack_map->init_lsn.value;
		elog(DEBUG1, "ptrack init: crc %u, file_crc %u, init_lsn %X/%X",
			 crc, *file_crc, (uint32) (init_lsn >> 32), (uint32) init_lsn);

		/* TODO: Handle this error. Probably we can just recreate the file */
		if (!EQ_CRC32C(*file_crc, crc))
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("ptrack init: incorrect checksum of file \"%s\"", ptrack_path),
					 errhint("Delete \"%s\" and start the server again.", ptrack_path)));
		}
	}
	else
	{
		memcpy(ptrack_map->magic, PTRACK_MAGIC, PTRACK_MAGIC_SIZE);
		ptrack_map->version_num = PTRACK_VERSION_NUM;
	}

}

/*
 * Map must be already initialized by postmaster at start.
 * mmap working copy of ptrack_map.
 */
void
ptrackMapAttach(void)
{
	char		ptrack_mmap_path[MAXPGPATH];
	int			ptrack_fd;
	struct stat stat_buf;

	elog(DEBUG1, "ptrack attach");

	/* We do it at process start, so the map must be not allocated yet. */
	Assert(ptrack_map == NULL);

	if (ptrack_map_size == 0)
		return;

	sprintf(ptrack_mmap_path, "%s/%s", DataDir, PTRACK_MMAP_PATH);
	if (!ptrack_file_exists(ptrack_mmap_path))
	{
		elog(WARNING, "ptrack attach: '%s' file doesn't exist ", ptrack_mmap_path);
		return;
	}

	if (stat(ptrack_mmap_path, &stat_buf) == 0 &&
		stat_buf.st_size != PtrackActualSize)
		elog(ERROR, "ptrack attach: ptrack_map_size doesn't match size of the file \"%s\"", ptrack_mmap_path);

	ptrack_fd = BasicOpenFile(ptrack_mmap_path, O_RDWR | PG_BINARY);
	if (ptrack_fd < 0)
		elog(ERROR, "ptrack attach: failed to open ptrack map file \"%s\": %m", ptrack_mmap_path);

	elog(DEBUG1, "ptrack attach: before mmap");
#ifdef WIN32
	{
		HANDLE		mh = CreateFileMapping((HANDLE) _get_osfhandle(ptrack_fd),
										   NULL,
										   PAGE_READWRITE,
										   0,
										   (DWORD) PtrackActualSize,
										   NULL);

		if (mh == NULL)
			elog(ERROR, "ptrack attach: failed to create file mapping: %m");

		ptrack_map = (PtrackMap) MapViewOfFile(mh, FILE_MAP_ALL_ACCESS, 0, 0, 0);
		if (ptrack_map == NULL)
		{
			CloseHandle(mh);
			elog(ERROR, "ptrack attach: failed to mmap ptrack file: %m");
		}
	}
#else
	ptrack_map = (PtrackMap) mmap(NULL, PtrackActualSize,
								  PROT_READ | PROT_WRITE, MAP_SHARED,
								  ptrack_fd, 0);
	if (ptrack_map == MAP_FAILED)
		elog(ERROR, "ptrack attach: failed to mmap ptrack file: %m");
#endif
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
		ptrackCleanFilesAndMap();
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
	 * Write both magic and varsion_num at once.
	 */
	ptrack_write_chunk(ptrack_tmp_fd, &crc, (char *) &ptrack_map->magic,
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

	/*
	 * XXX: for some reason assign_ptrack_map_size is called twice during the
	 * postmaster boot!  First, it is always called with bootValue, so we use
	 * -1 as default value and no-op here.  Next, it is called with the actual
	 * value from config.  That way, we use 0 as an option for user to turn
	 * off ptrack and clean up all files.
	 */
	if (newval == -1)
		return;

	/* Delete ptrack_map and all related files, if ptrack was switched off. */
	if (newval == 0)
	{
		ptrackCleanFilesAndMap();
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

		/* Init map on postmaster start */
		if (!IsUnderPostmaster)
		{
			if (ptrack_map == NULL)
				ptrackMapInit();
		}
		else
			ptrackMapAttach();
	}
}

/*
 * Mark all blocks of the file in ptrack_map.
 * For use in functions that copy directories bypassing buffer manager.
 */
static void
ptrack_mark_file(Oid dbOid, Oid tablespaceOid,
				 const char *filepath, const char *filename)
{
	RelFileNodeBackend rnode;
	ForkNumber	forknum;
	BlockNumber blkno,
				nblocks = 0;
	struct stat stat_buf;
	int			oidchars;
	char		oidbuf[OIDCHARS + 1];

	/* Do not track temporary relations */
	if (looks_like_temp_rel_name(filename))
		return;

	/* Mark of non-temporary relation */
	rnode.backend = InvalidBackendId;

	rnode.node.dbNode = dbOid;
	rnode.node.spcNode = tablespaceOid;

	if (!parse_filename_for_nontemp_relation(filename, &oidchars, &forknum))
		return;

	memcpy(oidbuf, filename, oidchars);
	oidbuf[oidchars] = '\0';
	rnode.node.relNode = atooid(oidbuf);

	/* Compute number of blocks based on file size */
	if (stat(filepath, &stat_buf) == 0)
		nblocks = stat_buf.st_size / BLCKSZ;

	elog(DEBUG1, "ptrack_mark_file %s, nblocks %u rnode db %u spc %u rel %u, forknum %d",
		 filepath, nblocks, rnode.node.dbNode, rnode.node.spcNode, rnode.node.relNode, forknum);

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

	/* Do not walk during bootstrap and if ptrack is disabled */
	if (ptrack_map_size == 0
		|| DataDir == NULL
		|| IsBootstrapProcessingMode()
		|| InitializingParallelWorker)
		return;

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
			ptrack_mark_file(dbOid, tablespaceOid, subpath, de->d_name);
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
	size_t		hash;
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

	bid.relnode = smgr_rnode.node;
	bid.forknum = forknum;
	bid.blocknum = blocknum;

	hash = BID_HASH_FUNC(bid);
	slot1 = hash % PtrackContentNblocks;
	slot2 = ((hash << 32) | (hash >> 32)) % PtrackContentNblocks;

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
	elog(DEBUG3, "ptrack_mark_block: map[%zu]=" UINT64_FORMAT, hash, pg_atomic_read_u64(&ptrack_map->entries[slot1]));

	/* And to the second */
	old_lsn.value = pg_atomic_read_u64(&ptrack_map->entries[slot2]);
	while (old_lsn.value < new_lsn &&
		   !pg_atomic_compare_exchange_u64(&ptrack_map->entries[slot2], (uint64 *) &old_lsn.value, new_lsn));
}
