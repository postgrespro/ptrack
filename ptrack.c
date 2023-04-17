/*
 * ptrack.c
 *		Block level incremental backup engine
 *
 * Copyright (c) 2019-2022, Postgres Professional
 *
 * IDENTIFICATION
 *	  ptrack/ptrack.c
 *
 * INTERFACE ROUTINES (PostgreSQL side)
 *	  ptrackMapInit()          --- allocate new shared ptrack_map
 *	  assign_ptrack_map_size() --- ptrack_map_size GUC assign callback
 *	  ptrack_walkdir()         --- walk directory and mark all blocks of all
 *	                               data files in ptrack_map
 *	  ptrack_mark_block()      --- mark single page in ptrack_map
 *
 * Currently ptrack has following public API methods:
 *
 * # ptrack_version                  --- returns ptrack version string (2.4 currently).
 * # ptrack_get_pagemapset('LSN')    --- returns a set of changed data files with
 * 										 bitmaps of changed blocks since specified LSN.
 * # ptrack_init_lsn                 --- returns LSN of the last ptrack map initialization.
 *
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#if PG_VERSION_NUM < 120000
#include "access/htup_details.h"
#endif
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#ifdef PGPRO_EE
/* For file_is_in_cfs_tablespace() only. */
#include "common/cfs_common.h"
#endif
#include "port/pg_crc32c.h"
#include "storage/copydir.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#if PG_VERSION_NUM >= 120000
#include "storage/md.h"
#endif
#include "storage/smgr.h"
#include "storage/reinit.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"

#include "datapagemap.h"
#include "ptrack.h"
#include "engine.h"

PG_MODULE_MAGIC;

PtrackMap	ptrack_map = NULL;
uint64		ptrack_map_size = 0;
int			ptrack_map_size_tmp;

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static copydir_hook_type prev_copydir_hook = NULL;
static mdwrite_hook_type prev_mdwrite_hook = NULL;
static mdextend_hook_type prev_mdextend_hook = NULL;
static ProcessSyncRequests_hook_type prev_ProcessSyncRequests_hook = NULL;

void		_PG_init(void);
void		_PG_fini(void);

static void ptrack_shmem_startup_hook(void);
static void ptrack_copydir_hook(const char *path);
static void ptrack_mdwrite_hook(RelFileNodeBackend smgr_rnode,
								ForkNumber forkno, BlockNumber blkno);
static void ptrack_mdextend_hook(RelFileNodeBackend smgr_rnode,
								 ForkNumber forkno, BlockNumber blkno);
static void ptrack_ProcessSyncRequests_hook(void);

static void ptrack_gather_filelist(List **filelist, char *path, Oid spcOid, Oid dbOid);
static int	ptrack_filelist_getnext(PtScanCtx * ctx);
#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static void ptrack_shmem_request(void);
#endif

/*
 * Module load callback
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		elog(ERROR, "ptrack module must be initialized by Postmaster. "
			 "Put the following line to configuration file: "
			 "shared_preload_libraries='ptrack'");

	/*
	 * Define (or redefine) custom GUC variables.
	 *
	 * XXX: for some reason assign_ptrack_map_size is called twice during the
	 * postmaster boot!  First, it is always called with bootValue, so we use
	 * -1 as default value and no-op here.  Next, it is called with the actual
	 * value from config.
	 */
	DefineCustomIntVariable("ptrack.map_size",
							"Sets the size of ptrack map in MB used for incremental backup (0 disabled).",
							NULL,
							&ptrack_map_size_tmp,
							0,
#if SIZEOF_SIZE_T == 8
							0, 32 * 1024, /* limit to 32 GB */
#else
							0, 256, /* limit to 256 MB */
#endif
							PGC_POSTMASTER,
							GUC_UNIT_MB,
							NULL,
							assign_ptrack_map_size,
							NULL);

	/* Request server shared memory */
	if (ptrack_map_size != 0)
	{
#if PG_VERSION_NUM >= 150000
		prev_shmem_request_hook = shmem_request_hook;
		shmem_request_hook = ptrack_shmem_request;
#else
		RequestAddinShmemSpace(PtrackActualSize);
#endif
	}
	else
		ptrackCleanFiles();

	/* Install hooks */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = ptrack_shmem_startup_hook;
	prev_copydir_hook = copydir_hook;
	copydir_hook = ptrack_copydir_hook;
	prev_mdwrite_hook = mdwrite_hook;
	mdwrite_hook = ptrack_mdwrite_hook;
	prev_mdextend_hook = mdextend_hook;
	mdextend_hook = ptrack_mdextend_hook;
	prev_ProcessSyncRequests_hook = ProcessSyncRequests_hook;
	ProcessSyncRequests_hook = ptrack_ProcessSyncRequests_hook;
}

