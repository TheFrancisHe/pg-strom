/*
 * gstore_fdw.c
 *
 * On GPU column based data store as FDW provider.
 * ----
 * Copyright 2011-2017 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2017 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "postgres.h"
#include "access/reloptions.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "foreign/fdwapi.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/pg_crc.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "pg_strom.h"
#include "cuda_plcuda.h"

#define GPUSTORE_CHUNK_SIZE		(1UL << 30)

/*
 * GpuStoreChunk
 */
struct GpuStoreChunk
{
	dlist_node	chain;
	pg_crc32	hash;			/* hash value by (database_oid + table_oid) */
	Oid			database_oid;
	Oid			table_oid;
	TransactionId xmax;
	TransactionId xmin;
	CommandId	cid;
	bool		xmax_commited;
	bool		xmin_commited;
	cl_uint		kds_nitems;			/* copy of kds->nitems */
	cl_uint		kds_length;			/* copy of kds->length */
	cl_int		cuda_dindex;		/* set by 'pinning' option */
	CUipcMemHandle ipc_mhandle;
	dsm_handle	dsm_handle;
};
typedef struct GpuStoreChunk	GpuStoreChunk;

/*
 * GpuStoreMap - status of local mapping
 */
struct GpuStoreMap
{
	dsm_segment	   *dsm_seg;
};
typedef struct GpuStoreMap		GpuStoreMap;

#define GPUSTOREMAP_FOR_CHUNK(gs_chunk)			\
	(&gstore_maps[(gs_chunk) - (gstore_head->gs_chunks)])

/*
 * GpuStoreHead
 */
#define GSTORE_CHUNK_HASH_NSLOTS	97
typedef struct
{
	pg_atomic_uint32 has_warm_chunks;
	slock_t			lock;
	dlist_head		free_chunks;
	dlist_head		active_chunks[GSTORE_CHUNK_HASH_NSLOTS];
	GpuStoreChunk	gs_chunks[FLEXIBLE_ARRAY_MEMBER];
} GpuStoreHead;

/* ---- static functions ---- */
static Oid	gstore_fdw_read_options(Oid table_oid,
									char **p_referenced,
									int *p_pinning);

/* ---- static variables ---- */
static int				gstore_max_nchunks;		/* GUC */
static shmem_startup_hook_type shmem_startup_next;
static GpuStoreHead	   *gstore_head = NULL;
static GpuStoreMap	   *gstore_maps = NULL;

/*
 * gstore_fdw_satisfies_visibility - equivalent to HeapTupleSatisfiesMVCC,
 * but simplified for GpuStoreChunk.
 */
static bool
gstore_fdw_satisfies_visibility(GpuStoreChunk *gs_chunk, Snapshot snapshot)
{
	if (!gs_chunk->xmin_commited)
	{
		if (!TransactionIdIsValid(gs_chunk->xmin))
			return false;		/* aborted or crashed */
		if (TransactionIdIsCurrentTransactionId(gs_chunk->xmin))
		{
			if (gs_chunk->cid >= snapshot->curcid)
				return false;	/* inserted after scan started */
			
			if (gs_chunk->xmax == InvalidTransactionId)
				return true;	/* nobody delete it yet */

			if (!TransactionIdIsCurrentTransactionId(gs_chunk->xmax))
			{
				/* deleting subtransaction must have aborted */
				gs_chunk->xmax = InvalidTransactionId;
				return true;
			}
			if (gs_chunk->cid >= snapshot->curcid)
				return true;    /* deleted after scan started */
			else
				return false;   /* deleted before scan started */
		}
		else if (XidInMVCCSnapshot(gs_chunk->xmin, snapshot))
			return false;
		else if (TransactionIdDidCommit(gs_chunk->xmin))
			gs_chunk->xmin_commited = true;
		else
		{
			/* it must have aborted or crashed */
			gs_chunk->xmin = InvalidTransactionId;
			return false;
		}
	}
	else
	{
		/* xmin is committed, but maybe not according to our snapshot */
		if (gs_chunk->xmin != FrozenTransactionId &&
			XidInMVCCSnapshot(gs_chunk->xmin, snapshot))
			return false;	/* treat as still in progress */
	}
	/* by here, the inserting transaction has committed */
	if (!TransactionIdIsValid(gs_chunk->xmax))
		return true;	/* nobody deleted yet */

	if (!gs_chunk->xmax_commited)
	{
		if (TransactionIdIsCurrentTransactionId(gs_chunk->xmax))
		{
			if (gs_chunk->cid >= snapshot->curcid)
				return true;    /* deleted after scan started */
			else
				return false;   /* deleted before scan started */
        }

		if (XidInMVCCSnapshot(gs_chunk->xmax, snapshot))
			return true;

        if (!TransactionIdDidCommit(gs_chunk->xmax))
        {
            /* it must have aborted or crashed */
			gs_chunk->xmax = InvalidTransactionId;
            return true;
        }
		/* xmax transaction committed */
		gs_chunk->xmax_commited = true;
	}
    else
	{
		/* xmax is committed, but maybe not according to our snapshot */
		if (XidInMVCCSnapshot(gs_chunk->xmax, snapshot))
			return true;        /* treat as still in progress */
    }
	/* xmax transaction committed */
	return false;
}

/*
 * gstore_fdw_mapped_chunk
 */
static inline kern_data_store *
gstore_fdw_mapped_chunk(GpuStoreChunk *gs_chunk)
{
	GpuStoreMap	   *gs_map = GPUSTOREMAP_FOR_CHUNK(gs_chunk);

	if (!gs_map->dsm_seg)
	{
		gs_map->dsm_seg = dsm_attach(gs_chunk->dsm_handle);
		dsm_pin_mapping(gs_map->dsm_seg);
	}
	else if (dsm_segment_handle(gs_map->dsm_seg) != gs_chunk->dsm_handle)
	{
		dsm_detach(gs_map->dsm_seg);

		gs_map->dsm_seg = dsm_attach(gs_chunk->dsm_handle);
		dsm_pin_mapping(gs_map->dsm_seg);
	}
	return (kern_data_store *)dsm_segment_address(gs_map->dsm_seg);
}

/*
 * gstore_fdw_(first|next)_chunk
 */
static GpuStoreChunk *
gstore_fdw_first_chunk(Relation frel, Snapshot snapshot)
{
	Oid				table_oid = RelationGetRelid(frel);
	GpuStoreChunk  *gs_chunk;
	dlist_iter		iter;
	pg_crc32		hash;
	int				index;

	INIT_LEGACY_CRC32(hash);
	COMP_LEGACY_CRC32(hash, &MyDatabaseId, sizeof(Oid));
	COMP_LEGACY_CRC32(hash, &table_oid, sizeof(Oid));
	FIN_LEGACY_CRC32(hash);
	index = hash % GSTORE_CHUNK_HASH_NSLOTS;

	dlist_foreach(iter, &gstore_head->active_chunks[index])
	{
		gs_chunk = dlist_container(GpuStoreChunk, chain, iter.cur);

		if (gs_chunk->hash == hash &&
			gs_chunk->database_oid == MyDatabaseId &&
			gs_chunk->table_oid == table_oid)
		{
			if (gstore_fdw_satisfies_visibility(gs_chunk, snapshot))
				return gs_chunk;
		}
	}
	return NULL;
}

static GpuStoreChunk *
gstore_fdw_next_chunk(GpuStoreChunk *gs_chunk, Snapshot snapshot)
{
	Oid			database_oid = gs_chunk->database_oid;
	Oid			table_oid = gs_chunk->table_oid;
	pg_crc32	hash = gs_chunk->hash;
	int			index = hash % GSTORE_CHUNK_HASH_NSLOTS;
	dlist_head *active_chunks = &gstore_head->active_chunks[index];
	dlist_node *dnode;

	while (dlist_has_next(active_chunks, &gs_chunk->chain))
	{
		dnode = dlist_next_node(active_chunks, &gs_chunk->chain);
		gs_chunk = dlist_container(GpuStoreChunk, chain, dnode);

		if (gs_chunk->hash == hash &&
			gs_chunk->database_oid == database_oid &&
			gs_chunk->table_oid == table_oid)
		{
			if (gstore_fdw_satisfies_visibility(gs_chunk, snapshot))
				return gs_chunk;
		}
	}
	return NULL;
}

