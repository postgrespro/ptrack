/*
 * ptrack.c
 *		Public API for in-core ptrack engine
 *
 * Copyright (c) 2019-2020, Postgres Professional
 *
 * IDENTIFICATION
 *	  contrib/ptrack/ptrack.c
 */

/*
 * #############################################################
 * #  _____ _______ _____            _____ _  __  ___    ___   #
 * # |  __ \__   __|  __ \     /\   / ____| |/ / |__ \  / _ \  #
 * # | |__) | | |  | |__) |   /  \ | |    | ' /     ) || | | | #
 * # |  ___/  | |  |  _  /   / /\ \| |    |  <     / / | | | | #
 * # | |      | |  | | \ \  / ____ \ |____| . \   / /_ | |_| | #
 * # |_|      |_|  |_|  \_\/_/    \_\_____|_|\_\ |____(_)___/  #
 * #############################################################
 *
 * Currently ptrack 2.0 has following public API methods:
 *
 * # ptrack_version                  --- returns ptrack version string (2.0 currently).
 * # pg_ptrack_get_pagemapset('LSN') --- returns a set of changed data files with
 * 										 bitmaps of changed blocks since specified LSN.
 * # pg_ptrack_control_lsn           --- returns LSN of the last ptrack map initialization.
 * # pg_ptrack_get_block             --- returns a spicific block of relation.
 *
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "funcapi.h"
#include "miscadmin.h"
#include "access/hash.h"
#include "access/skey.h"
#include "catalog/pg_type.h"
#include "catalog/pg_tablespace.h"
#include "storage/lmgr.h"
#include "storage/ptrack.h"
#include "storage/reinit.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "nodes/pg_list.h"

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

static void ptrack_gather_filelist(List **filelist, char *path, Oid spcOid, Oid dbOid);
static int ptrack_filelist_getnext(PtScanCtx *ctx);

/*
 * Module load callback
 */
void
_PG_init(void)
{

}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{

}

/********************************************************************/
/* Datapage bitmapping structures and routines taken from pg_rewind */
/* TODO: consider moving to another location */
struct datapagemap
{
	char	   *bitmap;
	int			bitmapsize;
};
typedef struct datapagemap datapagemap_t;

struct datapagemap_iterator
{
	datapagemap_t *map;
	BlockNumber nextblkno;
};
typedef struct datapagemap_iterator datapagemap_iterator_t;
static void datapagemap_add(datapagemap_t *map, BlockNumber blkno);

static void
datapagemap_add(datapagemap_t *map, BlockNumber blkno)
{
	int			offset;
	int			bitno;

	offset = blkno / 8;
	bitno = blkno % 8;

	/* enlarge or create bitmap if needed */
	if (map->bitmapsize <= offset)
	{
		int			oldsize = map->bitmapsize;
		int			newsize;

		/*
		 * The minimum to hold the new bit is offset + 1. But add some
		 * headroom, so that we don't need to repeatedly enlarge the bitmap in
		 * the common case that blocks are modified in order, from beginning
		 * of a relation to the end.
		 */
		newsize = offset + 1;
		newsize += 10;

		if (map->bitmap != NULL)
			map->bitmap = repalloc(map->bitmap, newsize);
		else
			map->bitmap = palloc(newsize);

		/* zero out the newly allocated region */
		memset(&map->bitmap[oldsize], 0, newsize - oldsize);

		map->bitmapsize = newsize;
	}

	/* Set the bit */
	map->bitmap[offset] |= (1 << bitno);
}
/********************************************************************/

/*
 * Recursively walk through the path and add all data files to filelist.
 */