#if PG_VERSION_NUM >= 150000
static void
ptrack_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(PtrackActualSize);
}
#endif

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks */
	shmem_startup_hook = prev_shmem_startup_hook;
	copydir_hook = prev_copydir_hook;
	mdwrite_hook = prev_mdwrite_hook;
	mdextend_hook = prev_mdextend_hook;
	ProcessSyncRequests_hook = prev_ProcessSyncRequests_hook;
}

/*
 * ptrack_shmem_startup hook: allocate or attach to shared memory.
 */
static void
ptrack_shmem_startup_hook(void)
{
	bool map_found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/*
	 * Create or attach to the shared memory state
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	if (ptrack_map_size != 0)
	{
		ptrack_map = ShmemInitStruct("ptrack map",
									PtrackActualSize,
									&map_found);
		if (!map_found)
		{
			ptrackMapInit();
			elog(DEBUG1, "Shared memory for ptrack is ready");
		}
	}
	else
	{
		ptrack_map = NULL;
	}

	LWLockRelease(AddinShmemInitLock);
}

/*
 * Ptrack follow up for copydir() routine.  It parses database OID
 * and tablespace OID from path string.  We do not need to recursively
 * walk subdirs here, copydir() will do it for us if needed.
 */
static void
ptrack_copydir_hook(const char *path)
{
	Oid			spcOid = InvalidOid;
	Oid			dbOid = InvalidOid;
	int			oidchars;
	char		oidbuf[OIDCHARS + 1];

	elog(DEBUG1, "ptrack_copydir_hook: path %s", path);

	if (strstr(path, "global/") == path)
		spcOid = GLOBALTABLESPACE_OID;
	else if (strstr(path, "base/") == path)
	{
		spcOid = DEFAULTTABLESPACE_OID;
		oidchars = strspn(path + 5, "0123456789");
		strncpy(oidbuf, path + 5, oidchars);
		oidbuf[oidchars] = '\0';
		dbOid = atooid(oidbuf);
	}
	else if (strstr(path, "pg_tblspc/") == path)
	{
		char	   *dbPos;

		oidchars = strspn(path + 10, "0123456789");
		strncpy(oidbuf, path + 10, oidchars);
		oidbuf[oidchars] = '\0';
		spcOid = atooid(oidbuf);

		dbPos = strstr(path, TABLESPACE_VERSION_DIRECTORY) + strlen(TABLESPACE_VERSION_DIRECTORY) + 1;
		oidchars = strspn(dbPos, "0123456789");
		strncpy(oidbuf, dbPos, oidchars);
		oidbuf[oidchars] = '\0';
		dbOid = atooid(oidbuf);
	}

	elog(DEBUG1, "ptrack_copydir_hook: spcOid %u, dbOid %u", spcOid, dbOid);

	ptrack_walkdir(path, spcOid, dbOid);

	if (prev_copydir_hook)
		prev_copydir_hook(path);
}

static void
ptrack_mdwrite_hook(RelFileNodeBackend smgr_rnode,
					ForkNumber forknum, BlockNumber blocknum)
{
	ptrack_mark_block(smgr_rnode, forknum, blocknum);

	if (prev_mdwrite_hook)
		prev_mdwrite_hook(smgr_rnode, forknum, blocknum);
}

static void
ptrack_mdextend_hook(RelFileNodeBackend smgr_rnode,
					 ForkNumber forknum, BlockNumber blocknum)
{
	ptrack_mark_block(smgr_rnode, forknum, blocknum);

	if (prev_mdextend_hook)
		prev_mdextend_hook(smgr_rnode, forknum, blocknum);
}

static void
ptrack_ProcessSyncRequests_hook()
{
	ptrackCheckpoint();

	if (prev_ProcessSyncRequests_hook)
		prev_ProcessSyncRequests_hook();
}

/*
 * Recursively walk through the path and add all data files to filelist.
 */
static void
ptrack_gather_filelist(List **filelist, char *path, Oid spcOid, Oid dbOid)
{
	DIR		   *dir;
	struct dirent *de;
#if CFS_SUPPORT
	bool is_cfs;

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
			strcmp(de->d_name, "..") == 0 ||
			looks_like_temp_rel_name(de->d_name) ||
			is_cfm_file_path(de->d_name))
			continue;

		snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);

		sret = lstat(subpath, &fst);

		if (sret < 0)
		{
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("ptrack: could not stat file \"%s\": %m", subpath)));
			continue;
		}

		if (S_ISREG(fst.st_mode))
		{
			if (fst.st_size == 0)
			{
				elog(DEBUG3, "ptrack: skip empty file %s", subpath);

				/* But try the next one */
				continue;
			}

			/* Regular file inside database directory, otherwise skip it */
			if (dbOid != InvalidOid || spcOid == GLOBALTABLESPACE_OID)
			{
				int			oidchars;
				char		oidbuf[OIDCHARS + 1];
				char	   *segpath;
				PtrackFileList_i *pfl = palloc0(sizeof(PtrackFileList_i));

				/*
				 * Check that filename seems to be a regular relation file.
				 */
				if (!parse_filename_for_nontemp_relation(de->d_name, &oidchars, &pfl->forknum))
					continue;

				/* Parse segno */
				segpath = strstr(de->d_name, ".");
				pfl->segno = segpath != NULL ? atoi(segpath + 1) : 0;

				/* Fill the pfl in */
				memcpy(oidbuf, de->d_name, oidchars);
				oidbuf[oidchars] = '\0';
				nodeRel(pfl->relnode) = atooid(oidbuf);
				nodeDb(pfl->relnode) = dbOid;
				nodeSpc(pfl->relnode) = spcOid == InvalidOid ? DEFAULTTABLESPACE_OID : spcOid;
				pfl->path = GetRelationPath(dbOid, nodeSpc(pfl->relnode),
											nodeRel(pfl->relnode), InvalidBackendId, pfl->forknum);
#if CFS_SUPPORT
				pfl->is_cfs_compressed = is_cfs
					&& md_get_compressor_internal(pfl->relnode, InvalidBackendId, pfl->forknum) != 0;
#endif

				*filelist = lappend(*filelist, pfl);

				elog(DEBUG3, "ptrack: added file %s of rel %u to file list",
					 pfl->path, nodeRel(pfl->relnode));
			}
		}
		else if (S_ISDIR(fst.st_mode))
		{
			if (strspn(de->d_name + 1, "0123456789") == strlen(de->d_name + 1)
				&& dbOid == InvalidOid)
				ptrack_gather_filelist(filelist, subpath, spcOid, atooid(de->d_name));
			else if (spcOid != InvalidOid && strcmp(de->d_name, TABLESPACE_VERSION_DIRECTORY) == 0)
				ptrack_gather_filelist(filelist, subpath, spcOid, InvalidOid);
		}
		/* TODO: is it enough to properly check symlink support? */