/*
 * gstoreGetForeignRelSize
 */
static void
gstoreGetForeignRelSize(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid ftable_oid)
{
	GpuStoreChunk  *gs_chunk;
	size_t			nitems = 0;
	size_t			length = 0;
	char		   *referenced;
	Relation		frel = NULL;

	gstore_fdw_read_options(ftable_oid, &referenced, NULL);
	if (!referenced)
		frel = heap_open(ftable_oid, AccessShareLock);
	else
	{
		List   *names = stringToQualifiedNameList(referenced);

		frel = heap_openrv(makeRangeVarFromNameList(names),
						   AccessShareLock);
	}

	SpinLockAcquire(&gstore_head->lock);
	PG_TRY();
	{
		Snapshot	snapshot = RegisterSnapshot(GetTransactionSnapshot());

		for (gs_chunk = gstore_fdw_first_chunk(frel, snapshot);
			 gs_chunk != NULL;
			 gs_chunk = gstore_fdw_next_chunk(gs_chunk, snapshot))
		{
			nitems += gs_chunk->kds_nitems;
			length += TYPEALIGN(BLCKSZ, gs_chunk->kds_length);
		}
		UnregisterSnapshot(snapshot);
	}
	PG_CATCH();
	{
		SpinLockRelease(&gstore_head->lock);
		PG_RE_THROW();
	}
	PG_END_TRY();
	SpinLockRelease(&gstore_head->lock);
	baserel->rows	= nitems;
	baserel->pages	= length / BLCKSZ;
	heap_close(frel, NoLock);
}

/*
 * gstoreGetForeignPaths
 */
static void
gstoreGetForeignPaths(PlannerInfo *root,
					  RelOptInfo *baserel,
					  Oid foreigntableid)
{
	ParamPathInfo *param_info;
	ForeignPath *fpath;
	Cost		startup_cost = baserel->baserestrictcost.startup;
	Cost		per_tuple = baserel->baserestrictcost.per_tuple;
	Cost		run_cost;
	QualCost	qcost;

	param_info = get_baserel_parampathinfo(root, baserel, NULL);
	if (param_info)
	{
		cost_qual_eval(&qcost, param_info->ppi_clauses, root);
		startup_cost += qcost.startup;
		per_tuple += qcost.per_tuple;
	}
	run_cost = per_tuple * baserel->rows;

	fpath = create_foreignscan_path(root,
									baserel,
									NULL,	/* default pathtarget */
									baserel->rows,
									startup_cost,
									startup_cost + run_cost,
									NIL,	/* no pathkeys */
									NULL,	/* no outer rel either */
									NULL,	/* no extra plan */
									NIL);	/* no fdw_private */
	add_path(baserel, (Path *) fpath);
}

/*
 * gstoreGetForeignPlan
 */
static ForeignScan *
gstoreGetForeignPlan(PlannerInfo *root,
					 RelOptInfo *baserel,
					 Oid foreigntableid,
					 ForeignPath *best_path,
					 List *tlist,
					 List *scan_clauses,
					 Plan *outer_plan)
{
	List	   *scan_quals = NIL;
	ListCell   *lc;

	foreach (lc, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(IsA(rinfo, RestrictInfo));
		if (rinfo->pseudoconstant)
			continue;
		scan_quals = lappend(scan_quals, rinfo->clause);
	}

	return make_foreignscan(tlist,
							scan_quals,
							baserel->relid,
							NIL,		/* fdw_exprs */
							NIL,		/* fdw_private */
							NIL,		/* fdw_scan_tlist */
							NIL,		/* fdw_recheck_quals */
							NULL);		/* outer_plan */
}

/*
 * gstoreScanState - state object for scan
 */
typedef struct
{
	GpuStoreChunk  *gs_chunk;
	cl_ulong		gs_index;
	Relation		gs_rel;
	bool			pinning;
	cl_uint			nattrs;
	AttrNumber		attnos[FLEXIBLE_ARRAY_MEMBER];
} gstoreScanState;

/*
 * gstoreBeginForeignScan
 */
static void
gstoreBeginForeignScan(ForeignScanState *node, int eflags)
{
	//ForeignScan	   *fscan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	TupleDesc	tupdesc = RelationGetDescr(node->ss.ss_currentRelation);
	Relation	gs_rel = NULL;
	gstoreScanState *gss_state;
	char	   *referenced;
	int			i, j;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	if (!IsMVCCSnapshot(estate->es_snapshot))
		elog(ERROR, "cannot scan gstore_fdw table without MVCC snapshot");

	gstore_fdw_read_options(RelationGetRelid(node->ss.ss_currentRelation),
							&referenced, NULL);
	if (referenced)
	{
		List   *names = stringToQualifiedNameList(referenced);

		gs_rel = heap_openrv(makeRangeVarFromNameList(names),
							 AccessShareLock);
	}
	gss_state = palloc(offsetof(gstoreScanState,
								attnos[tupdesc->natts]));
	gss_state->gs_chunk = NULL;
	gss_state->gs_index = 0;
	gss_state->gs_rel = gs_rel;
	gss_state->nattrs = tupdesc->natts;
	if (gs_rel != NULL)
	{
		TupleDesc	phydesc = RelationGetDescr(gs_rel);

		for (i=0; i < tupdesc->natts; i++)
		{
			const char *attname = NameStr(tupdesc->attrs[i]->attname);

			for (j=0; j < phydesc->natts; j++)
			{
				Form_pg_attribute	pattr = phydesc->attrs[j];

				if (strcmp(attname, NameStr(pattr->attname)) == 0)
				{
					gss_state->attnos[i] = pattr->attnum;
					break;
				}
			}
			if (j >= phydesc->natts)
				elog(ERROR, "attribute \"%s\" was not found at \"%s\"",
					 attname, RelationGetRelationName(gs_rel));
		}
	}
	else
	{
		for (i=0; i < tupdesc->natts; i++)
			gss_state->attnos[i] = i+1;
	}
	node->fdw_state = (void *)gss_state;
}

/*
 * gstoreIterateForeignScan
 */
static TupleTableSlot *
gstoreIterateForeignScan(ForeignScanState *node)
{
	gstoreScanState	*gss_state = (gstoreScanState *) node->fdw_state;
	Relation		frel = node->ss.ss_currentRelation;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	EState		   *estate = node->ss.ps.state;
	Snapshot		snapshot = estate->es_snapshot;
	GpuStoreChunk  *gs_chunk;
	kern_data_store *kds;
	cl_ulong		index;
	cl_int			i, colidx;
	cl_uint			offset;
	kern_colmeta   *kcmeta;
	char		   *att_values;

	ExecClearTuple(slot);
	if (!gss_state->gs_chunk)
	{
		SpinLockAcquire(&gstore_head->lock);
		gs_chunk = gstore_fdw_first_chunk(frel, snapshot);
		SpinLockRelease(&gstore_head->lock);
		if (!gs_chunk)
			return NULL;
		gss_state->gs_chunk = gs_chunk;
	}
next:
	kds = gstore_fdw_mapped_chunk(gss_state->gs_chunk);

	if (gss_state->gs_index >= kds->nitems)
	{
		gss_state->gs_chunk = gstore_fdw_next_chunk(gss_state->gs_chunk,
													snapshot);
		gss_state->gs_index = 0;
		if (!gss_state->gs_chunk)
			return NULL;
		goto next;
	}
	index = gss_state->gs_index++;
	ExecStoreAllNullTuple(slot);

	for (i=0; i < gss_state->nattrs; i++)
	{
		colidx = gss_state->attnos[i] - 1;
		Assert(colidx >= 0 && colidx < kds->ncols);
		kcmeta = &kds->colmeta[colidx];
		att_values = (char *)kds + kcmeta->values_offset;
		if (kcmeta->attlen > 0)
		{
			/* null-check */
			if (kcmeta->extra_sz > 0 &&
				index < BITS_PER_BYTE * kcmeta->extra_sz)
			{
				bits8  *nullmap = (bits8 *)
					(att_values + MAXALIGN(kcmeta->attlen * kds->nitems));

				if (att_isnull(index, nullmap))
					continue;
			}
			att_values += kcmeta->attlen * index;
			slot->tts_isnull[i] = false;
			slot->tts_values[i] = fetch_att(att_values,
											kcmeta->attbyval,
											kcmeta->attlen);
		}
		else
		{
			/* varlena field */
			Assert(kcmeta->attlen == -1);

			offset = ((cl_uint *)att_values)[index];
			/* null-check */
			if (offset == 0)
				continue;
			Assert(offset >= sizeof(cl_uint) * kds->nitems &&
				   offset <  sizeof(cl_uint) * kds->nitems + kcmeta->extra_sz);
			slot->tts_isnull[i] = false;
			slot->tts_values[i] = PointerGetDatum(att_values + offset);
		}
	}
	return slot;
}