static void
ptrack_gather_filelist(List **filelist, char *path, Oid spcOid, Oid dbOid)
{
	DIR			  *dir;
	struct dirent *de;

	dir = AllocateDir(path);

	while ((de = ReadDirExtended(dir, path, LOG)) != NULL)
	{
		char		subpath[MAXPGPATH * 2];
		struct stat fst;
		int			sret;

		CHECK_FOR_INTERRUPTS();

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0 ||
			looks_like_temp_rel_name(de->d_name))
			continue;

		snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);

		sret = lstat(subpath, &fst);

		if (sret < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", subpath)));
			continue;
		}

		if (S_ISREG(fst.st_mode))
		{
			/* Regular file inside database directory, otherwise skip it */
			if (dbOid != InvalidOid || spcOid == GLOBALTABLESPACE_OID)
			{
				int		oidchars;
				char	oidbuf[OIDCHARS + 1];
				char   *segpath;
				PtrackFileList_i *pfl = palloc0(sizeof(PtrackFileList_i));

				/*
				 * Check that filename seems to be a regular relation file.
				 */
				if (!parse_filename_for_nontemp_relation(de->d_name, &oidchars, &pfl->forknum))
					continue;

				/* Parse segno for main fork */
				if (pfl->forknum == MAIN_FORKNUM)
				{
					segpath = strstr(de->d_name, ".");
					pfl->segno = segpath != NULL ? atoi(segpath + 1) : 0;
				}
				else
					pfl->segno = 0;

				memcpy(oidbuf, de->d_name, oidchars);
				oidbuf[oidchars] = '\0';
				pfl->relnode.relNode = atooid(oidbuf);
				pfl->relnode.dbNode = dbOid;
				pfl->relnode.spcNode = spcOid == InvalidOid ? DEFAULTTABLESPACE_OID : spcOid;
				pfl->path = GetRelationPath(dbOid, pfl->relnode.spcNode,
											pfl->relnode.relNode, InvalidBackendId, pfl->forknum);

				*filelist = lappend(*filelist, pfl);
				// elog(WARNING, "added file %s of rel %u to ptrack list", pfl->path, pfl->relnode.relNode);
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
		// TODO: is it enough to properly check symlink support?
#ifndef WIN32
		else if (S_ISLNK(fst.st_mode))
#else
		else if (pgwin32_is_junction(subpath))
#endif
		{
			/* We expect that symlinks with only digits in the name to be tablespaces */
			if (strspn(de->d_name + 1, "0123456789") == strlen(de->d_name + 1))
				ptrack_gather_filelist(filelist, subpath, atooid(de->d_name), InvalidOid);
		}
	}

	FreeDir(dir); /* we ignore any error here */
}