#if !defined(WIN32) || (PG_VERSION_NUM >= 160000)
		else if (S_ISLNK(fst.st_mode))
#else
		else if (pgwin32_is_junction(subpath))
#endif
		{
			/*
			 * We expect that symlinks with only digits in the name to be
			 * tablespaces
			 */
			if (strspn(de->d_name + 1, "0123456789") == strlen(de->d_name + 1))
				ptrack_gather_filelist(filelist, subpath, atooid(de->d_name), InvalidOid);
		}
	}

	FreeDir(dir);				/* we ignore any error here */
}

static int
ptrack_filelist_getnext(PtScanCtx * ctx)
{
	PtrackFileList_i *pfl = NULL;
	ListCell   *cell;
	char	   *fullpath;
	struct stat fst;
	off_t       rel_st_size = 0;
	XLogRecPtr  maxlsn;
#if CFS_SUPPORT
	RelFileNodeBackend rnodebackend;
#endif

get_next:

	/* No more file in the list */
	if (list_length(ctx->filelist) == 0)
		return -1;

	/* Get first file from the head */
	cell = list_head(ctx->filelist);
	pfl = (PtrackFileList_i *) lfirst(cell);

	/* Remove this file from the list */
	ctx->filelist = list_delete_first(ctx->filelist);

	if (pfl->segno > 0)
	{
		Assert(pfl->forknum == MAIN_FORKNUM);
		fullpath = psprintf("%s/%s.%d", DataDir, pfl->path, pfl->segno);
		ctx->relpath = psprintf("%s.%d", pfl->path, pfl->segno);
	}
	else
	{
		fullpath = psprintf("%s/%s", DataDir, pfl->path);
		ctx->relpath = pfl->path;
	}

	nodeSpc(ctx->bid.relnode) = nodeSpc(pfl->relnode);
	nodeDb(ctx->bid.relnode) = nodeDb(pfl->relnode);
	nodeRel(ctx->bid.relnode) = nodeRel(pfl->relnode);
	ctx->bid.forknum = pfl->forknum;
	ctx->bid.blocknum = 0;

	if (stat(fullpath, &fst) != 0)
	{
		elog(WARNING, "ptrack: cannot stat file %s", fullpath);

		/* But try the next one */
		goto get_next;
	}

	if (fst.st_size == 0)
	{
		elog(DEBUG3, "ptrack: skip empty file %s", fullpath);

		/* But try the next one */
		goto get_next;
	}

	maxlsn = ptrack_read_file_maxlsn(pfl->relnode, pfl->forknum);

	if (maxlsn < ctx->lsn)
	{
		elog(DEBUG3, "ptrack: skip file %s: maxlsn is %X/%X, expected %X/%X",
				fullpath, (uint32) (maxlsn >> 32), (uint32) maxlsn,
				(uint32) (ctx->lsn >> 32), (uint32) ctx->lsn);

		/* Try the next one */
		goto get_next;
	}

#if CFS_SUPPORT
	nodeOf(rnodebackend) = ctx->bid.relnode;
	rnodebackend.backend = InvalidBackendId;

	if(pfl->is_cfs_compressed) {
		rel_st_size = get_cfs_relation_file_decompressed_size(rnodebackend, fullpath, pfl->forknum);

		// Could not open fullpath for some reason, trying the next file.
		if(rel_st_size == -1)
			goto get_next;
	} else
#endif
	rel_st_size = fst.st_size;

	if (pfl->segno > 0)
	{
		ctx->relsize = pfl->segno * RELSEG_SIZE + rel_st_size / BLCKSZ;
		ctx->bid.blocknum = pfl->segno * RELSEG_SIZE;
	}
	else
		/* Estimate relsize as size of first segment in blocks */
		ctx->relsize = rel_st_size / BLCKSZ;

	elog(DEBUG3, "ptrack: got file %s with size %u from the file list", pfl->path, ctx->relsize);

	return 0;
}