/*
 * gstoreReScanForeignScan
 */
static void
gstoreReScanForeignScan(ForeignScanState *node)
{
	gstoreScanState *gss_state = (gstoreScanState *) node->fdw_state;

	gss_state->gs_chunk = NULL;
	gss_state->gs_index = 0;
}

/*
 * gstoreEndForeignScan
 */
static void
gstoreEndForeignScan(ForeignScanState *node)
{
}

/*
 * gstoreIsForeignRelUpdatable
 */
static int
gstoreIsForeignRelUpdatable(Relation rel)
{
	char   *referenced;

	gstore_fdw_read_options(RelationGetRelid(rel), &referenced, NULL);
	/* only primary gstore_fdw tables are updatable */
	if (referenced)
		return 0;

	return (1 << CMD_INSERT) | (1 << CMD_DELETE);
}

/*
 * gstorePlanDirectModify - allows only DELETE with no WHERE-clause
 */
static bool
gstorePlanDirectModify(PlannerInfo *root,
					   ModifyTable *plan,
					   Index resultRelation,
					   int subplan_index)
{
	CmdType		operation = plan->operation;
	Plan	   *subplan = (Plan *) list_nth(plan->plans, subplan_index);

	/* only DELETE command */
	if (operation != CMD_DELETE)
		return false;
	/* no WHERE-clause */
	if (subplan->qual != NIL)
		return false;
	/* no RETURNING-clause */
	if (plan->returningLists != NIL)
		return false;
	/* subplan should be GpuStore FDW */
	if (!IsA(subplan, ForeignScan))
		return false;

	/* OK, Update the operation */
	((ForeignScan *) subplan)->operation = CMD_DELETE;

	return true;
}

/*
 * gstorePlanForeignModify
 */
static List *
gstorePlanForeignModify(PlannerInfo *root,
						ModifyTable *plan,
						Index resultRelation,
						int subplan_index)
{
	CmdType		operation = plan->operation;

	if (operation != CMD_INSERT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gstore_fdw: not a supported operation"),
				 errdetail("gstore_fdw supports either INSERT into an empty GpuStore or DELETE without WHERE-clause only")));

	return NIL;
}

/*
 * gstoreLoadState - state object for INSERT
 */
typedef struct
{
	GpuContext *gcontext;	/* GpuContext, if pinned gstore */
	size_t		length;		/* available size except for KDS header */
	size_t		nrooms;		/* available max number of items */
	size_t		nitems;		/* current number of items */
	MemoryContext memcxt;	/* memcxt for construction per chunk */
	struct {
		HTAB   *vl_dict;	/* dictionary of varlena datum, if any */
		size_t	extra_sz;	/* usage by varlena datum */
		bits8  *nullmap;	/* NULL-bitmap */
		void   *values;		/* array of values */
		int		align;		/* numeric form of attalign */
	} a[FLEXIBLE_ARRAY_MEMBER];
} gstoreLoadState;

typedef struct
{
	cl_uint		offset;		/* to be used later */
	struct varlena *vl_datum;
} vl_dict_key;

static uint32
vl_dict_hash_value(const void *__key, Size keysize)
{
	const vl_dict_key *key = __key;
	pg_crc32		crc;

	Assert(keysize == sizeof(vl_dict_key));
	INIT_LEGACY_CRC32(crc);
	COMP_LEGACY_CRC32(crc, key->vl_datum, VARSIZE_ANY(key->vl_datum));
	FIN_LEGACY_CRC32(crc);

	return (uint32) crc;
}

static int
vl_dict_compare(const void *__key1, const void *__key2, Size keysize)
{
	const vl_dict_key *key1 = __key1;
	const vl_dict_key *key2 = __key2;

	if (VARSIZE_ANY(key1->vl_datum) == VARSIZE_ANY(key2->vl_datum))
		return memcmp(key1->vl_datum, key2->vl_datum,
					  VARSIZE_ANY(key1->vl_datum));
	return 1;
}

static HTAB *
vl_dict_create(MemoryContext memcxt, size_t nrooms)
{
	HASHCTL		hctl;

	memset(&hctl, 0, sizeof(HASHCTL));
	hctl.hash = vl_dict_hash_value;
	hctl.match = vl_dict_compare;
	hctl.keysize = sizeof(vl_dict_key);
	hctl.hcxt = memcxt;

	return hash_create("varlena dictionary hash-table",
					   Max(nrooms / 10, 4096),
					   &hctl,
					   HASH_FUNCTION |
					   HASH_COMPARE |
					   HASH_CONTEXT);
}

/*
 * gstore_fdw_writeout_chunk
 */