static int
ptrack_filelist_getnext(PtScanCtx *ctx)
{
	PtrackFileList_i   *pfl = NULL;
	ListCell		   *cell;
	char			   *fullpath;
	struct stat			fst;

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

	ctx->bid.relnode.spcNode = pfl->relnode.spcNode;
	ctx->bid.relnode.dbNode = pfl->relnode.dbNode;
	ctx->bid.relnode.relNode = pfl->relnode.relNode;
	ctx->bid.forknum = pfl->forknum;
	ctx->bid.blocknum = 0;

	if (stat(fullpath, &fst) != 0)
	{
		elog(WARNING, "cannot stat file %s", fullpath);

		/* But try the next one */
		return ptrack_filelist_getnext(ctx);
	}

	if (pfl->segno > 0)
	{
		ctx->relsize = pfl->segno * RELSEG_SIZE + fst.st_size / BLCKSZ;
		ctx->bid.blocknum = pfl->segno * RELSEG_SIZE;
	}
	else
		/* Estimate relsize as size of first segment in blocks */
		ctx->relsize = fst.st_size / BLCKSZ;

	elog(DEBUG3, "got file %s with size %u from the ptrack list", pfl->path, ctx->relsize);

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
PG_FUNCTION_INFO_V1(pg_ptrack_control_lsn);
Datum
pg_ptrack_control_lsn(PG_FUNCTION_ARGS)
{
	if (ptrack_map != NULL)
		PG_RETURN_LSN(ptrack_map->init_lsn);
	else
	{
		elog(DEBUG1, "pg_ptrack_control_lsn(). no ptrack_map");
		PG_RETURN_LSN(InvalidXLogRecPtr);
	}
}

/*
 * Function to retrieve blocks via buffercache.
 */
PG_FUNCTION_INFO_V1(pg_ptrack_get_block);
Datum
pg_ptrack_get_block(PG_FUNCTION_ARGS)
{
	Oid			tablespace_oid = PG_GETARG_OID(0);
	Oid			db_oid = PG_GETARG_OID(1);
	Oid			relfilenode = PG_GETARG_OID(2);
	BlockNumber blkno = PG_GETARG_UINT32(3);
	bytea	   *raw_page;
	char	   *raw_page_data;
	Buffer		buf;
	RelFileNode rnode;
	BlockNumber nblocks;
	SMgrRelation smgr;

	rnode.dbNode = db_oid;
	rnode.spcNode = tablespace_oid;
	rnode.relNode = relfilenode;

	elog(DEBUG1, "pg_ptrack_get_block(%i, %i, %i, %u)",
		 tablespace_oid, db_oid, relfilenode, blkno);
	smgr = smgropen(rnode, InvalidBackendId);
	nblocks = smgrnblocks(smgr, MAIN_FORKNUM);

	if (blkno >= nblocks)
		PG_RETURN_NULL();

	/* Initialize buffer to copy to */
	raw_page = (bytea *) palloc0(BLCKSZ + VARHDRSZ);
	SET_VARSIZE(raw_page, BLCKSZ + VARHDRSZ);
	raw_page_data = VARDATA(raw_page);

	buf = ReadBufferWithoutRelcache(rnode, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);

	if (buf == InvalidBuffer)
		elog(ERROR, "Block is not found in the buffer cache");

	LockBuffer(buf, BUFFER_LOCK_SHARE);

	memcpy(raw_page_data, BufferGetPage(buf), BLCKSZ);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * Return set of database blocks which were changed since specified LSN.
 * This function may return false positives (blocks that have not been updated).
 */
PG_FUNCTION_INFO_V1(pg_ptrack_get_pagemapset);
Datum
pg_ptrack_get_pagemapset(PG_FUNCTION_ARGS)
{
	FuncCallContext	   *funcctx;
	PtScanCtx		   *ctx;
	MemoryContext		oldcontext;
	XLogRecPtr			update_lsn;
	datapagemap_t		pagemap;
	char				gather_path[MAXPGPATH];

	/* Exit immediately if there is no map */
	if (ptrack_map == NULL)
		elog(ERROR, "ptrack is disabled");

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		funcctx = SRF_FIRSTCALL_INIT();

		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		ctx = (PtScanCtx *) palloc0(sizeof(PtScanCtx));
		ctx->lsn = PG_GETARG_LSN(0);
		ctx->filelist = NIL;

		// get_call_result_type(fcinfo, NULL, &funcctx->tuple_desc);
		/* Make tuple descriptor */
		tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "path", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "pagemap", BYTEAOID, -1, 0);
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
		/* Stop traversal if there are no more segments */
		if (ctx->bid.blocknum > ctx->relsize)
		{
			/* We completed a segment and there is a bitmap to return */
			if (pagemap.bitmap != NULL)
			{
				Datum	values[2];
				bool	nulls[2] = {false};
				char	pathname[MAXPGPATH];
				bytea  *result = NULL;
				Size 	result_sz = pagemap.bitmapsize + VARHDRSZ;

				/* Create a bytea copy of our bitmap */
				result = (bytea *) palloc(result_sz);
				SET_VARSIZE(result, result_sz);
				memcpy(VARDATA(result), pagemap.bitmap, pagemap.bitmapsize);

				strcpy(pathname, ctx->relpath);

				values[0] = CStringGetTextDatum(pathname);
				values[1] = PointerGetDatum(result);

				pfree(pagemap.bitmap);
				pagemap.bitmap = NULL;
				pagemap.bitmapsize = 0;

				SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc, values, nulls)));
			}
			else
			{
				/* We have just processed unchanged file, let's pick next */
				if (ptrack_filelist_getnext(ctx) < 0)
					SRF_RETURN_DONE(funcctx);
			}
		}

		update_lsn = pg_atomic_read_u64(&PtrackContent(ptrack_map)[BID_HASH_FUNC(ctx->bid)]);

		if (update_lsn != InvalidXLogRecPtr)
			elog(DEBUG3, "update_lsn %X/%X of blckno %u of file %s",
				(uint32) (update_lsn >> 32), (uint32) update_lsn,
				ctx->bid.blocknum, ctx->relpath);

		/* Block has been changed since specified LSN. Mark it in the bitmap */
		if (update_lsn >= ctx->lsn)
			datapagemap_add(&pagemap, ctx->bid.blocknum % ((BlockNumber) RELSEG_SIZE));

		ctx->bid.blocknum += 1;
	}
}