/*
 * Returns ptrack version currently in use.
 */
PG_FUNCTION_INFO_V1(ptrack_version);
Datum
ptrack_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(PTRACK_VERSION));
}

/*
 * Function to get last ptrack map initialization LSN.
 */
PG_FUNCTION_INFO_V1(ptrack_init_lsn);
Datum
ptrack_init_lsn(PG_FUNCTION_ARGS)
{
	if (ptrack_map != NULL)
	{
		XLogRecPtr	init_lsn = (XLogRecPtr) (pg_atomic_read_u32(&ptrack_map->init_lsn) << 16);

		PG_RETURN_LSN(init_lsn);
	}
	else
	{
		elog(WARNING, "ptrack is disabled");
		PG_RETURN_LSN(InvalidXLogRecPtr);
	}
}

/*
 * Return set of database blocks which were changed since specified LSN.
 * This function may return false positives (blocks that have not been updated).
 */
PG_FUNCTION_INFO_V1(ptrack_get_pagemapset);
Datum
ptrack_get_pagemapset(PG_FUNCTION_ARGS)
{
	PtScanCtx *ctx;
	FuncCallContext *funcctx;
	MemoryContext oldcontext;
	datapagemap_t pagemap;
	int64		pagecount = 0;
	char		gather_path[MAXPGPATH];
	uint32		init_lsn = InvalidXLogRecPtr;
	bool		within_ptrack_map = true;

	/* Exit immediately if there is no map */
	if (ptrack_map == NULL)
		elog(ERROR, "ptrack is disabled");

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		XLogRecPtr	lsn = PG_GETARG_LSN(0);

		funcctx = SRF_FIRSTCALL_INIT();

		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		ctx = (PtScanCtx *) palloc0(sizeof(PtScanCtx));
		ctx->lsn = (uint32)(lsn >> 16);
		ctx->filelist = NIL;

		/* Make tuple descriptor */
#if PG_VERSION_NUM >= 120000
		tupdesc = CreateTemplateTupleDesc(3);
#else
		tupdesc = CreateTemplateTupleDesc(3, false);
#endif
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "path", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "pagecount", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "pagemap", BYTEAOID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		funcctx->user_fctx = ctx;

		/*
		 * Form a list of all data files inside global, base and pg_tblspc.
		 *
		 * TODO: refactor it to do not form a list, but use iterator instead,
		 * e.g. just ptrack_filelist_getnext(ctx).
		 */
		sprintf(gather_path, "%s/%s", DataDir, "global");
		ptrack_gather_filelist(&ctx->filelist, gather_path, GLOBALTABLESPACE_OID, InvalidOid);

		sprintf(gather_path, "%s/%s", DataDir, "base");
		ptrack_gather_filelist(&ctx->filelist, gather_path, InvalidOid, InvalidOid);

		sprintf(gather_path, "%s/%s", DataDir, "pg_tblspc");
		ptrack_gather_filelist(&ctx->filelist, gather_path, InvalidOid, InvalidOid);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	ctx = (PtScanCtx *) funcctx->user_fctx;

	/* Initialize bitmap */
	pagemap.bitmap = NULL;
	pagemap.bitmapsize = 0;

	/* Take next file from the list */
	if (ptrack_filelist_getnext(ctx) < 0)
		SRF_RETURN_DONE(funcctx);

	while (true)
	{
		uint64		hash;
		size_t		slot1;
		size_t		slot2;
		uint32		update_lsn1;
		uint32		update_lsn2;

		/* Stop traversal if there are no more segments */
		if (ctx->bid.blocknum >= ctx->relsize)
		{
			/* We completed a segment and there is a bitmap to return */
			if (pagemap.bitmap != NULL)
			{
				Datum		values[3];
				bool		nulls[3] = {false};
				char		pathname[MAXPGPATH];
				bytea	   *result = NULL;
				Size		result_sz = pagemap.bitmapsize + VARHDRSZ;
				HeapTuple	htup = NULL;

				/* Create a bytea copy of our bitmap */
				result = (bytea *) palloc(result_sz);
				SET_VARSIZE(result, result_sz);
				memcpy(VARDATA(result), pagemap.bitmap, pagemap.bitmapsize);

				strcpy(pathname, ctx->relpath);

				values[0] = CStringGetTextDatum(pathname);
				values[1] = Int64GetDatum(pagecount);
				values[2] = PointerGetDatum(result);

				pfree(pagemap.bitmap);
				pagemap.bitmap = NULL;
				pagemap.bitmapsize = 0;
				pagecount = 0;

				htup = heap_form_tuple(funcctx->tuple_desc, values, nulls);
				if (htup)
					SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(htup));
			}

			if (ptrack_filelist_getnext(ctx) < 0)
				SRF_RETURN_DONE(funcctx);
		}

		init_lsn = pg_atomic_read_u32(&ptrack_map->init_lsn);
		hash = BID_HASH_FUNC(ctx->bid);
		slot1 = (size_t)(hash % PtrackContentNblocks);

		update_lsn1 = pg_atomic_read_u32(&ptrack_map->entries[slot1]);

		if (update_lsn1 != InvalidXLogRecPtr)
			elog(DEBUG3, "ptrack: update_lsn1 %X/%X of blckno %u of file %s",
				 (uint16) (update_lsn1 >> 16), (uint16) update_lsn1,
				 ctx->bid.blocknum, ctx->relpath);

		if (init_lsn != InvalidXLogRecPtr)
			within_ptrack_map = lsn_diff(init_lsn, update_lsn1) <= 0;

		/* Only probe the second slot if the first one is marked */
		if (within_ptrack_map && lsn_diff(ctx->lsn, update_lsn1) <= 0)
		{
			slot2 = (size_t)(((hash << 32) | (hash >> 32)) % PtrackContentNblocks);
			update_lsn2 = pg_atomic_read_u32(&ptrack_map->entries[slot2]);

			if (update_lsn2 != InvalidXLogRecPtr)
				elog(DEBUG3, "ptrack: update_lsn2 %X/%X of blckno %u of file %s",
					 (uint16) (update_lsn1 >> 16), (uint16) update_lsn2,
					 ctx->bid.blocknum, ctx->relpath);

			if (init_lsn != InvalidXLogRecPtr)
				within_ptrack_map = lsn_diff(init_lsn, update_lsn2) <= 0;

			/* Block has been changed since specified LSN.  Mark it in the bitmap */
			if (within_ptrack_map && lsn_diff(ctx->lsn, update_lsn2) <= 0)
			{
				pagecount += 1;
				datapagemap_add(&pagemap, ctx->bid.blocknum % ((BlockNumber) RELSEG_SIZE));
			}
		}

		ctx->bid.blocknum += 1;
	}
}