static void
gstore_fdw_writeout_chunk(Relation relation, gstoreLoadState *gs_lstate)
{
	GpuContext	   *gcontext = gs_lstate->gcontext;
	Oid				table_oid = RelationGetRelid(relation);
	TupleDesc		tupdesc = RelationGetDescr(relation);
	MemoryContext	memcxt = gs_lstate->memcxt;
	size_t			nrooms = gs_lstate->nrooms;
	size_t			nitems = gs_lstate->nitems;
	size_t			length;
	size_t			offset;
	pg_crc32		hash;
	cl_int			i, j, unitsz;
	cl_int			cuda_dindex = (gcontext ? gcontext->cuda_dindex : -1);
	dsm_segment	   *dsm_seg;
	kern_data_store *kds;
	CUipcMemHandle	ipc_mhandle;
	dlist_node	   *dnode;
	GpuStoreChunk  *gs_chunk = NULL;
	GpuStoreMap	   *gs_map = NULL;

	length = offset = MAXALIGN(offsetof(kern_data_store,
										colmeta[tupdesc->natts]));
	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute	attr = tupdesc->attrs[i];

		if (attr->attlen < 0)
		{
			length += (MAXALIGN(sizeof(cl_uint) * nitems) +
					   gs_lstate->a[i].extra_sz);
		}
		else
		{
			if (gs_lstate->a[i].nullmap)
				length += MAXALIGN(BITMAPLEN(nitems));
			length += MAXALIGN(TYPEALIGN(gs_lstate->a[i].align,
										 attr->attlen) * nitems);
		}
	}

	dsm_seg = dsm_create(length, 0);
	kds = dsm_segment_address(dsm_seg);

	init_kernel_data_store(kds,
						   tupdesc,
						   length,
						   KDS_FORMAT_COLUMN,
						   nitems);
	for (i=0; i < tupdesc->natts; i++)
	{
		kern_colmeta   *cmeta = &kds->colmeta[i];

		cmeta->values_offset = offset;
		if (cmeta->attlen < 0)
		{
			HASH_SEQ_STATUS	hseq;
			vl_dict_key	  **entries_array;
			vl_dict_key	   *entry;
			cl_uint		   *base;
			char		   *extra;

			/* put varlena datum on the extra area */
			base = (cl_uint *)((char *)kds + offset);
			extra = (char *)base + MAXALIGN(sizeof(cl_uint) * nitems);
			hash_seq_init(&hseq, gs_lstate->a[i].vl_dict);
			while ((entry = hash_seq_search(&hseq)) != NULL)
			{
				entry->offset = extra - (char *)base;
				unitsz = VARSIZE_ANY(entry->vl_datum);
				memcpy(extra, entry->vl_datum, unitsz);
				cmeta->extra_sz += MAXALIGN(unitsz);
				extra += MAXALIGN(unitsz);
			}
			hash_seq_term(&hseq);

			/* put offset of varlena datum */
			entries_array = (vl_dict_key **)gs_lstate->a[i].values;
			for (j=0; j < nitems; j++)
			{
				entry = entries_array[j];
				base[j] = (!entry ? 0 : entry->offset);
			}
		}
		else
		{
			unitsz = TYPEALIGN(gs_lstate->a[i].align, cmeta->attlen);

			memcpy((char *)kds + offset,
				   gs_lstate->a[i].values,
				   unitsz * nitems);
			offset += MAXALIGN(unitsz * nitems);
			if (gs_lstate->a[i].nullmap)
			{
				memcpy((char *)kds + offset,
					   gs_lstate->a[i].nullmap,
					   BITMAPLEN(nitems));
				cmeta->extra_sz = BITMAPLEN(nitems);
				offset += MAXALIGN(BITMAPLEN(nitems));
			}
		}
	}
	kds->nitems = nitems;

	/* allocation of device memory if 'pinning' mode */
	if (gcontext)
	{
		CUdeviceptr	m_deviceptr;
		CUresult	rc;

		rc = gpuMemAllocPreserved(cuda_dindex,
								  &ipc_mhandle,
								  length);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on gpuMemAllocPreserved: %s", errorText(rc));
		PG_TRY();
		{
			rc = gpuIpcOpenMemHandle(gcontext,
									 &m_deviceptr,
									 ipc_mhandle,
									 CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS);
			if (rc != CUDA_SUCCESS)
				elog(ERROR, "failed on gpuIpcOpenMemHandle: %s",
					 errorText(rc));

			rc = cuCtxPushCurrent(gcontext->cuda_context);
			if (rc != CUDA_SUCCESS)
				elog(ERROR, "failed on cuCtxPushCurrent: %s", errorText(rc));

			rc = cuMemcpyHtoD(m_deviceptr, kds, length);
			if (rc != CUDA_SUCCESS)
				elog(ERROR, "failed on cuMemcpyHtoD: %s", errorText(rc));

			rc = cuCtxPopCurrent(NULL);
			if (rc != CUDA_SUCCESS)
				elog(ERROR, "failed on cuCtxPopCurrent: %s", errorText(rc));
		}
		PG_CATCH();
		{
			gpuMemFreePreserved(cuda_dindex, ipc_mhandle);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
	{
		memset(&ipc_mhandle, 0, sizeof(CUipcMemHandle));
	}
	/* pin the DSM segment to servive over the transaction */
	dsm_pin_mapping(dsm_seg);
	dsm_pin_segment(dsm_seg);

	/* hash value */
	INIT_LEGACY_CRC32(hash);
    COMP_LEGACY_CRC32(hash, &MyDatabaseId, sizeof(Oid));
	COMP_LEGACY_CRC32(hash, &table_oid, sizeof(Oid));
	FIN_LEGACY_CRC32(hash);

	SpinLockAcquire(&gstore_head->lock);
	if (dlist_is_empty(&gstore_head->free_chunks))
	{
		SpinLockRelease(&gstore_head->lock);
		if (gcontext)
			gpuMemFreePreserved(cuda_dindex, ipc_mhandle);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("too many gstore_fdw chunks required")));
	}
	dnode = dlist_pop_head_node(&gstore_head->free_chunks);
	gs_chunk = dlist_container(GpuStoreChunk, chain, dnode);
	gs_map = GPUSTOREMAP_FOR_CHUNK(gs_chunk);
	memset(gs_chunk, 0, sizeof(GpuStoreChunk));
	gs_chunk->hash = hash;
	gs_chunk->database_oid = MyDatabaseId;
	gs_chunk->table_oid = table_oid;
	gs_chunk->xmax = InvalidTransactionId;
	gs_chunk->xmin = GetCurrentTransactionId();
	gs_chunk->cid = GetCurrentCommandId(true);
	gs_chunk->xmax_commited = false;
	gs_chunk->xmin_commited = false;
	gs_chunk->kds_length = kds->length;
	gs_chunk->kds_nitems = kds->nitems;
	memcpy(&gs_chunk->ipc_mhandle, &ipc_mhandle, sizeof(CUipcMemHandle));
	gs_chunk->cuda_dindex = cuda_dindex;
	gs_chunk->dsm_handle = dsm_segment_handle(dsm_seg);
	gs_map->dsm_seg = dsm_seg;

	i = hash % GSTORE_CHUNK_HASH_NSLOTS;
	dlist_push_tail(&gstore_head->active_chunks[i], &gs_chunk->chain);
	pg_atomic_add_fetch_u32(&gstore_head->has_warm_chunks, 1);
	SpinLockRelease(&gstore_head->lock);

	/* reset temporary buffer */
	MemoryContextReset(memcxt);
	for (i=0; i < tupdesc->natts; i++)
	{
		if (gs_lstate->a[i].vl_dict)
			gs_lstate->a[i].vl_dict = vl_dict_create(memcxt, nrooms);
		if (gs_lstate->a[i].nullmap)
		{
			pfree(gs_lstate->a[i].nullmap);
			gs_lstate->a[i].nullmap = NULL;
		}
	}
	gs_lstate->nitems = 0;
}

/*
 * gstore_fdw_release_chunk
 */
static void
gstore_fdw_release_chunk(GpuStoreChunk *gs_chunk)
{
	GpuStoreMap    *gs_map = GPUSTOREMAP_FOR_CHUNK(gs_chunk);

	dlist_delete(&gs_chunk->chain);
	if (gs_chunk->cuda_dindex >= 0)
		gpuMemFreePreserved(gs_chunk->cuda_dindex, gs_chunk->ipc_mhandle);
	if (gs_map->dsm_seg)
		dsm_detach(gs_map->dsm_seg);
	gs_map->dsm_seg = NULL;
#if PG_VERSION_NUM >= 100000
	/*
	 * NOTE: PG9.6 has no way to release DSM segment once pinned.
	 * dsm_unpin_segment() was newly supported at PG10.
	 */
	dsm_unpin_segment(gs_chunk->handle);
#endif
	memset(gs_chunk, 0, sizeof(GpuStoreMap));
	gs_chunk->dsm_handle = UINT_MAX;
	dlist_push_head(&gstore_head->free_chunks,
					&gs_chunk->chain);
}

/*
 * gstoreBeginForeignModify
 */
static void
gstoreBeginForeignModify(ModifyTableState *mtstate,
						 ResultRelInfo *rrinfo,
						 List *fdw_private,
						 int subplan_index,
						 int eflags)
{
	EState		   *estate = mtstate->ps.state;
	Relation		relation = rrinfo->ri_RelationDesc;
	TupleDesc		tupdesc = RelationGetDescr(relation);
	GpuContext	   *gcontext = NULL;
	GpuStoreChunk  *gs_chunk;
	gstoreLoadState *gs_lstate;
	char		   *referenced;
	int				pinning;
	size_t			nrooms;
	cl_int			i, unitsz;

	gstore_fdw_read_options(RelationGetRelid(relation),
							&referenced, &pinning);
	if (referenced)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is not a primary gstore_fdw foreign table",
						RelationGetRelationName(relation))));
	if (pinning >= 0)
	{
		gcontext = AllocGpuContext(pinning, false);
		if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
			ActivateGpuContext(gcontext);
	}
	LockRelationOid(RelationGetRelid(relation), ShareUpdateExclusiveLock);
	SpinLockAcquire(&gstore_head->lock);
	gs_chunk = gstore_fdw_first_chunk(relation, estate->es_snapshot);
	SpinLockRelease(&gstore_head->lock);

	//XXX - xact hook may be able to merge smaller chunks
	if (gs_chunk)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gstore_fdw: foreign table \"%s\" is not empty",
						RelationGetRelationName(relation))));
	/* state object */
	gs_lstate = palloc0(offsetof(gstoreLoadState, a[tupdesc->natts]));
	gs_lstate->gcontext = gcontext;
	gs_lstate->memcxt = AllocSetContextCreate(estate->es_query_cxt,
											  "gstore_fdw temporary context",
											  ALLOCSET_DEFAULT_SIZES);
	gs_lstate->length = GPUSTORE_CHUNK_SIZE -
		offsetof(kern_data_store, colmeta[tupdesc->natts]);

	/*
	 * calculation of the maximum possible nrooms, in case when all the
	 * column has no NULLs (thus no null bitmap), and extra_sz by varlena
	 * values are ignored.
	 */
	unitsz = 0;
	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];
		int		align;

		if (attr->attlen < 0)
			unitsz += sizeof(cl_uint);	/* varlena offset */
		else
		{
			if (attr->attalign == 'c')
				align = sizeof(cl_char);
			else if (attr->attalign == 's')
				align = sizeof(cl_short);
			else if (attr->attalign == 'i')
				align = sizeof(cl_int);
			else if (attr->attalign == 'd')
				align = sizeof(cl_long);
			else
				elog(ERROR, "Bug? unexpected alignment: %c", attr->attalign);
			unitsz += TYPEALIGN(align, attr->attlen);
			gs_lstate->a[i].align = align;
		}
	}
	nrooms = (gs_lstate->length -	/* consider the margin for alignment */
			  MAXIMUM_ALIGNOF * tupdesc->natts) / unitsz;
	gs_lstate->nrooms = nrooms;
	gs_lstate->nitems = 0;

	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];

		if (attr->attlen < 0)
		{
			MemoryContext	memcxt = gs_lstate->memcxt;
			gs_lstate->a[i].vl_dict = vl_dict_create(memcxt, nrooms);
			gs_lstate->a[i].values = palloc(sizeof(void *) * nrooms);
		}
		else
		{
			gs_lstate->a[i].values = palloc(TYPEALIGN(gs_lstate->a[i].align,
													  attr->attlen) * nrooms);
		}
	}
	rrinfo->ri_FdwState = gs_lstate;
}

/*
 * gstoreExecForeignInsert
 */
static TupleTableSlot *
gstoreExecForeignInsert(EState *estate,
						ResultRelInfo *rrinfo,
						TupleTableSlot *slot,
						TupleTableSlot *planSlot)
{
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	gstoreLoadState *gs_lstate = rrinfo->ri_FdwState;
	size_t		nrooms = gs_lstate->nrooms;
	size_t		nitems = gs_lstate->nitems;
	size_t		usage = 0;
	size_t		index;
	cl_int		i;

	slot_getallattrs(slot);

	/*
	 * calculation of extra consumption by this new line
	 */
	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute	attr = tupdesc->attrs[i];

		if (attr->attlen < 0)
		{
			if (!slot->tts_isnull[i])
			{
				vl_dict_key	key;

				key.offset = 0;
				key.vl_datum = (struct varlena *)slot->tts_values[i];

				if (!hash_search(gs_lstate->a[i].vl_dict,
								 &key, HASH_FIND, NULL))
					usage += MAXALIGN(VARSIZE_ANY(key.vl_datum));
			}
			usage += sizeof(cl_uint);
		}
		else
		{
			if (gs_lstate->a[i].nullmap || slot->tts_isnull[i])
				usage += MAXALIGN(BITMAPLEN(nitems + 1));
			usage += TYPEALIGN(gs_lstate->a[i].align,
							   attr->attlen) * (nitems + 1);
		}
	}

	if (usage > gs_lstate->length)
		gstore_fdw_writeout_chunk(rrinfo->ri_RelationDesc, gs_lstate);

	index = gs_lstate->nitems++;
	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];
		bits8	   *nullmap = gs_lstate->a[i].nullmap;
		char	   *values = gs_lstate->a[i].values;
		Datum		datum = slot->tts_values[i];

		if (slot->tts_isnull[i])
		{
			if (attr->attnotnull)
				elog(ERROR,
					 "attribute \"%s\" of relation \"%s\" must be NOT NULL",
					 NameStr(attr->attname),
					 RelationGetRelationName(rrinfo->ri_RelationDesc));
			if (!nullmap)
			{
				nullmap = MemoryContextAlloc(gs_lstate->memcxt,
											 MAXALIGN(BITMAPLEN(nrooms)));
				memset(nullmap, -1, BITMAPLEN(index));
				gs_lstate->a[i].nullmap = nullmap;
			}
			nullmap[index >> 3] &= ~(1 << (index & 7));
		}
		else
		{
			if (nullmap)
				nullmap[index >> 3] |=  (1 << (index & 7));
			if (attr->attlen < 0)
			{
				vl_dict_key	key, *entry;
				bool		found;

				key.offset = 0;
				key.vl_datum = (struct varlena *)DatumGetPointer(datum);
				entry = hash_search(gs_lstate->a[i].vl_dict,
									&key,
									HASH_ENTER,
									&found);
				if (!found)
				{
					MemoryContext oldcxt
						= MemoryContextSwitchTo(gs_lstate->memcxt);
					entry->offset = 0;
					entry->vl_datum = PG_DETOAST_DATUM_COPY(datum);
					MemoryContextSwitchTo(oldcxt);

					gs_lstate->a[i].extra_sz += MAXALIGN(VARSIZE_ANY(datum));
				}
				((vl_dict_key **)values)[index] = entry;
			}
			else if (!attr->attbyval)
			{
				values += TYPEALIGN(gs_lstate->a[i].align,
									attr->attlen) * index;
				memcpy(values, DatumGetPointer(datum), attr->attlen);
			}
			else
			{
				values += TYPEALIGN(gs_lstate->a[i].align,
									attr->attlen) * index;
				switch (attr->attlen)
				{
					case sizeof(cl_char):
						*((cl_char *)values) = DatumGetChar(datum);
						break;
					case sizeof(cl_short):
						*((cl_short *)values) = DatumGetInt16(datum);
						break;
					case sizeof(cl_int):
						*((cl_int *)values) = DatumGetInt32(datum);
						break;
					case sizeof(cl_long):
						*((cl_long *)values) = DatumGetInt64(datum);
						break;
					default:
						elog(ERROR, "Unexpected attribute length: %d",
							 attr->attlen);
				}
			}
		}
	}
	return slot;
}

/*
 * gstoreExecForeignDelete
 */
static TupleTableSlot *
gstoreExecForeignDelete(EState *estate,
						ResultRelInfo *rinfo,
						TupleTableSlot *slot,
						TupleTableSlot *planSlot)
{
	elog(ERROR, "Only Direct DELETE is supported");
}

/*
 * gstoreEndForeignModify
 */
static void
gstoreEndForeignModify(EState *estate,
					   ResultRelInfo *rrinfo)
{
	gstoreLoadState *gs_lstate = rrinfo->ri_FdwState;

	if (gs_lstate->nitems > 0)
		gstore_fdw_writeout_chunk(rrinfo->ri_RelationDesc, gs_lstate);
	if (gs_lstate->gcontext)
		PutGpuContext(gs_lstate->gcontext);
}

/*
 * gstoreBeginDirectModify
 */
static void
gstoreBeginDirectModify(ForeignScanState *node, int eflags)
{
	EState	   *estate = node->ss.ps.state;
	ResultRelInfo *rrinfo = estate->es_result_relation_info;
	Relation	frel = rrinfo->ri_RelationDesc;

	LockRelationOid(RelationGetRelid(frel), ShareUpdateExclusiveLock);
}

/*
 * gstoreIterateDirectModify
 */
static TupleTableSlot *
gstoreIterateDirectModify(ForeignScanState *node)
{
	EState	   *estate = node->ss.ps.state;
	ResultRelInfo *rrinfo = estate->es_result_relation_info;
	Relation	frel = rrinfo->ri_RelationDesc;
	Snapshot	snapshot = estate->es_snapshot;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	Instrumentation *instr = node->ss.ps.instrument;
	GpuStoreChunk *gs_chunk;

	SpinLockAcquire(&gstore_head->lock);
	for (gs_chunk = gstore_fdw_first_chunk(frel, snapshot);
		 gs_chunk != NULL;
		 gs_chunk = gstore_fdw_next_chunk(gs_chunk, snapshot))
	{
		Assert(!TransactionIdIsValid(gs_chunk->xmax));
		gs_chunk->xmax = GetCurrentTransactionId();
		gs_chunk->cid = GetCurrentCommandId(true);

		estate->es_processed += gs_chunk->kds_nitems;
		if (instr)
			instr->tuplecount += gs_chunk->kds_nitems;
	}
	pg_atomic_add_fetch_u32(&gstore_head->has_warm_chunks, 1);
	SpinLockRelease(&gstore_head->lock);

	return ExecClearTuple(slot);
}

/*
 * gstoreEndDirectModify
 */
static void
gstoreEndDirectModify(ForeignScanState *node)
{}

/*
 * gstoreXactCallbackPerChunk
 */
static bool
gstoreOnXactCallbackPerChunk(bool is_commit, GpuStoreChunk *gs_chunk,
							 TransactionId oldestXmin)
{
	if (TransactionIdIsCurrentTransactionId(gs_chunk->xmax))
	{
		if (is_commit)
			gs_chunk->xmax_commited = true;
		else
			gs_chunk->xmax = InvalidTransactionId;
	}
	if (TransactionIdIsCurrentTransactionId(gs_chunk->xmin))
	{
		if (is_commit)
			gs_chunk->xmin_commited = true;
		else
		{
			gstore_fdw_release_chunk(gs_chunk);
			return false;
		}
	}

	if (TransactionIdIsValid(gs_chunk->xmax))
	{
		/* someone tried to delete chunk, but not commited yet */
		if (!gs_chunk->xmax_commited)
			return true;
		/*
		 * chunk deletion is commited, but some open transactions may
		 * still reference the chunk
		 */
		if (!TransactionIdPrecedes(gs_chunk->xmax, oldestXmin))
			return true;

		/* Otherwise, GpuStoreChunk can be released immediately */
		gstore_fdw_release_chunk(gs_chunk);
	}
	else if (TransactionIdIsNormal(gs_chunk->xmin))
	{
		/* someone tried to insert chunk, but not commited yet */
		if (!gs_chunk->xmin_commited)
			return true;
		/*
		 * chunk insertion is commited, but some open transaction may
		 * need MVCC style visibility control
		 */
		if (!TransactionIdPrecedes(gs_chunk->xmin, oldestXmin))
			return true;

		/* Otherwise, GpuStoreChunk can be visible to everybody */
		gs_chunk->xmin = FrozenTransactionId;
	}
	else if (!TransactionIdIsValid(gs_chunk->xmin))
	{
		/* GpuChunk insertion aborted */
		gstore_fdw_release_chunk(gs_chunk);
	}
	return false;
}

/*
 * gstoreXactCallback
 */
static void
gstoreXactCallback(XactEvent event, void *arg)
{
	TransactionId oldestXmin;
	bool		is_commit;
	bool		meet_warm_chunks = false;
	cl_int		i;

	if (event == XACT_EVENT_COMMIT)
		is_commit = true;
	else if (event == XACT_EVENT_ABORT)
		is_commit = false;
	else
		return;		/* do nothing */

//	elog(INFO, "gstoreXactCallback xid=%u", GetCurrentTransactionIdIfAny());

	if (pg_atomic_read_u32(&gstore_head->has_warm_chunks) == 0)
		return;

	oldestXmin = GetOldestXmin(NULL, true);
	SpinLockAcquire(&gstore_head->lock);
	for (i=0; i < GSTORE_CHUNK_HASH_NSLOTS; i++)
	{
		dlist_mutable_iter	iter;

		dlist_foreach_modify(iter, &gstore_head->active_chunks[i])
		{
			GpuStoreChunk  *gs_chunk
				= dlist_container(GpuStoreChunk, chain, iter.cur);

			if (gstoreOnXactCallbackPerChunk(is_commit, gs_chunk, oldestXmin))
				meet_warm_chunks = true;
		}
	}
	if (!meet_warm_chunks)
		pg_atomic_write_u32(&gstore_head->has_warm_chunks, 0);
	SpinLockRelease(&gstore_head->lock);
}

#if 0
/*
 * gstoreSubXactCallback - just for debug
 */
static void
gstoreSubXactCallback(SubXactEvent event, SubTransactionId mySubid,
					  SubTransactionId parentSubid, void *arg)
{
	elog(INFO, "gstoreSubXactCallback event=%s my_xid=%u pr_xid=%u",
		 (event == SUBXACT_EVENT_START_SUB ? "StartSub" :
		  event == SUBXACT_EVENT_COMMIT_SUB ? "CommitSub" :
		  event == SUBXACT_EVENT_ABORT_SUB ? "AbortSub" :
		  event == SUBXACT_EVENT_PRE_COMMIT_SUB ? "PreCommitSub" : "???"),
		 mySubid, parentSubid);
}
#endif

/*
 * relation_is_gstore_fdw
 */
static bool
relation_is_gstore_fdw(Oid table_oid, bool allows_reference_gstore)
{
	HeapTuple	tup;
	Oid			fserv_oid;
	Oid			fdw_oid;
	Oid			handler_oid;
	PGFunction	handler_fn;
	Datum		datum;
	char	   *referenced;
	char	   *prosrc;
	char	   *probin;
	bool		isnull;
	/* it should be foreign table, of course */
	if (get_rel_relkind(table_oid) != RELKIND_FOREIGN_TABLE)
		return false;
	/* pull OID of foreign-server */
	fserv_oid = gstore_fdw_read_options(table_oid,
										&referenced,
										NULL);
	if (!allows_reference_gstore && referenced)
		return false;

	/* pull OID of foreign-data-wrapper */
	tup = SearchSysCache1(FOREIGNSERVEROID, ObjectIdGetDatum(fserv_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "foreign server with OID %u does not exist", fserv_oid);
	fdw_oid = ((Form_pg_foreign_server) GETSTRUCT(tup))->srvfdw;
	ReleaseSysCache(tup);

	/* pull OID of FDW handler function */
	tup = SearchSysCache1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fdw_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for foreign-data wrapper %u",fdw_oid);
	handler_oid = ((Form_pg_foreign_data_wrapper) GETSTRUCT(tup))->fdwhandler;
	ReleaseSysCache(tup);
	/* pull library path & function name */
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(handler_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", handler_oid);
	if (((Form_pg_proc) GETSTRUCT(tup))->prolang != ClanguageId)
		elog(ERROR, "FDW handler function is not written with C-language");

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc for C function %u", handler_oid);
	prosrc = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &isnull);
	if (isnull)
		elog(ERROR, "null probin for C function %u", handler_oid);
	probin = TextDatumGetCString(datum);
	ReleaseSysCache(tup);
	/* check whether function pointer is identical */
	handler_fn = load_external_function(probin, prosrc, true, NULL);
	if (handler_fn != pgstrom_gstore_fdw_handler)
		return false;
	/* OK, it is GpuStore foreign table */
	return true;
}

/*
 * gstore_fdw_read_options
 */
static Oid
gstore_fdw_read_options(Oid table_oid,
						char **p_referenced,
						int *p_pinning)
{
	HeapTuple	tup;
	Datum		datum;
	bool		isnull;
	Oid			fserv_oid;
	char	   *referenced = NULL;
	int			pinning = -1;

	tup = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for foreign table %u", table_oid);
	fserv_oid = ((Form_pg_foreign_table) GETSTRUCT(tup))->ftserver;
	datum = SysCacheGetAttr(FOREIGNTABLEREL, tup,
							Anum_pg_foreign_table_ftoptions,
							&isnull);
	if (!isnull)
	{
		List	   *options = untransformRelOptions(datum);
		ListCell   *lc;

		foreach (lc, options)
		{
			DefElem	   *defel = lfirst(lc);

			if (strcmp(defel->defname, "reference") == 0)
			{
				referenced = defGetString(defel);
			}
			else if (strcmp(defel->defname, "pinning") == 0)
			{
				pinning = atoi(defGetString(defel));
				if (pinning < 0 || pinning >= numDevAttrs)
					elog(ERROR, "pinning on unknown GPU device: %d", pinning);
			}
			else
				elog(ERROR, "Unknown FDW option: '%s'='%s'",
					 defel->defname, defGetString(defel));
		}
	}
	ReleaseSysCache(tup);
	if (pinning >= 0 && referenced)
		elog(ERROR, "cannot use 'reference' and 'pinning' option together");


	if (p_referenced)
		*p_referenced = referenced;
	if (p_pinning)
		*p_pinning = pinning;
	return fserv_oid;
}

/*
 * pgstrom_gstore_fdw_validator
 */
Datum
pgstrom_gstore_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	ListCell   *lc;
	char	   *referenced = NULL;
	int			pinning = -1;

	foreach(lc, options)
	{
		DefElem	   *defel = lfirst(lc);

		if (strcmp(defel->defname, "reference") == 0 &&
			catalog == ForeignTableRelationId)
		{
			char   *relname = defGetString(defel);
			List   *names;
			Oid		reloid;

			if (referenced)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("\"reference\" option appears twice")));

			names = stringToQualifiedNameList(relname);
			reloid = RangeVarGetRelid(makeRangeVarFromNameList(names),
									  NoLock, false);
			if (!relation_is_gstore_fdw(reloid, false))
				elog(ERROR, "%s: not a primary gstore_fdw foreign table",
					 relname);
			referenced = relname;
		}
		else if (strcmp(defel->defname, "pinning") == 0 &&
				 catalog == ForeignTableRelationId)
		{
			if (pinning >= 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"pinning\" option appears twice")));
			pinning = atoi(defGetString(defel));
			if (pinning < 0 || pinning >= numDevAttrs)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("\"pinning\" on unavailable GPU device")));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("FDW option \"%s\" = \"%s\" is not supported",
							defel->defname, defGetString(defel))));
		}
	}
	if (pinning >= 0 && referenced)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use 'reference' and 'pinning' together")));
	PG_RETURN_VOID();
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_fdw_validator);

/*
 * pgstrom_gstore_fdw_handler
 */
Datum
pgstrom_gstore_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	/* functions for scanning foreign tables */
	routine->GetForeignRelSize	= gstoreGetForeignRelSize;
	routine->GetForeignPaths	= gstoreGetForeignPaths;
	routine->GetForeignPlan		= gstoreGetForeignPlan;
	routine->BeginForeignScan	= gstoreBeginForeignScan;
	routine->IterateForeignScan	= gstoreIterateForeignScan;
	routine->ReScanForeignScan	= gstoreReScanForeignScan;
	routine->EndForeignScan		= gstoreEndForeignScan;

	/* functions for INSERT/DELETE foreign tables */
	routine->IsForeignRelUpdatable = gstoreIsForeignRelUpdatable;

	routine->PlanForeignModify	= gstorePlanForeignModify;
	routine->BeginForeignModify	= gstoreBeginForeignModify;
	routine->ExecForeignInsert	= gstoreExecForeignInsert;
	routine->ExecForeignDelete	= gstoreExecForeignDelete;
	routine->EndForeignModify	= gstoreEndForeignModify;

	routine->PlanDirectModify	= gstorePlanDirectModify;
    routine->BeginDirectModify	= gstoreBeginDirectModify;
    routine->IterateDirectModify = gstoreIterateDirectModify;
    routine->EndDirectModify	= gstoreEndDirectModify;

	PG_RETURN_POINTER(routine);
}
PG_FUNCTION_INFO_V1(pgstrom_gstore_fdw_handler);

/*
 * pgstrom_reggstore_in
 */
Datum
pgstrom_reggstore_in(PG_FUNCTION_ARGS)
{
	Datum	datum = regclassin(fcinfo);

	if (!relation_is_gstore_fdw(DatumGetObjectId(datum), true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						DatumGetObjectId(datum))));
	PG_RETURN_DATUM(datum);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_in);

/*
 * pgstrom_reggstore_out
 */
Datum
pgstrom_reggstore_out(PG_FUNCTION_ARGS)
{
	Oid		relid = PG_GETARG_OID(0);

	if (!relation_is_gstore_fdw(relid, true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						relid)));
	return regclassout(fcinfo);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_out);

/*
 * pgstrom_reggstore_recv
 */
Datum
pgstrom_reggstore_recv(PG_FUNCTION_ARGS)
{
	/* exactly the same as oidrecv, so share code */
	Datum	datum = oidrecv(fcinfo);

	if (!relation_is_gstore_fdw(DatumGetObjectId(datum), true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						DatumGetObjectId(datum))));
	PG_RETURN_DATUM(datum);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_recv);

/*
 * pgstrom_reggstore_send
 */
Datum
pgstrom_reggstore_send(PG_FUNCTION_ARGS)
{
	Oid		relid = PG_GETARG_OID(0);

	if (!relation_is_gstore_fdw(relid, true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("Relation %u is not a foreign table of gstore_fdw",
						relid)));
	/* Exactly the same as oidsend, so share code */
	return oidsend(fcinfo);
}
PG_FUNCTION_INFO_V1(pgstrom_reggstore_send);

/*
 * type_is_reggstore
 */
bool
type_is_reggstore(Oid type_oid)
{
	Oid			typinput;
	HeapTuple	tup;
	char	   *prosrc;
	char	   *probin;
	Datum		datum;
	bool		isnull;
	PGFunction	handler_fn;

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", type_oid);
	typinput = ((Form_pg_type) GETSTRUCT(tup))->typinput;
	ReleaseSysCache(tup);

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(typinput));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", typinput);

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc for C function %u", typinput);
	prosrc = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &isnull);
	if (isnull)
		elog(ERROR, "null probin for C function %u", typinput);
	probin = TextDatumGetCString(datum);
	ReleaseSysCache(tup);

	/* check whether function pointer is identical */
	handler_fn = load_external_function(probin, prosrc, true, NULL);
	if (handler_fn != pgstrom_reggstore_in)
		return false;
	/* ok, it is reggstore type */
	return true;
}

/*
 * load_normal_gstore_fdw
 */
static CUdeviceptr
load_normal_gstore_fdw(GpuContext *gcontext, Relation frel, TupleDesc tupdesc)
{
	GpuStoreChunk  *gs_chunk;
	List		   *gs_chunks_list = NIL;
	size_t			total_nitems = 0;
	size_t			length = 0;
	size_t			usage;
	cl_int			i, j, nchunks;
	ListCell	   *lc;
	kern_reggstore *kr_gstore;
	kern_data_store *kds_src;
	kern_data_store *kds_dst;
	CUresult		rc;
	CUdeviceptr		m_gstore;

	SpinLockAcquire(&gstore_head->lock);
	PG_TRY();
	{
		Snapshot		snapshot = GetActiveSnapshot();

		for (gs_chunk = gstore_fdw_first_chunk(frel, snapshot);
			 gs_chunk != NULL;
			 gs_chunk = gstore_fdw_next_chunk(gs_chunk, snapshot))
		{
			kds_src = gstore_fdw_mapped_chunk(gs_chunk);

			gs_chunks_list = lappend(gs_chunks_list, gs_chunk);
			total_nitems += kds_src->nitems;
			length += STROMALIGN(kds_src->length);
		}
		SpinLockRelease(&gstore_head->lock);
	}
	PG_CATCH();
	{
		SpinLockRelease(&gstore_head->lock);
		PG_RE_THROW();
	}
	PG_END_TRY();
	SpinLockRelease(&gstore_head->lock);

	if (gs_chunks_list == NIL)
		return 0UL;		/* No GpuStore chunks */

	/* allocation of managed memory */
	nchunks = list_length(gs_chunks_list);
	length += STROMALIGN(offsetof(kern_reggstore, kds[nchunks]));
	length += nchunks * STROMALIGN(offsetof(kern_data_store,
											colmeta[tupdesc->natts]));
	rc = gpuMemAllocManagedRaw(gcontext,
							   &m_gstore,
							   length,
							   CU_MEM_ATTACH_GLOBAL);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "out of managed memory");

	/* setup buffer */
	kr_gstore = (kern_reggstore *)m_gstore;
	kr_gstore->nchunks = nchunks;
	kr_gstore->total_nitems = total_nitems;

	usage = STROMALIGN(offsetof(kern_reggstore, kds[nchunks]));
	i=0;
	foreach (lc, gs_chunks_list)
	{
		size_t		values_offset;

		gs_chunk = lfirst(lc);

		kds_src = gstore_fdw_mapped_chunk(gs_chunk);
		kds_dst = (kern_data_store *)(m_gstore + usage);
		kr_gstore->kds[i] = kds_dst;

		Assert(kds_src->format == KDS_FORMAT_COLUMN);
		init_kernel_data_store(kds_dst,
							   tupdesc,
							   UINT_MAX,	/* dummy */
							   KDS_FORMAT_COLUMN,
							   kds_src->nitems);
		kds_dst->nitems = kds_src->nitems;
		values_offset = STROMALIGN(offsetof(kern_data_store,
											colmeta[kds_dst->ncols]));
		for (j=0; j < tupdesc->natts; j++)
		{
			Form_pg_attribute attr = tupdesc->attrs[j];
			kern_colmeta   *cmeta;
			cl_int			unitsz;

			Assert(attr->attnum > 0 && attr->attnum <= kds_src->ncols);
			cmeta = &kds_src->colmeta[attr->attnum - 1];
			Assert(kds_dst->colmeta[j].attbyval == cmeta->attbyval &&
				   kds_dst->colmeta[j].attalign == cmeta->attalign &&
				   kds_dst->colmeta[j].attlen   == cmeta->attlen &&
				   kds_dst->colmeta[j].attcacheoff == -1 &&
				   kds_dst->colmeta[j].atttypid == cmeta->atttypid &&
				   kds_dst->colmeta[j].atttypmod == cmeta->atttypmod);
			kds_dst->colmeta[j].values_offset = values_offset;
			kds_dst->colmeta[j].extra_sz = cmeta->extra_sz;

			if (cmeta->attlen < 0)
				unitsz = sizeof(cl_uint);
			else
				unitsz = TYPEALIGN(cmeta->attalign, cmeta->attlen);

			memcpy((char *)kds_dst + kds_dst->colmeta[j].values_offset,
				   (char *)kds_src + cmeta->values_offset,
				   unitsz * kds_src->nitems + cmeta->extra_sz);

			values_offset += STROMALIGN(unitsz * kds_src->nitems +
										cmeta->extra_sz);
		}
		kds_dst->length = values_offset;
		usage += STROMALIGN(kds_dst->length);
		i++;
	}
	return m_gstore;
}

/*
 * load_pinned_gstore_fdw
 */
static CUdeviceptr
load_pinned_gstore_fdw(GpuContext *gcontext, Relation frel, TupleDesc tupdesc)
{
	elog(ERROR, "%s: not supported yet", __FUNCTION__);
}

/*
 * pgstrom_load_gstore_fdw
 */
CUdeviceptr
pgstrom_load_gstore_fdw(GpuContext *gcontext, Oid gstore_oid)
{
	char	   *referenced;
	int			pinning;
	Relation	frel;
	Relation	grel = NULL;
	TupleDesc	tupdesc = NULL;
	CUdeviceptr	result;

	if (!relation_is_gstore_fdw(gstore_oid, true))
		elog(ERROR, "relation %s is not gstore_fdw foreign table",
			 get_rel_name(gstore_oid));

	frel = heap_open(gstore_oid, AccessShareLock);
	gstore_fdw_read_options(gstore_oid, &referenced, &pinning);
	if (!referenced)
	{
		tupdesc = RelationGetDescr(frel);
	}
	else
	{
		List	   *names;
		int			i, nattrs;

		if (pinning >= 0)
			elog(ERROR, "not a consistent gstore_fdw foreign table options");

		names = stringToQualifiedNameList(referenced);
		grel = heap_openrv(makeRangeVarFromNameList(names),
						   AccessShareLock);
		if (!relation_is_gstore_fdw(RelationGetRelid(frel), false))
			elog(ERROR, "\"%s\" is not a primary gstore_fdw foreign table",
				 RelationGetRelationName(frel));
		gstore_fdw_read_options(gstore_oid, &referenced, &pinning);
		Assert(!referenced);

		/* construct a pseudo tupdesc */
		nattrs = RelationGetNumberOfAttributes(frel);
		tupdesc = CreateTemplateTupleDesc(nattrs, false);
		for (i=0; i < nattrs; i++)
		{
			Form_pg_attribute attr = RelationGetDescr(frel)->attrs[i];
			AttrNumber	anum;

			anum = get_attnum(RelationGetRelid(grel),
							  NameStr(attr->attname));
			if (anum <= InvalidAttrNumber)
				elog(ERROR, "no \"%s\" attribute found on the gstore \"%s\"",
					 NameStr(attr->attname),
					 RelationGetRelationName(grel));

			memcpy(tupdesc->attrs[i],
				   RelationGetDescr(grel)->attrs[anum-1],
				   ATTRIBUTE_FIXED_PART_SIZE);
		}
		gstore_oid = RelationGetRelid(grel);
	}

	if (grel)
	{
		if (pinning)
			result = load_pinned_gstore_fdw(gcontext, grel, tupdesc);
		else
			result = load_normal_gstore_fdw(gcontext, grel, tupdesc);
		heap_close(grel, NoLock);
	}
	else
	{
		if (pinning)
			result = load_pinned_gstore_fdw(gcontext, frel, tupdesc);
		else
			result = load_normal_gstore_fdw(gcontext, frel, tupdesc);
	}
	heap_close(frel, NoLock);

	return result;
}

/*
 * pgstrom_startup_gstore_fdw
 */
static void
pgstrom_startup_gstore_fdw(void)
{
	bool		found;
	int			i;

	if (shmem_startup_next)
		(*shmem_startup_next)();

	gstore_head = ShmemInitStruct("GPU Store Control Structure",
								  offsetof(GpuStoreHead,
										   gs_chunks[gstore_max_nchunks]),
								  &found);
	if (found)
		elog(ERROR, "Bug? shared memory for gstore_fdw already built");
	gstore_maps = calloc(gstore_max_nchunks, sizeof(GpuStoreMap));
	if (!gstore_maps)
		elog(ERROR, "out of memory");
	SpinLockInit(&gstore_head->lock);
	dlist_init(&gstore_head->free_chunks);
	for (i=0; i < GSTORE_CHUNK_HASH_NSLOTS; i++)
		dlist_init(&gstore_head->active_chunks[i]);
	for (i=0; i < gstore_max_nchunks; i++)
	{
		GpuStoreChunk  *gs_chunk = &gstore_head->gs_chunks[i];

		memset(gs_chunk, 0, sizeof(GpuStoreChunk));
		gs_chunk->dsm_handle = UINT_MAX;

		dlist_push_tail(&gstore_head->free_chunks, &gs_chunk->chain);
	}
}

/*
 * pgstrom_init_gstore_fdw
 */
void
pgstrom_init_gstore_fdw(void)
{
	DefineCustomIntVariable("pg_strom.gstore_max_nchunks",
							"maximum number of gstore_fdw relations",
							NULL,
							&gstore_max_nchunks,
							2048,
							1024,
							INT_MAX,
							PGC_POSTMASTER,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	RequestAddinShmemSpace(MAXALIGN(offsetof(GpuStoreHead,
											 gs_chunks[gstore_max_nchunks])));
	shmem_startup_next = shmem_startup_hook;
	shmem_startup_hook = pgstrom_startup_gstore_fdw;

	RegisterXactCallback(gstoreXactCallback, NULL);
	//RegisterSubXactCallback(gstoreSubXactCallback, NULL);
}
