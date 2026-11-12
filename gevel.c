#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "access/gin.h"
#if PG_VERSION_NUM >= 90400
#include "utils/snapmgr.h"
#endif
#if PG_VERSION_NUM >= 90100
#include "access/gin_private.h"
#endif
#include "access/gist.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#if PG_VERSION_NUM >= 90200
#include "access/spgist_private.h"
#include "access/spgist.h"
#include "utils/datum.h"
#ifndef SGLT_GET_NEXTOFFSET
#define SGLT_GET_NEXTOFFSET(tup) ((tup)->nextOffset)
#endif
#endif
#include "access/heapam.h"
#include "catalog/index.h"
#if PG_VERSION_NUM >= 90600
#include <catalog/pg_am.h>
#endif
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "catalog/namespace.h"
#if PG_VERSION_NUM >= 80300
#include <tsearch/ts_utils.h>
#endif
#if PG_VERSION_NUM >= 100000
#include <utils/regproc.h>
#include <utils/varlena.h>
#endif
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include <fmgr.h>
#include <funcapi.h>
#include "utils/memutils.h"
#include <access/heapam.h>
#include <catalog/pg_type.h>
#include <access/relscan.h>
#if PG_VERSION_NUM >= 120000
#include <access/nbtree.h>
#include <access/brin.h>
#include <access/brin_revmap.h>
#include <access/brin_page.h>
#include <access/brin_tuple.h>

#if PG_VERSION_NUM >= 170000
#define GEVEL_BRIN_REVMAP_INIT(index, ppr) brinRevmapInitialize(index, ppr)
#define GEVEL_BRIN_GET_TUPLE(revmap, blk, pbuf, poff, psize, lock) \
	brinGetTupleForHeapBlock(revmap, blk, pbuf, poff, psize, lock)
#else
#define GEVEL_BRIN_REVMAP_INIT(index, ppr) brinRevmapInitialize(index, ppr, NULL)
#define GEVEL_BRIN_GET_TUPLE(revmap, blk, pbuf, poff, psize, lock) \
	brinGetTupleForHeapBlock(revmap, blk, pbuf, poff, psize, lock, NULL)
#endif

#endif


#ifdef PAGESIZE
#undef PAGESIZE
#endif
#define PAGESIZE	(BLCKSZ - MAXALIGN(sizeof(PageHeaderData) + sizeof(ItemIdData)))

#ifndef PG_NARGS
#define PG_NARGS() (fcinfo->nargs)
#endif

#if PG_VERSION_NUM >= 90600
#define	ISNULL		true
#define ISNOTNULL	false
#define heap_formtuple	heap_form_tuple
#else
#define ISNULL		'n'
#define ISNOTNULL	' '
#endif

#if PG_VERSION_NUM >= 110000
#define INDEX_GET_ATTR(r, i)	TupleDescAttr((r)->rd_att, (i))
#else
#define INDEX_GET_ATTR(r, i)	((r)->rd_att->attrs[(i)])
#endif

static Datum
safe_index_datum(Datum val, Form_pg_attribute att)
{
	if (att->attbyval)
		return val;
	if (att->attlen == -1)
		return PointerGetDatum(pg_detoast_datum_copy((struct varlena *) DatumGetPointer(val)));
	return datumCopy(val, att->attbyval, att->attlen);
}

static char
*t2c(text* in) {
	char *out=palloc(VARSIZE(in));

	memcpy(out, VARDATA(in), VARSIZE(in)-VARHDRSZ);
	out[ VARSIZE(in)-VARHDRSZ ] ='\0';
	return out;
}

typedef struct {
	int maxlevel;
	text	*txt;
	char	*ptr;
	int		len;
} IdxInfo;

static Relation checkOpenedRelation(Relation r, Oid PgAmOid);

#ifdef PG_MODULE_MAGIC
/* >= 8.2 */

PG_MODULE_MAGIC;

static Relation
gist_index_open(RangeVar *relvar) {
#if PG_VERSION_NUM < 90200
	Oid relOid = RangeVarGetRelid(relvar, false);
#else
	Oid relOid = RangeVarGetRelid(relvar, NoLock, false);
#endif
	return checkOpenedRelation(
				index_open(relOid, AccessShareLock), GIST_AM_OID);
}

#define	gist_index_close(r)	index_close((r), AccessShareLock)

static Relation
gin_index_open(RangeVar *relvar) {
#if PG_VERSION_NUM < 90200
	Oid relOid = RangeVarGetRelid(relvar, false);
#else
	Oid relOid = RangeVarGetRelid(relvar, NoLock, false);
#endif
	return checkOpenedRelation(
				index_open(relOid, AccessShareLock), GIN_AM_OID);
}

#define gin_index_close(r) index_close((r), AccessShareLock)

#if PG_VERSION_NUM >= 120000
static Relation
btree_index_open(RangeVar *relvar) {
	Oid relOid = RangeVarGetRelid(relvar, NoLock, false);
	return checkOpenedRelation(
				index_open(relOid, AccessShareLock), BTREE_AM_OID);
}

#define	btree_index_close(r)	index_close((r), AccessShareLock)

static Relation
brin_index_open(Oid indexOid)
{
	return checkOpenedRelation(
				index_open(indexOid, AccessShareLock), BRIN_AM_OID);
}

#define	brin_index_close(r)	index_close((r), AccessShareLock)
#endif

#else /* <8.2 */

static Relation
gist_index_open(RangeVar *relvar) {
	Relation rel = index_openrv(relvar);

	LockRelation(rel, AccessExclusiveLock);
	return checkOpenedRelation(rel, GIST_AM_OID);
}

static void
gist_index_close(Relation rel) {
	UnlockRelation(rel, AccessExclusiveLock);
	index_close(rel);
}

static Relation
gin_index_open(RangeVar *relvar) {
	Relation rel = index_openrv(relvar);

	LockRelation(rel, AccessShareLock);
	return checkOpenedRelation(rel, GIN_AM_OID);
}

static void
gin_index_close(Relation rel) {
	UnlockRelation(rel, AccessShareLock);
	index_close(rel);
}

static Relation
btree_index_open(RangeVar *relvar) {
	Relation rel = index_openrv(relvar);

	LockRelation(rel, AccessExclusiveLock);
	return checkOpenedRelation(rel, BTREE_AM_OID);
}

static void
btree_index_close(Relation rel) {
	UnlockRelation(rel, AccessExclusiveLock);
	index_close(rel);
}

#endif

#if PG_VERSION_NUM >= 160000
#define stringToQualifiedNameList(x,y)	stringToQualifiedNameList(x, NULL)
#elif PG_VERSION_NUM >= 80300
#define stringToQualifiedNameList(x,y)	stringToQualifiedNameList(x)
#endif

#if PG_VERSION_NUM < 80300
#define SET_VARSIZE(p,l)	VARATT_SIZEP(p)=(l)
#endif

static Relation
checkOpenedRelation(Relation r, Oid PgAmOid) {
	if ( r->rd_index == NULL )
		elog(ERROR, "Relation %s.%s is not an index",
					get_namespace_name(RelationGetNamespace(r)),
					RelationGetRelationName(r)
			);

#if PG_VERSION_NUM >= 110000
	if ( r->rd_rel->relkind == RELKIND_PARTITIONED_INDEX )
		elog(ERROR, "\"%s\" is a partitioned index; inspect individual partition indexes instead",
					RelationGetRelationName(r));
#endif

	if ( r->rd_rel->relam != PgAmOid )
		elog(ERROR, "Index %s.%s has wrong type",
					get_namespace_name(RelationGetNamespace(r)),
					RelationGetRelationName(r)
			);

	return r;
}

static void
gist_dumptree(Relation r, int level, BlockNumber blk, OffsetNumber coff, IdxInfo *info) {
	Buffer		buffer;
	Page		page;
	IndexTuple	which;
	ItemId		iid;
	OffsetNumber i,
				maxoff;
	char	   *pred;

	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	pred = (char *) palloc(sizeof(char) * level * 4 + 1);
	MemSet(pred, ' ', level*4);
	pred[level*4] = '\0';

	buffer = ReadBuffer(r, blk);
	LockBuffer(buffer, GIST_SHARE);
	page = (Page) BufferGetPage(buffer);

	if (PageIsNew(page) || GistPageIsDeleted(page))
	{
		UnlockReleaseBuffer(buffer);
		pfree(pred);
		return;
	}

	maxoff = PageGetMaxOffsetNumber(page);

	while ( (info->ptr-((char*)info->txt)) + level*4 + 256 >= info->len ) {
		int dist=info->ptr-((char*)info->txt);
		if (info->len > MaxAllocSize / 2)
			elog(ERROR, "out of memory in gist_dumptree");
		info->len *= 2;
		info->txt=(text*)repalloc(info->txt, info->len);
		info->ptr = ((char*)info->txt)+dist;
	}

	sprintf(info->ptr, "%s%d(l:%d) blk: %u numTuple: %d free: %db(%.2f%%) rightlink:%u (%s)\n",
		pred,
		coff,
		level,
		blk,
		(int) maxoff,
		(int) PageGetFreeSpace(page),
		100.0*(((float)PAGESIZE)-(float)PageGetFreeSpace(page))/((float)PAGESIZE),
		GistPageGetOpaque(page)->rightlink,
		( GistPageGetOpaque(page)->rightlink == InvalidBlockNumber ) ? "InvalidBlockNumber" : "OK" );
	info->ptr=strchr(info->ptr,'\0');
	pfree(pred);

	if (!GistPageIsLeaf(page) && ( info->maxlevel<0 || level<info->maxlevel ) )
	{
		BlockNumber *children = (BlockNumber *) palloc(sizeof(BlockNumber) * (maxoff + 1));
		int nchildren = 0;
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
			iid = PageGetItemId(page, i);
			if (!ItemIdIsValid(iid) || !ItemIdHasStorage(iid))
				continue;
			which = (IndexTuple) PageGetItem(page, iid);
			children[nchildren++] = ItemPointerGetBlockNumber(&(which->t_tid));
		}
		UnlockReleaseBuffer(buffer);

		for (i = 0; i < nchildren; i++) {
			gist_dumptree(r, level + 1, children[i], i + FirstOffsetNumber, info);
		}
		pfree(children);
	}
	else
	{
		UnlockReleaseBuffer(buffer);
	}
}

PG_FUNCTION_INFO_V1(gist_tree);
Datum	gist_tree(PG_FUNCTION_ARGS);
Datum
gist_tree(PG_FUNCTION_ARGS) {
	text	*name=PG_GETARG_TEXT_P(0);
	char *relname=t2c(name);
	RangeVar   *relvar;
	Relation		index;
	List	   *relname_list;
	IdxInfo	info;

	relname_list = stringToQualifiedNameList(relname, "gist_tree");
	relvar = makeRangeVarFromNameList(relname_list);
	index = gist_index_open(relvar);
	PG_FREE_IF_COPY(name,0);

	info.maxlevel = ( PG_NARGS() > 1 ) ? PG_GETARG_INT32(1) : -1;
	info.len=1024;
	info.txt=(text*)palloc( info.len );
	info.ptr=((char*)info.txt)+VARHDRSZ;
	*info.ptr = '\0';

	if (RelationGetNumberOfBlocks(index) == 0)
	{
		gist_index_close(index);
		pfree(relname);
		SET_VARSIZE(info.txt, VARHDRSZ);
		PG_RETURN_POINTER(info.txt);
	}

	gist_dumptree(index, 0, GIST_ROOT_BLKNO, 0, &info);

	gist_index_close(index);
	pfree(relname);

	SET_VARSIZE(info.txt, info.ptr-((char*)info.txt));
	PG_RETURN_POINTER(info.txt);
}

typedef struct {
	int		level;
	int		numpages;
	int		numleafpages;
	int		numtuple;
	int		numinvalidtuple;
	int		numleaftuple;
	uint64	tuplesize;
	uint64	leaftuplesize;
	uint64	totalsize;
} IdxStat;

static void
gist_stattree(Relation r, int level, BlockNumber blk, OffsetNumber coff, IdxStat *info) {
	Buffer		buffer;
	Page		page;
	IndexTuple	which;
	ItemId		iid;
	OffsetNumber i,
				maxoff;
	int			used;
	int			freeSpace;

	(void) coff;
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	buffer = ReadBuffer(r, blk);
	LockBuffer(buffer, GIST_SHARE);
	page = (Page) BufferGetPage(buffer);

	if (PageIsNew(page) || GistPageIsDeleted(page))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	maxoff = PageGetMaxOffsetNumber(page);
	freeSpace = (int) PageGetFreeSpace(page);
	used = ((int) PAGESIZE > freeSpace) ? ((int) PAGESIZE - freeSpace) : 0;

	info->numpages++;
	info->tuplesize += used;
	info->totalsize += BLCKSZ;
	info->numtuple += maxoff;
	if ( info->level < level )
		info->level = level;

	if (GistPageIsLeaf(page)) {
		info->numleafpages++;
		info->leaftuplesize += used;
		info->numleaftuple += maxoff;
		UnlockReleaseBuffer(buffer);
	} else {
		BlockNumber *children = (BlockNumber *) palloc(sizeof(BlockNumber) * (maxoff + 1));
		int nchildren = 0;
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
			iid = PageGetItemId(page, i);
			if (!ItemIdIsValid(iid) || !ItemIdHasStorage(iid))
				continue;
			which = (IndexTuple) PageGetItem(page, iid);
			if ( GistTupleIsInvalid(which) )
				info->numinvalidtuple++;
			children[nchildren++] = ItemPointerGetBlockNumber(&(which->t_tid));
		}
		UnlockReleaseBuffer(buffer);

		for (i = 0; i < nchildren; i++) {
			gist_stattree(r, level + 1, children[i], i + FirstOffsetNumber, info);
		}
		pfree(children);
	}
}

PG_FUNCTION_INFO_V1(gist_stat);
Datum	gist_stat(PG_FUNCTION_ARGS);
Datum
gist_stat(PG_FUNCTION_ARGS) {
	text	*name=PG_GETARG_TEXT_P(0);
	char *relname=t2c(name);
	RangeVar   *relvar;
	Relation		index;
	List	   *relname_list;
	IdxStat	info;
	text *out=(text*)palloc(1024);
	char *ptr=((char*)out)+VARHDRSZ;

	relname_list = stringToQualifiedNameList(relname, "gist_stat");
	relvar = makeRangeVarFromNameList(relname_list);
	index = gist_index_open(relvar);
	PG_FREE_IF_COPY(name,0);

	memset(&info, 0, sizeof(IdxStat));

	if (RelationGetNumberOfBlocks(index) == 0)
	{
		gist_index_close(index);
		pfree(relname);
		snprintf(ptr, 1024 - VARHDRSZ,
			"Number of levels:          0\n"
			"Number of pages:           0\n"
			"Number of leaf pages:      0\n"
			"Number of tuples:          0\n"
			"Number of invalid tuples:  0\n"
			"Number of leaf tuples:     0\n"
			"Total size of tuples:      0 bytes\n"
			"Total size of leaf tuples: 0 bytes\n"
			"Total size of index:       0 bytes\n");
		ptr = strchr(ptr, '\0');
		SET_VARSIZE(out, ptr - ((char*)out));
		PG_RETURN_POINTER(out);
	}

	gist_stattree(index, 0, GIST_ROOT_BLKNO, 0, &info);

	gist_index_close(index);
	pfree(relname);

	snprintf(ptr, 1024 - VARHDRSZ,
		"Number of levels:          %d\n"
		"Number of pages:           %d\n"
		"Number of leaf pages:      %d\n"
		"Number of tuples:          %d\n"
		"Number of invalid tuples:  %d\n"
		"Number of leaf tuples:     %d\n"
		"Total size of tuples:      "UINT64_FORMAT" bytes\n"
		"Total size of leaf tuples: "UINT64_FORMAT" bytes\n"
		"Total size of index:       "UINT64_FORMAT" bytes\n",
		info.level+1,
		info.numpages,
		info.numleafpages,
		info.numtuple,
		info.numinvalidtuple,
		info.numleaftuple,
		info.tuplesize,
		info.leaftuplesize,
		info.totalsize);

	ptr=strchr(ptr,'\0');

	SET_VARSIZE(out, ptr-((char*)out));
	PG_RETURN_POINTER(out);
}

typedef struct GPItem {
	BlockNumber		blk;
	OffsetNumber	offset;
	int				level;
	struct GPItem	*next;
} GPItem;

typedef struct {
	List		*relname_list;
	RangeVar	*relvar;
	Relation	index;
	Datum		*dvalues;
#if PG_VERSION_NUM >= 90600
	bool		*nulls;
#else
	char		*nulls;
#endif
	GPItem		*item;
	MemoryContext	tmpctx;
} TypeStorage;

static GPItem*
pushGPPage( FuncCallContext *funcctx, BlockNumber blk, int level ) {
	GPItem	*nitem;
	MemoryContext	 oldcontext;
	TypeStorage *st = (TypeStorage*)(funcctx->user_fctx);

	if (RelationGetNumberOfBlocks(st->index) <= blk)
		return st->item;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
	nitem = (GPItem*)palloc( sizeof(GPItem) );
	nitem->blk = blk;
	nitem->offset = FirstOffsetNumber;
	nitem->level = level;
	nitem->next = st->item;
	st->item = nitem;

	MemoryContextSwitchTo(oldcontext);
	return nitem;
}

static GPItem*
popGPPage( FuncCallContext *funcctx ) {
	TypeStorage *st = (TypeStorage*)(funcctx->user_fctx);
	GPItem  *oitem;

	if (st == NULL || st->item == NULL)
		return NULL;

	oitem = st->item;
	st->item = oitem->next;

	pfree( oitem );
	return st->item;
}

#if PG_VERSION_NUM >= 170000
#define TS_GET_TYPEVAL(s, i, v)	TupleDescAttr((s)->index->rd_att, (i))->v
#elif PG_VERSION_NUM >= 110000
#define TS_GET_TYPEVAL(s, i, v)	(s)->index->rd_att->attrs[(i)].v
#else
#define TS_GET_TYPEVAL(s, i, v)	(s)->index->rd_att->attrs[(i)]->v
#endif

static void close_call( FuncCallContext  *funcctx );

static void
gist_print_cleanup_callback(void *arg) {
	FuncCallContext *funcctx = (FuncCallContext *) arg;
	close_call(funcctx);
}

static void
setup_firstcall(FuncCallContext  *funcctx, text *name) {
	MemoryContext	 oldcontext;
	TypeStorage	 *st;
	char *relname=t2c(name);
	TupleDesc			tupdesc;
	char			attname[NAMEDATALEN];
	int i;
	MemoryContextCallback *cb;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	st=(TypeStorage*)palloc( sizeof(TypeStorage) );
	memset(st,0,sizeof(TypeStorage));
	st->relname_list = stringToQualifiedNameList(relname, "gist_tree");
	st->relvar = makeRangeVarFromNameList(st->relname_list);
	st->index = gist_index_open(st->relvar);
	st->tmpctx = AllocSetContextCreate(funcctx->multi_call_memory_ctx,
									   "gist_print_per_tuple",
									   ALLOCSET_DEFAULT_SIZES);
	funcctx->user_fctx = (void*)st;

#if PG_VERSION_NUM >= 120000
	tupdesc = CreateTemplateTupleDesc(st->index->rd_att->natts+2);
#else
	tupdesc = CreateTemplateTupleDesc(st->index->rd_att->natts+2, false);
#endif
	TupleDescInitEntry(tupdesc, 1, "level", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 2, "valid", BOOLOID, -1, 0);
	for (i = 0; i < st->index->rd_att->natts; i++) {
		sprintf(attname, "z%d", i+2);
		TupleDescInitEntry(
			tupdesc,
			i+3,
			attname,
			TS_GET_TYPEVAL(st, i, atttypid),
			TS_GET_TYPEVAL(st, i, atttypmod),
			TS_GET_TYPEVAL(st, i, attndims)
		);
	}
	BlessTupleDesc(tupdesc);

	st->dvalues = (Datum *) palloc((tupdesc->natts+2) * sizeof(Datum));
	st->nulls = palloc((tupdesc->natts+2) * sizeof(*st->nulls));

	funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);
#if PG_VERSION_NUM < 120000
	funcctx->slot = TupleDescGetSlot(tupdesc);
#endif

	cb = (MemoryContextCallback *) MemoryContextAlloc(funcctx->multi_call_memory_ctx, sizeof(MemoryContextCallback));
	cb->func = gist_print_cleanup_callback;
	cb->arg = (void *) funcctx;
	MemoryContextRegisterResetCallback(funcctx->multi_call_memory_ctx, cb);

	MemoryContextSwitchTo(oldcontext);
	pfree(relname);

	if (RelationGetNumberOfBlocks(st->index) > 0)
		pushGPPage(funcctx, GIST_ROOT_BLKNO, 1);
	else
		st->item = NULL;
}

static void
close_call( FuncCallContext  *funcctx ) {
	TypeStorage *st = (TypeStorage*)(funcctx->user_fctx);

	if (st == NULL)
		return;

	if (!IsAbortedTransactionBlockState())
	{
		while(st->item && popGPPage(funcctx));

		if (st->index) {
			gist_index_close(st->index);
			st->index = NULL;
		}
	}
	else
	{
		st->item = NULL;
		st->index = NULL;
	}

	if (st->dvalues) {
		pfree(st->dvalues);
		st->dvalues = NULL;
	}
	if (st->nulls) {
		pfree(st->nulls);
		st->nulls = NULL;
	}

	funcctx->user_fctx = NULL;
}

PG_FUNCTION_INFO_V1(gist_print);
Datum	gist_print(PG_FUNCTION_ARGS);
Datum
gist_print(PG_FUNCTION_ARGS) {
	FuncCallContext  *funcctx;
	TypeStorage	 *st;
	Datum result=(Datum)0;
	ItemId		  iid;
	IndexTuple	  ituple;
	HeapTuple	   htuple;
	int i;
	int max_natts;
	bool isnull;
	MemoryContext oldcontext;

	if (SRF_IS_FIRSTCALL()) {
		text	*name=PG_GETARG_TEXT_P(0);
		funcctx = SRF_FIRSTCALL_INIT();
		setup_firstcall(funcctx, name);
		PG_FREE_IF_COPY(name,0);
	}

	funcctx = SRF_PERCALL_SETUP();
	st = (TypeStorage*)(funcctx->user_fctx);

	if ( !st || !st->item ) {
		close_call(funcctx);
		SRF_RETURN_DONE(funcctx);
	}

	CHECK_FOR_INTERRUPTS();

	/* Reset per-tuple memory context from previous iteration (Fix 2) */
	MemoryContextReset(st->tmpctx);

	while (st->item != NULL)
	{
		Buffer buffer;
		Page page;
		bool isLeaf;
		bool isInvalid;
		BlockNumber downlink = InvalidBlockNumber;
		int current_level;

		buffer = ReadBuffer(st->index, st->item->blk);
		LockBuffer(buffer, GIST_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (PageIsNew(page) || GistPageIsDeleted(page) || st->item->offset > PageGetMaxOffsetNumber(page))
		{
			UnlockReleaseBuffer(buffer);
			popGPPage(funcctx);
			continue;
		}

		iid = PageGetItemId(page, st->item->offset);
		if (!ItemIdIsValid(iid) || !ItemIdHasStorage(iid))
		{
			st->item->offset = OffsetNumberNext(st->item->offset);
			UnlockReleaseBuffer(buffer);
			continue;
		}

		ituple = (IndexTuple) PageGetItem(page, iid);
		isLeaf = GistPageIsLeaf(page);
		isInvalid = (!isLeaf && GistTupleIsInvalid(ituple));
		if (!isLeaf)
			downlink = ItemPointerGetBlockNumber(&(ituple->t_tid));
		current_level = st->item->level;

#if PG_VERSION_NUM >= 110000
		max_natts = isLeaf
						? st->index->rd_att->natts
						: IndexRelationGetNumberOfKeyAttributes(st->index);
#else
		max_natts = st->index->rd_att->natts;
#endif

		/* Switch to per-tuple memory context for datum detoasting/copying */
		oldcontext = MemoryContextSwitchTo(st->tmpctx);

		st->dvalues[0] = Int32GetDatum(current_level);
		st->nulls[0] = ISNOTNULL;
		st->dvalues[1] = BoolGetDatum(isInvalid ? false : true);
		st->nulls[1] = ISNOTNULL;

		for (i = 2; i < funcctx->attinmeta->tupdesc->natts; i++)
		{
			if (isInvalid || (i - 1 > max_natts))
			{
				st->dvalues[i] = (Datum) 0;
				st->nulls[i] = ISNULL;
			}
			else
			{
				Datum val = index_getattr(ituple, i - 1, st->index->rd_att, &isnull);
				if (isnull)
				{
					st->dvalues[i] = (Datum) 0;
					st->nulls[i] = ISNULL;
				}
				else
				{
					/* Fix 5: detoast & safe copy into tmpctx */
					Form_pg_attribute att = INDEX_GET_ATTR(st->index, i - 2);
					st->dvalues[i] = safe_index_datum(val, att);
					st->nulls[i] = ISNOTNULL;
				}
			}
		}

		/* Advance offset for next inspection */
		st->item->offset = OffsetNumberNext(st->item->offset);

		/* Fix 3: Release buffer lock and pin BEFORE yielding control! */
		UnlockReleaseBuffer(buffer);

		/* If non-leaf, push downlink child page onto the stack */
		if (!isLeaf && BlockNumberIsValid(downlink))
			pushGPPage(funcctx, downlink, current_level + 1);

		htuple = heap_formtuple(funcctx->attinmeta->tupdesc, st->dvalues, st->nulls);
#if PG_VERSION_NUM >= 120000
		result = HeapTupleGetDatum(htuple);
#else
		result = TupleGetDatum(funcctx->slot, htuple);
#endif

		MemoryContextSwitchTo(oldcontext);

		SRF_RETURN_NEXT(funcctx, result);
	}

	close_call(funcctx);
	SRF_RETURN_DONE(funcctx);
}

typedef struct GinStatState {
	Relation		index;
	GinState		ginstate;
	OffsetNumber	attnum;

	Buffer			buffer;
	bool			bufferIsLocked;
	OffsetNumber	offset;
	Datum			curval;
#if PG_VERSION_NUM >= 90100
	GinNullCategory	category;
#endif
	Datum			dvalues[2];
#if PG_VERSION_NUM >= 110000
	bool			nulls[2];
#else
	char			nulls[2];
#endif
	MemoryContext	tmpctx;
} GinStatState;

static bool
moveRightIfItNeeded( GinStatState *st )
{
	Page page = BufferGetPage(st->buffer);

	if ( st->offset > PageGetMaxOffsetNumber(page) ) {
		/*
		* We scaned the whole page, so we should take right page
		*/
		BlockNumber blkno = GinPageGetOpaque(page)->rightlink;

		if ( GinPageRightMost(page) )
			return false;  /* no more page */

		LockBuffer(st->buffer, GIN_UNLOCK);
		st->bufferIsLocked = false;
		st->buffer = ReleaseAndReadBuffer(st->buffer, st->index, blkno);
		LockBuffer(st->buffer, GIN_SHARE);
		st->bufferIsLocked = true;
		st->offset = FirstOffsetNumber;
	}

	return true;
}

/*
 * Refinds a previois position, at returns it has correctly
 * set offset and buffer is locked
 */
static bool
refindPosition(GinStatState *st)
{
	Page	page;

	/* find left if needed (it causes only for first search) */
	for (;;) {
		IndexTuple  itup;
		BlockNumber blkno;

		LockBuffer(st->buffer, GIN_SHARE);
		st->bufferIsLocked = true;

		page = BufferGetPage(st->buffer);
		if (GinPageIsLeaf(page))
			break;

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
		blkno = GinItemPointerGetBlockNumber(&(itup)->t_tid);

		LockBuffer(st->buffer,GIN_UNLOCK);
		st->bufferIsLocked = false;
		st->buffer = ReleaseAndReadBuffer(st->buffer, st->index, blkno);
	}

	if (st->offset == InvalidOffsetNumber) {
		return (PageGetMaxOffsetNumber(page) >= FirstOffsetNumber ) ? true : false; /* first one */
	}

	for(;;) {
		int				cmp;
#if PG_VERSION_NUM >= 90100
		GinNullCategory	category;
#elif PG_VERSION_NUM < 80400
		bool			isnull = false;
#endif
		Datum			datum;
		IndexTuple		itup;

		if (moveRightIfItNeeded(st)==false)
			return false;

		page = BufferGetPage(st->buffer);

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, st->offset));
#if PG_VERSION_NUM >= 90100
		datum = gintuple_get_key(&st->ginstate, itup, &category);
		cmp = ginCompareAttEntries(&st->ginstate,
									st->attnum + 1, st->curval, st->category,
									gintuple_get_attrnum(&st->ginstate, itup), datum, category);
#elif PG_VERSION_NUM >= 80400
		datum = gin_index_getattr(&st->ginstate, itup);

		cmp = compareAttEntries(&st->ginstate,
									st->attnum + 1, st->curval,
									gintuple_get_attrnum(&st->ginstate, itup), datum);
#else
		datum = index_getattr(itup, FirstOffsetNumber, st->ginstate.tupdesc, &isnull);

		cmp = DatumGetInt32(
				FunctionCall2(
						&st->ginstate.compareFn,
						st->curval,
						datum
					));
#endif
		if ( cmp == 0 )
		{
			if ( st->curval && !TS_GET_TYPEVAL(st, st->attnum, attbyval) )
				pfree( (void*) st->curval );
			st->curval = (Datum)0;
			return true;
		}

		st->offset++;
	}

	return false;
}

static void gin_close_call(FuncCallContext *funcctx);

static void
gin_stat_cleanup_callback(void *arg) {
	FuncCallContext *funcctx = (FuncCallContext *) arg;
	gin_close_call(funcctx);
}

static void
gin_setup_firstcall(FuncCallContext  *funcctx, text *name, int attnum) {
	MemoryContext	 oldcontext;
	GinStatState	 *st;
	char *relname=t2c(name);
	TupleDesc			tupdesc;
	MemoryContextCallback *cb;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	st=(GinStatState*)palloc( sizeof(GinStatState) );
	memset(st,0,sizeof(GinStatState));
	st->index = gin_index_open(
		 makeRangeVarFromNameList(stringToQualifiedNameList(relname, "gin_stat")));
	initGinState( &st->ginstate, st->index );
	st->tmpctx = AllocSetContextCreate(funcctx->multi_call_memory_ctx,
									   "gin_stat_per_tuple",
									   ALLOCSET_DEFAULT_SIZES);

#if PG_VERSION_NUM >= 80400
	if (attnum < 0 || attnum >= st->index->rd_att->natts)
		elog(ERROR,"Wrong column's number");
	st->attnum = attnum;
#else
	st->attnum = 0;
#endif

	funcctx->user_fctx = (void*)st;

#if PG_VERSION_NUM >= 120000
	tupdesc = CreateTemplateTupleDesc(2);
#else
	tupdesc = CreateTemplateTupleDesc(2, false);
#endif
	TupleDescInitEntry(tupdesc, 1, "value",
			TS_GET_TYPEVAL(st, st->attnum, atttypid),
			TS_GET_TYPEVAL(st, st->attnum, atttypmod),
			TS_GET_TYPEVAL(st, st->attnum, attndims));
	TupleDescInitEntry(tupdesc, 2, "nrow", INT4OID, -1, 0);
	BlessTupleDesc(tupdesc);

	memset( st->nulls, ISNOTNULL, 2*sizeof(*st->nulls) );

#if PG_VERSION_NUM >= 120000
	funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);
#else
	funcctx->slot = TupleDescGetSlot(tupdesc);
#endif

	cb = (MemoryContextCallback *) MemoryContextAlloc(funcctx->multi_call_memory_ctx, sizeof(MemoryContextCallback));
	cb->func = gin_stat_cleanup_callback;
	cb->arg = (void *) funcctx;
	MemoryContextRegisterResetCallback(funcctx->multi_call_memory_ctx, cb);

	MemoryContextSwitchTo(oldcontext);
	pfree(relname);

	st->offset = InvalidOffsetNumber;
	if (RelationGetNumberOfBlocks(st->index) <= GIN_ROOT_BLKNO)
		st->buffer = InvalidBuffer;
	else
		st->buffer = ReadBuffer( st->index, GIN_ROOT_BLKNO );
	st->bufferIsLocked = false;
}

static void
gin_close_call(FuncCallContext *funcctx) {
	GinStatState *st = (GinStatState*)(funcctx->user_fctx);

	if (st == NULL)
		return;

	if (!IsAbortedTransactionBlockState())
	{
		if (BufferIsValid(st->buffer)) {
			if (st->bufferIsLocked)
				UnlockReleaseBuffer(st->buffer);
			else
				ReleaseBuffer(st->buffer);
			st->buffer = InvalidBuffer;
			st->bufferIsLocked = false;
		}
		if (st->index) {
			gin_index_close(st->index);
			st->index = NULL;
		}
	}
	else
	{
		st->buffer = InvalidBuffer;
		st->bufferIsLocked = false;
		st->index = NULL;
	}

	if (st->curval && !TS_GET_TYPEVAL(st, st->attnum, attbyval))
	{
		pfree((void*) st->curval);
		st->curval = (Datum)0;
	}

	funcctx->user_fctx = NULL;
}

static void
processTuple( FuncCallContext  *funcctx,  GinStatState *st, IndexTuple itup ) {
	MemoryContext		oldcontext;
#if PG_VERSION_NUM >= 90100
	Datum				key;
#elif PG_VERSION_NUM < 80400
	bool				isnull;
#endif

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

#if PG_VERSION_NUM >= 90100
	key = gintuple_get_key(&st->ginstate, itup, &st->category);

	if (st->category != GIN_CAT_NORM_KEY)
		st->curval = (Datum)0;
	else
#endif
	st->curval = datumCopy(
#if PG_VERSION_NUM >= 90100
					key,
#elif PG_VERSION_NUM >= 80400
					gin_index_getattr(&st->ginstate, itup),
#else
					index_getattr(itup, FirstOffsetNumber, st->ginstate.tupdesc, &isnull),
#endif
					TS_GET_TYPEVAL(st, st->attnum, attbyval),
					TS_GET_TYPEVAL(st, st->attnum, attlen));
	MemoryContextSwitchTo(oldcontext);

	st->dvalues[0] = st->curval;
#if PG_VERSION_NUM >= 90100
	/* do no distiguish various null category */
	st->nulls[0] = (st->category == GIN_CAT_NORM_KEY) ? ISNOTNULL : ISNULL;
#endif

	if ( GinIsPostingTree(itup) ) {
		BlockNumber	rootblkno = GinGetPostingTree(itup);
#if PG_VERSION_NUM >= 90400
		GinBtreeData	btree;
		GinBtreeStack	*stack;
		ItemPointerData	minItem;
		int				nlist;
		ItemPointer		list;
#else
		GinPostingTreeScan *gdi;
		Buffer			entrybuffer;
#endif
		Page		page;
		uint32		predictNumber;

		LockBuffer(st->buffer, GIN_UNLOCK);
		st->bufferIsLocked = false;
#if PG_VERSION_NUM >= 90400
#if PG_VERSION_NUM >= 170000
		stack = ginScanBeginPostingTree(&btree, st->index, rootblkno);
#elif PG_VERSION_NUM >= 90600
		stack = ginScanBeginPostingTree(&btree, st->index, rootblkno, NULL);
#else
		stack = ginScanBeginPostingTree(&btree, st->index, rootblkno);
#endif
		page = BufferGetPage(stack->buffer);
		ItemPointerSetMin(&minItem);
		list = GinDataLeafPageGetItems(page, &nlist, minItem);
		pfree(list);
		predictNumber = stack->predictNumber;
		{
			int64 totalRows = (int64) predictNumber * (int64) nlist;
			st->dvalues[1] = Int32GetDatum( (totalRows > PG_INT32_MAX) ? PG_INT32_MAX : (int32) totalRows );
		}
#elif PG_VERSION_NUM >= 90100
		gdi = ginPrepareScanPostingTree(st->index, rootblkno, TRUE);
		entrybuffer = ginScanBeginPostingTree(gdi);
		page = BufferGetPage(entrybuffer);
		predictNumber = gdi->stack->predictNumber;
		st->dvalues[1] = Int32GetDatum( predictNumber * GinPageGetOpaque(page)->maxoff );
#else
		gdi = prepareScanPostingTree(st->index, rootblkno, TRUE);
		entrybuffer = scanBeginPostingTree(gdi);
		page = BufferGetPage(entrybuffer);
		predictNumber = gdi->stack->predictNumber;
		st->dvalues[1] = Int32GetDatum( predictNumber * GinPageGetOpaque(page)->maxoff );
#endif

#if PG_VERSION_NUM < 90400
		LockBuffer(entrybuffer, GIN_UNLOCK);
		freeGinBtreeStack(gdi->stack);
		pfree(gdi);
#else
		LockBuffer(stack->buffer, GIN_UNLOCK);
		freeGinBtreeStack(stack);
#endif
	} else {
		st->dvalues[1] = Int32GetDatum( GinGetNPosting(itup) );
		LockBuffer(st->buffer, GIN_UNLOCK);
		st->bufferIsLocked = false;
	}
}

PG_FUNCTION_INFO_V1(gin_stat);
Datum	gin_stat(PG_FUNCTION_ARGS);
Datum
gin_stat(PG_FUNCTION_ARGS) {
	FuncCallContext  *funcctx;
	GinStatState	 *st;
	Datum result=(Datum)0;
	IndexTuple	  ituple;
	HeapTuple	   htuple;
	Page page;

	if (SRF_IS_FIRSTCALL()) {
		text	*name=PG_GETARG_TEXT_P(0);
		funcctx = SRF_FIRSTCALL_INIT();
		gin_setup_firstcall(funcctx, name, (PG_NARGS()==2) ? PG_GETARG_INT32(1) : 0 );
		PG_FREE_IF_COPY(name,0);
	}

	funcctx = SRF_PERCALL_SETUP();
	st = (GinStatState*)(funcctx->user_fctx);

	if ( !st || !BufferIsValid(st->buffer) || refindPosition(st) == false ) {
		gin_close_call(funcctx);
		SRF_RETURN_DONE(funcctx);
	}

	/* Reset per-tuple memory context from previous iteration (Fix 2) */
	MemoryContextReset(st->tmpctx);

	for(;;) {
		CHECK_FOR_INTERRUPTS();

		st->offset++;

		if (moveRightIfItNeeded(st)==false) {
			gin_close_call(funcctx);
			SRF_RETURN_DONE(funcctx);
		}

		page = BufferGetPage(st->buffer);
		ituple = (IndexTuple) PageGetItem(page, PageGetItemId(page, st->offset));

#if PG_VERSION_NUM >= 80400
		if (st->attnum + 1 == gintuple_get_attrnum(&st->ginstate, ituple))
			break;
		if (gintuple_get_attrnum(&st->ginstate, ituple) > st->attnum + 1) {
			gin_close_call(funcctx);
			SRF_RETURN_DONE(funcctx);
		}
#else
		break;
#endif
	}

	processTuple( funcctx,  st, ituple );

	{
		MemoryContext oldcontext = MemoryContextSwitchTo(st->tmpctx);
		htuple = heap_formtuple(funcctx->attinmeta->tupdesc, st->dvalues, st->nulls);
#if PG_VERSION_NUM >= 120000
		result = HeapTupleGetDatum(htuple);
#else
		result = TupleGetDatum(funcctx->slot, htuple);
#endif
		MemoryContextSwitchTo(oldcontext);
	}

	SRF_RETURN_NEXT(funcctx, result);
}

PG_FUNCTION_INFO_V1(gin_count_estimate);
Datum gin_count_estimate(PG_FUNCTION_ARGS);
#if PG_VERSION_NUM >= 80300
Datum
gin_count_estimate(PG_FUNCTION_ARGS) {
	text			*name=PG_GETARG_TEXT_PP(0);
	Relation		index;
	IndexScanDesc	scan;
	int64			count = 0;
	ScanKeyData		key;
	List			*relname_list;
	RangeVar		*relvar;
#if PG_VERSION_NUM >= 80400
	TIDBitmap		*bitmap;
#else
#define	MAXTIDS		1024
	ItemPointerData	tids[MAXTIDS];
	int32			returned_tids;
	bool			more;
#endif

	relname_list = textToQualifiedNameList(name);
	relvar = makeRangeVarFromNameList(relname_list);
	index = gin_index_open(relvar);

	if ( index->rd_opcintype[0] != TSVECTOROID ) {
		gin_index_close(index);
		PG_FREE_IF_COPY(name, 0);
		elog(ERROR, "Column type is not a tsvector");
	}

#if PG_VERSION_NUM >= 80400
	bitmap = tbm_create(work_mem * 1024L
#if PG_VERSION_NUM >= 100000
						, NULL
#endif
						);
#endif

	key.sk_flags = 0;
	key.sk_attno = 1;
	key.sk_strategy	= TSearchStrategyNumber;
	key.sk_subtype  = 0;
	key.sk_argument = PG_GETARG_DATUM(1);

	fmgr_info( F_TS_MATCH_VQ , &key.sk_func );

#if PG_VERSION_NUM >= 90100
#if PG_VERSION_NUM >= 180000
	scan = index_beginscan_bitmap(index, SnapshotSelf, NULL, 1);
#elif PG_VERSION_NUM >= 90400
	scan = index_beginscan_bitmap(index, SnapshotSelf, 1);
#else
	scan = index_beginscan_bitmap(index, SnapshotNow, 1);
#endif
	index_rescan(scan, &key, 1, NULL, 0);

	count = index_getbitmap(scan, bitmap);
	tbm_free(bitmap);
#elif PG_VERSION_NUM >= 80400
	scan = index_beginscan_bitmap(index, SnapshotNow, 1, &key);

	count = index_getbitmap(scan, bitmap);
	tbm_free(bitmap);
#else
	scan = index_beginscan_multi(index, SnapshotNow, 1, &key);

	do {
		more = index_getmulti(scan, tids, MAXTIDS, &returned_tids);
		count += returned_tids;
	} while(more);
#endif

	index_endscan( scan );
	gin_index_close(index);
	PG_FREE_IF_COPY(name, 0);

	PG_RETURN_INT64(count);
}
#else
Datum
gin_count_estimate(PG_FUNCTION_ARGS) {
	elog(NOTICE, "Function is not working under PgSQL < 8.3");

	PG_RETURN_INT64(0);
}
#endif

PG_FUNCTION_INFO_V1(spgist_stat);
Datum spgist_stat(PG_FUNCTION_ARGS);
Datum
spgist_stat(PG_FUNCTION_ARGS)
{
#if PG_VERSION_NUM < 90200
	elog(NOTICE, "Function is not working under PgSQL < 9.2");

	PG_RETURN_DATUM(CStringGetTextDatum("???"));
#else
	text	   *name = PG_GETARG_TEXT_P(0);
	RangeVar   *relvar;
	Relation	index;
	BlockNumber blkno;
	BlockNumber totalPages = 0,
				innerPages = 0,
				leafPages = 0,
				emptyPages = 0,
				deletedPages = 0;
	double	  usedSpace = 0.0,
			  usedLeafSpace = 0.0,
			  usedInnerSpace = 0.0;
	char		res[1024];
	int		 bufferSize = -1;
	int64	   innerTuples = 0,
				leafTuples = 0,
				nAllTheSame = 0,
				nLeafPlaceholder = 0,
				nInnerPlaceholder = 0,
				nLeafRedirect = 0,
				nInnerRedirect = 0;

#define IS_INDEX(r) ((r)->rd_rel->relkind == RELKIND_INDEX)
#define IS_SPGIST(r) ((r)->rd_rel->relam == SPGIST_AM_OID)

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(name));
	index = relation_openrv(relvar, AccessShareLock);

#if PG_VERSION_NUM >= 110000
	if (index->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		elog(ERROR, "\"%s\" is a partitioned index; inspect individual partition indexes instead",
			 RelationGetRelationName(index));
#endif

	if (!IS_INDEX(index) || !IS_SPGIST(index))
		elog(ERROR, "relation \"%s\" is not an SPGiST index",
			 RelationGetRelationName(index));

	totalPages = RelationGetNumberOfBlocks(index);

	if (totalPages <= 1)
	{
		index_close(index, AccessShareLock);
		snprintf(res, sizeof(res),
				 "totalPages:         0\n"
				 "deletedPages:       0\n"
				 "innerPages:         0\n"
				 "leafPages:          0\n"
				 "emptyPages:         0\n"
				 "usedSpace:          0.00 kbytes\n"
				 "usedInnerSpace:     0.00 kbytes\n"
				 "usedLeafSpace:      0.00 kbytes\n"
				 "freeSpace:          0.00 kbytes\n"
				 "fillRatio:          0.00%%\n"
				 "leafTuples:         0\n"
				 "innerTuples:        0\n"
				 "innerAllTheSame:    0\n"
				 "leafPlaceholders:   0\n"
				 "innerPlaceholders:  0\n"
				 "leafRedirects:      0\n"
				 "innerRedirects:     0");
		PG_FREE_IF_COPY(name, 0);
		PG_RETURN_DATUM(CStringGetTextDatum(res));
	}

	for (blkno = SPGIST_ROOT_BLKNO; blkno < totalPages; blkno++)
	{
		Buffer	  buffer;
		Page		page;
		int		 pageFree;

		CHECK_FOR_INTERRUPTS();

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);

		if (PageIsNew(page) || SpGistPageIsDeleted(page))
		{
			deletedPages++;
			UnlockReleaseBuffer(buffer);
			continue;
		}

		if (SpGistPageIsLeaf(page))
		{
			leafPages++;
			leafTuples += PageGetMaxOffsetNumber(page);
			nLeafPlaceholder += SpGistPageGetOpaque(page)->nPlaceholder;
			nLeafRedirect += SpGistPageGetOpaque(page)->nRedirection;
		}
		else
		{
			int	 i,
					max;

			innerPages++;
			max = PageGetMaxOffsetNumber(page);
			innerTuples += max;
			nInnerPlaceholder += SpGistPageGetOpaque(page)->nPlaceholder;
			nInnerRedirect += SpGistPageGetOpaque(page)->nRedirection;
			for (i = FirstOffsetNumber; i <= max; i++)
			{
				ItemId iid = PageGetItemId(page, i);
				if (ItemIdHasStorage(iid))
				{
					SpGistInnerTuple it = (SpGistInnerTuple) PageGetItem(page, iid);
					if (it->tupstate == SPGIST_LIVE && it->allTheSame)
						nAllTheSame++;
				}
			}
		}

		if (bufferSize < 0)
			bufferSize = BufferGetPageSize(buffer)
				- MAXALIGN(sizeof(SpGistPageOpaqueData))
				- SizeOfPageHeaderData;

		pageFree = PageGetExactFreeSpace(page);

		usedSpace += bufferSize - pageFree;
		if (SpGistPageIsLeaf(page))
			usedLeafSpace += bufferSize - pageFree;
		else
			usedInnerSpace += bufferSize - pageFree;

		if (pageFree == bufferSize)
			emptyPages++;

		UnlockReleaseBuffer(buffer);
	}

	index_close(index, AccessShareLock);

	totalPages--;			   /* discount metapage */

	snprintf(res, sizeof(res),
			 "totalPages:         %u\n"
			 "deletedPages:       %u\n"
			 "innerPages:         %u\n"
			 "leafPages:          %u\n"
			 "emptyPages:         %u\n"
			 "usedSpace:          %.2f kbytes\n"
			 "usedInnerSpace:     %.2f kbytes\n"
			 "usedLeafSpace:      %.2f kbytes\n"
			 "freeSpace:          %.2f kbytes\n"
			 "fillRatio:          %.2f%%\n"
			 "leafTuples:         " INT64_FORMAT "\n"
			 "innerTuples:        " INT64_FORMAT "\n"
			 "innerAllTheSame:    " INT64_FORMAT "\n"
			 "leafPlaceholders:   " INT64_FORMAT "\n"
			 "innerPlaceholders:  " INT64_FORMAT "\n"
			 "leafRedirects:      " INT64_FORMAT "\n"
			 "innerRedirects:     " INT64_FORMAT,
			 totalPages, deletedPages, innerPages, leafPages, emptyPages,
			 usedSpace / 1024.0,
			 usedInnerSpace / 1024.0,
			 usedLeafSpace / 1024.0,
			 (((double) bufferSize) * ((double) totalPages) - usedSpace) / 1024,
			 100.0 * (usedSpace / (((double) bufferSize) * ((double) totalPages))),
			 leafTuples, innerTuples, nAllTheSame,
			 nLeafPlaceholder, nInnerPlaceholder,
			 nLeafRedirect, nInnerRedirect);

	PG_FREE_IF_COPY(name, 0);
	PG_RETURN_DATUM(CStringGetTextDatum(res));
#endif
}

#if PG_VERSION_NUM >= 90200

typedef struct SPGistPrintStackElem {
	ItemPointerData		iptr;
	int16				nlabel;
	int					level;
} SPGistPrintStackElem;

typedef struct SPGistPrint {
	SpGistState	state;
	Relation	index;
	Datum		dvalues[8 /* see CreateTemplateTupleDesc call */];
#if PG_VERSION_NUM >= 110000
	bool			nulls[8];
#else
	char			nulls[8];
#endif
	List		*stack;
	MemoryContext	tmpctx;
} SPGistPrint;

static Oid
resolve_spgist_type(Relation index, SpGistTypeDesc *typedesc)
{
	if (typedesc->type != VOIDOID && get_typtype(typedesc->type) == TYPTYPE_PSEUDO)
	{
		Oid concreteType = INDEX_GET_ATTR(index, 0)->atttypid;
		typedesc->type = concreteType;
		get_typlenbyval(concreteType, &typedesc->attlen, &typedesc->attbyval);
	}
	return typedesc->type;
}

static Datum
safe_spgist_datum(Datum val, SpGistTypeDesc *typedesc)
{
	if (typedesc->attbyval)
		return val;
	if (typedesc->attlen == -1)
		return PointerGetDatum(pg_detoast_datum_copy((struct varlena *) DatumGetPointer(val)));
	return datumCopy(val, typedesc->attbyval, typedesc->attlen);
}

static void
pushSPGistPrint(FuncCallContext *funcctx, SPGistPrint *prst, ItemPointer ip, int level) {
	MemoryContext	oldcontext;
	SPGistPrintStackElem	*e;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	e = palloc(sizeof(*e));
	e->iptr = *ip;
	e->nlabel = 0;
	e->level = level;
	prst->stack = lcons(e, prst->stack);

	MemoryContextSwitchTo(oldcontext);
}

static void
spgist_print_cleanup_callback(void *arg)
{
	SPGistPrint *prst = (SPGistPrint *) arg;
	if (prst && prst->index != NULL)
	{
		if (!IsAbortedTransactionBlockState())
			index_close(prst->index, AccessShareLock);
		prst->index = NULL;
	}
}

static void
close_spgist_print(SPGistPrint *prst) {
	if (prst && prst->index != NULL)
	{
		if (!IsAbortedTransactionBlockState())
			index_close(prst->index, AccessShareLock);
		prst->index = NULL;
	}
}
#endif

PG_FUNCTION_INFO_V1(spgist_print);
Datum spgist_print(PG_FUNCTION_ARGS);
Datum
spgist_print(PG_FUNCTION_ARGS)
{
#if PG_VERSION_NUM < 90200
	elog(NOTICE, "Function is not working under PgSQL < 9.2");

	PG_RETURN_DATUM(CStringGetTextDatum("???"));
#else
	FuncCallContext			*funcctx;
	SPGistPrint				*prst;
	SPGistPrintStackElem	*s;
	HeapTuple				htuple;
	Datum					result;
	MemoryContext			oldcontext;

	if (SRF_IS_FIRSTCALL()) {
		text			*name=PG_GETARG_TEXT_P(0);
		RangeVar		*relvar;
		Relation		index;
		ItemPointerData	ipd;
		TupleDesc		tupdesc;
		MemoryContextCallback *cb;

		funcctx = SRF_FIRSTCALL_INIT();
		relvar = makeRangeVarFromNameList(textToQualifiedNameList(name));
		index = relation_openrv(relvar, AccessShareLock);

#if PG_VERSION_NUM >= 110000
		if (index->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
			elog(ERROR, "\"%s\" is a partitioned index; inspect individual partition indexes instead",
				 RelationGetRelationName(index));
#endif

		if (!IS_INDEX(index) || !IS_SPGIST(index))
			elog(ERROR, "relation \"%s\" is not an SPGiST index",
				 RelationGetRelationName(index));

		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		prst = palloc0(sizeof(*prst));

		prst->index = index;
		initSpGistState(&prst->state, index);
		prst->tmpctx = AllocSetContextCreate(funcctx->multi_call_memory_ctx,
											 "spgist_print_per_tuple",
											 ALLOCSET_DEFAULT_SIZES);

		{
			Oid prefixType = resolve_spgist_type(index, &prst->state.attPrefixType);
			Oid labelType = prst->state.attLabelType.type;
			Oid leafType = resolve_spgist_type(index, &prst->state.attLeafType);

#if PG_VERSION_NUM >= 120000
			tupdesc = CreateTemplateTupleDesc(3 /* types */ + 1 /* level */ + 1 /* nlabel */ +  2 /* tids */ + 1);
#else
			tupdesc = CreateTemplateTupleDesc(3 /* types */ + 1 /* level */ + 1 /* nlabel */ +  2 /* tids */ + 1, false);
#endif
			TupleDescInitEntry(tupdesc, 1, "tid", TIDOID, -1, 0);
			TupleDescInitEntry(tupdesc, 2, "allthesame", BOOLOID, -1, 0);
			TupleDescInitEntry(tupdesc, 3, "node", INT4OID, -1, 0);
			TupleDescInitEntry(tupdesc, 4, "level", INT4OID, -1, 0);
			TupleDescInitEntry(tupdesc, 5, "tid_pointer", TIDOID, -1, 0);
			TupleDescInitEntry(tupdesc, 6, "prefix",
					(prefixType == VOIDOID) ? INT4OID : prefixType, -1, 0);
			TupleDescInitEntry(tupdesc, 7, "label",
					(labelType == VOIDOID) ? INT4OID : labelType, -1, 0);
			TupleDescInitEntry(tupdesc, 8, "leaf",
					(leafType == VOIDOID) ? INT4OID : leafType, -1, 0);
		}

		BlessTupleDesc(tupdesc);

		funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);
#if PG_VERSION_NUM < 120000
		funcctx->slot = TupleDescGetSlot(tupdesc);
#endif

		funcctx->user_fctx = (void*)prst;

		cb = (MemoryContextCallback *) palloc(sizeof(MemoryContextCallback));
		cb->func = spgist_print_cleanup_callback;
		cb->arg = (void *) prst;
		MemoryContextRegisterResetCallback(funcctx->multi_call_memory_ctx, cb);

		prst->stack = NIL;
		if (RelationGetNumberOfBlocks(index) > SPGIST_ROOT_BLKNO)
		{
			ItemPointerSet(&ipd, SPGIST_ROOT_BLKNO, FirstOffsetNumber);
			pushSPGistPrint(funcctx, prst, &ipd, 1);
			if (RelationGetNumberOfBlocks(index) > SPGIST_NULL_BLKNO)
			{
				ItemPointerSet(&ipd, SPGIST_NULL_BLKNO, FirstOffsetNumber);
				pushSPGistPrint(funcctx, prst, &ipd, 1);
			}
		}

		MemoryContextSwitchTo(oldcontext);

		PG_FREE_IF_COPY(name,0);
	}

	funcctx = SRF_PERCALL_SETUP();
	prst = (SPGistPrint*)(funcctx->user_fctx);

	/* Reset per-tuple memory context from previous call (Fix 2) */
	MemoryContextReset(prst->tmpctx);

next:
	for(;;) {
		if ( prst->stack == NIL ) {
			close_spgist_print(prst);
			SRF_RETURN_DONE(funcctx);
		}

		CHECK_FOR_INTERRUPTS();

		s = (SPGistPrintStackElem*)linitial(prst->stack);
		prst->stack = list_delete_first(prst->stack);

		if (ItemPointerIsValid(&s->iptr))
			break;
		pfree(s);
	}

	do {
		Buffer				buffer;
		Page				page;
		SpGistDeadTuple		dtuple;
		ItemPointer			tid;
		BlockNumber			blkno = ItemPointerGetBlockNumber(&s->iptr);
		OffsetNumber		offset = ItemPointerGetOffsetNumber(&s->iptr);

		buffer = ReadBuffer(prst->index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		if (PageIsNew(page) || SpGistPageIsDeleted(page) || offset > PageGetMaxOffsetNumber(page)) {
			UnlockReleaseBuffer(buffer);
			pfree(s);
			goto next;
		}

		{
			ItemId iid = PageGetItemId(page, offset);
			if (!ItemIdIsValid(iid) || !ItemIdHasStorage(iid)) {
				UnlockReleaseBuffer(buffer);
				pfree(s);
				goto next;
			}
			dtuple = (SpGistDeadTuple) PageGetItem(page, iid);
		}

		if (dtuple->tupstate == SPGIST_REDIRECT) {
			ItemPointerData redir_ipd = ((SpGistDeadTuple)dtuple)->pointer;
			UnlockReleaseBuffer(buffer);
			pfree(s);
			pushSPGistPrint(funcctx, prst, &redir_ipd, s->level);
			goto next;
		}

		if (dtuple->tupstate != SPGIST_LIVE) {
			UnlockReleaseBuffer(buffer);
			pfree(s);
			goto next;
		}

		if (SpGistPageIsLeaf(page)) {
			OffsetNumber max = PageGetMaxOffsetNumber(page);
			SpGistLeafTuple	leafTuple;

			if (SpGistBlockIsRoot(blkno) && offset < max) {
				ItemPointerData next_ipd;
				ItemPointerSet(&next_ipd, blkno, offset + 1);
				pushSPGistPrint(funcctx, prst, &next_ipd, s->level);
			}

			leafTuple = (SpGistLeafTuple)dtuple;

			if (!SpGistBlockIsRoot(blkno)) {
				OffsetNumber nextOffset = SGLT_GET_NEXTOFFSET(leafTuple);
				if (nextOffset != InvalidOffsetNumber && nextOffset <= max) {
					ItemPointerData next_ipd;
					ItemPointerSet(&next_ipd, blkno, nextOffset);
					pushSPGistPrint(funcctx, prst, &next_ipd, s->level);
				}
			}

			oldcontext = MemoryContextSwitchTo(prst->tmpctx);

			tid = palloc(sizeof(ItemPointerData));
			*tid = s->iptr;
			prst->dvalues[0] = PointerGetDatum(tid);
			prst->nulls[0] = ISNOTNULL;
			prst->nulls[1] = ISNULL;
			prst->nulls[2] = ISNULL;
			prst->dvalues[3]  = Int32GetDatum(s->level);
			prst->nulls[3] = ISNOTNULL;
			prst->nulls[4] = ISNULL;
			prst->nulls[5] = ISNULL;
			prst->nulls[6] = ISNULL;
			if (SpGistPageStoresNulls(page)) {
				prst->dvalues[7] = (Datum) 0;
				prst->nulls[7] = ISNULL;
			} else {
				prst->dvalues[7] = safe_spgist_datum(SGLTDATUM(leafTuple, &prst->state),
													 &prst->state.attLeafType);
				prst->nulls[7] = ISNOTNULL;
			}

			MemoryContextSwitchTo(oldcontext);
		} else {
			SpGistInnerTuple	innerTuple = (SpGistInnerTuple)dtuple;
			int					i;
			SpGistNodeTuple		node;

			SGITITERATE(innerTuple, i, node) {
				if (ItemPointerIsValid(&node->t_tid)) {
					if (i >= s->nlabel)
						break;
				}
			}

			if (i >= innerTuple->nNodes) {
				UnlockReleaseBuffer(buffer);
				pfree(s);
				goto next;
			}

			oldcontext = MemoryContextSwitchTo(prst->tmpctx);

			tid = palloc(sizeof(ItemPointerData));
			*tid = s->iptr;
			prst->dvalues[0] = PointerGetDatum(tid);
			prst->nulls[0] = ISNOTNULL;
			prst->dvalues[1] = BoolGetDatum(innerTuple->allTheSame);
			prst->nulls[1] = ISNOTNULL;
			prst->dvalues[2] = Int32GetDatum(i);
			prst->nulls[2] = ISNOTNULL;
			prst->dvalues[3]  = Int32GetDatum(s->level);
			prst->nulls[3] = ISNOTNULL;
			tid = palloc(sizeof(ItemPointerData));
			*tid = node->t_tid;
			prst->dvalues[4] = PointerGetDatum(tid);
			prst->nulls[4] = ISNOTNULL;
			if (innerTuple->prefixSize > 0) {
				prst->dvalues[5]  = safe_spgist_datum(SGITDATUM(innerTuple, &prst->state),
													  &prst->state.attPrefixType);
				prst->nulls[5] = ISNOTNULL;
			} else
				prst->nulls[5] = ISNULL;
			if (!innerTuple->allTheSame && OidIsValid(prst->state.config.labelType) && prst->state.config.labelType != VOIDOID && !IndexTupleHasNulls(node)) {
				prst->dvalues[6]  = safe_spgist_datum(SGNTDATUM(node, &prst->state),
													  &prst->state.attLabelType);
				prst->nulls[6] = ISNOTNULL;
			} else
				prst->nulls[6] = ISNULL;
			prst->nulls[7] = ISNULL;

			MemoryContextSwitchTo(oldcontext);

			pushSPGistPrint(funcctx, prst, &node->t_tid, s->level + 1);
			s->nlabel = i + 1;
			oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
			prst->stack = lcons(s, prst->stack);
			MemoryContextSwitchTo(oldcontext);
			s = NULL;
		}

		UnlockReleaseBuffer(buffer);
		if (s) pfree(s);
	} while(0);

	{
		MemoryContext tupcontext = MemoryContextSwitchTo(prst->tmpctx);
		htuple = heap_formtuple(funcctx->attinmeta->tupdesc, prst->dvalues, prst->nulls);
#if PG_VERSION_NUM >= 120000
		result = HeapTupleGetDatum(htuple);
#else
		result = TupleGetDatum(funcctx->slot, htuple);
#endif
		MemoryContextSwitchTo(tupcontext);
	}

	SRF_RETURN_NEXT(funcctx, result);
#endif
}

PG_FUNCTION_INFO_V1(gin_statpage);
Datum gin_statpage(PG_FUNCTION_ARGS);
Datum
gin_statpage(PG_FUNCTION_ARGS)
{
#if PG_VERSION_NUM < 90400
	elog(NOTICE, "Function is not working under PgSQL < 9.4");

	PG_RETURN_DATUM(CStringGetTextDatum("???"));
#else
	text	   *name = PG_GETARG_TEXT_P(0);
	RangeVar   *relvar;
	Relation	index;
	BlockNumber blkno;
	char		res[1024];
	uint32		totalPages,
#if PG_VERSION_NUM >= 100000
				deletedPages = 0,
				emptyDataPages = 0,
#endif
				entryPages = 0,
				dataPages = 0,
				dataInnerPages = 0,
				dataLeafPages = 0,
				entryInnerPages = 0,
				entryLeafPages = 0
				;
	uint64		dataInnerFreeSpace = 0,
				dataLeafFreeSpace = 0,
				dataInnerTuplesCount = 0,
				dataLeafIptrsCount = 0,
				entryInnerFreeSpace = 0,
				entryLeafFreeSpace = 0,
				entryInnerTuplesCount = 0,
				entryLeafTuplesCount = 0,
				entryPostingSize = 0,
				entryPostingCount = 0,
				entryAttrSize = 0
				;

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(name));
	index = relation_openrv(relvar, AccessShareLock);

#if PG_VERSION_NUM >= 110000
	if (index->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		elog(ERROR, "\"%s\" is a partitioned index; inspect individual partition indexes instead",
			 RelationGetRelationName(index));
#endif

	if (index->rd_rel->relkind != RELKIND_INDEX ||
			index->rd_rel->relam != GIN_AM_OID)
		elog(ERROR, "relation \"%s\" is not a GIN index",
			 RelationGetRelationName(index));

	totalPages = RelationGetNumberOfBlocks(index);

	if (totalPages <= GIN_ROOT_BLKNO)
	{
		index_close(index, AccessShareLock);
		snprintf(res, sizeof(res),
				 "totalPages:            0\n"
#if PG_VERSION_NUM >= 100000
				 "deletedPages:          0\n"
				 "emptyDataPages:        0\n"
#endif
				 "dataPages:             0\n"
				 "dataInnerPages:        0\n"
				 "dataLeafPages:         0\n"
				 "dataInnerFreeSpace:    0\n"
				 "dataLeafFreeSpace:     0\n"
				 "dataInnerTuplesCount:  0\n"
				 "dataLeafIptrsCount:    0\n"
				 "entryPages:            0\n"
				 "entryInnerPages:       0\n"
				 "entryLeafPages:        0\n"
				 "entryInnerFreeSpace:   0\n"
				 "entryLeafFreeSpace:    0\n"
				 "entryInnerTuplesCount: 0\n"
				 "entryLeafTuplesCount:  0\n"
				 "entryPostingSize:      0\n"
				 "entryPostingCount:     0\n"
				 "entryAttrSize:         0\n");
		PG_FREE_IF_COPY(name, 0);
		PG_RETURN_DATUM(CStringGetTextDatum(res));
	}

	for (blkno = GIN_ROOT_BLKNO; blkno < totalPages; blkno++)
	{
		Buffer		buffer;
		Page		page;
		PageHeader	header;

		CHECK_FOR_INTERRUPTS();

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		header = (PageHeader)page;

#if PG_VERSION_NUM >= 100000
		if (GinPageIsDeleted(page))
		{
			deletedPages++;
		}
		else
#endif
		if (GinPageIsData(page))
		{
			dataPages++;
			if (GinPageIsLeaf(page))
			{
				ItemPointerData minItem, *ptr;
				int nlist;


				dataLeafPages++;
				dataLeafFreeSpace += header->pd_upper - header->pd_lower;
				ItemPointerSetMin(&minItem);

				ptr = GinDataLeafPageGetItems(page, &nlist, minItem);

				if (ptr)
				{
					pfree(ptr);
					dataLeafIptrsCount += nlist;
				}
				else
					emptyDataPages++;
			}
			else
			{
				dataInnerPages++;
				dataInnerFreeSpace += header->pd_upper - header->pd_lower;
				dataInnerTuplesCount += GinPageGetOpaque(page)->maxoff;
			}
		}
		else
		{
			IndexTuple itup;
			OffsetNumber i, maxoff;

			maxoff = PageGetMaxOffsetNumber(page);

			entryPages++;
			if (GinPageIsLeaf(page))
			{
				entryLeafPages++;
				entryLeafFreeSpace += header->pd_upper - header->pd_lower;
				entryLeafTuplesCount += maxoff;
			}
			else
			{
				entryInnerPages++;
				entryInnerFreeSpace += header->pd_upper - header->pd_lower;
				entryInnerTuplesCount += maxoff;
			}

			for (i = 1; i <= maxoff; i++)
			{
				ItemId iid = PageGetItemId(page, i);

				if (!ItemIdIsValid(iid) || !ItemIdHasStorage(iid))
					continue;

				itup = (IndexTuple) PageGetItem(page, iid);

				if (GinPageIsLeaf(page))
				{
					if (GinIsPostingTree(itup))
					{
						entryAttrSize += IndexTupleSize(itup) - IndexInfoFindDataOffset((itup)->t_info);
					}
					else if (GinItupIsCompressed(itup))
					{
						GinPostingList *list = (GinPostingList *)GinGetPosting(itup);
						entryPostingCount += GinGetNPosting(itup);
						entryPostingSize += SizeOfGinPostingList(list);
						entryAttrSize += GinGetPostingOffset(itup) - IndexInfoFindDataOffset((itup)->t_info);
					}
					else
					{
						entryPostingCount += GinGetNPosting(itup);
						entryPostingSize += GinGetNPosting(itup) * sizeof(ItemPointerData);
						entryAttrSize += GinGetPostingOffset(itup) - IndexInfoFindDataOffset((itup)->t_info);
					}
				}
				else
				{
					entryAttrSize += IndexTupleSize(itup) - IndexInfoFindDataOffset((itup)->t_info);
				}
			}
		}

		UnlockReleaseBuffer(buffer);
	}

	index_close(index, AccessShareLock);
	if (totalPages > 0)
		totalPages--;

	snprintf(res, sizeof(res),
			 "totalPages:            %u\n"
#if PG_VERSION_NUM >= 100000
			 "deletedPages:          %u\n"
			 "emptyDataPages:        %u\n"
#endif
			 "dataPages:             %u\n"
			 "dataInnerPages:        %u\n"
			 "dataLeafPages:         %u\n"
			 "dataInnerFreeSpace:    " UINT64_FORMAT "\n"
			 "dataLeafFreeSpace:     " UINT64_FORMAT "\n"
			 "dataInnerTuplesCount:  " UINT64_FORMAT "\n"
			 "dataLeafIptrsCount:    " UINT64_FORMAT "\n"
			 "entryPages:            %u\n"
			 "entryInnerPages:       %u\n"
			 "entryLeafPages:        %u\n"
			 "entryInnerFreeSpace:   " UINT64_FORMAT "\n"
			 "entryLeafFreeSpace:    " UINT64_FORMAT "\n"
			 "entryInnerTuplesCount: " UINT64_FORMAT "\n"
			 "entryLeafTuplesCount:  " UINT64_FORMAT "\n"
			 "entryPostingSize:      " UINT64_FORMAT "\n"
			 "entryPostingCount:     " UINT64_FORMAT "\n"
			 "entryAttrSize:         " UINT64_FORMAT "\n",
			 totalPages,
#if PG_VERSION_NUM >= 100000
			 deletedPages,
			 emptyDataPages,
#endif
			 dataPages,
			 dataInnerPages,
			 dataLeafPages,
			 dataInnerFreeSpace,
			 dataLeafFreeSpace,
			 dataInnerTuplesCount,
			 dataLeafIptrsCount,
			 entryPages,
			 entryInnerPages,
			 entryLeafPages,
			 entryInnerFreeSpace,
			 entryLeafFreeSpace,
			 entryInnerTuplesCount,
			 entryLeafTuplesCount,
			 entryPostingSize,
			 entryPostingCount,
			 entryAttrSize
			 );

	PG_FREE_IF_COPY(name, 0);
	PG_RETURN_DATUM(CStringGetTextDatum(res));
#endif
}

#if PG_VERSION_NUM >= 120000
typedef enum {stat, print} TreeCond;
typedef struct
{
	IdxInfo idxInfo;
	IdxStat idxStat;
}BtreeIdxInfo;

/*
 * Depth-first search for btree
 * using for statistic data collection
 * and printing index values by level
 */
static void
btree_deep_search(Relation rel, int level,
		BlockNumber blk,  BtreeIdxInfo *btreeIdxInfo, TreeCond cond)
{
	Page			page;
	IndexTuple		itup;
	ItemId			iid;
	OffsetNumber	i,
					maxoff;
	BTPageOpaque	opaque;
	Buffer buffer;

	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	if (!BlockNumberIsValid(blk) || blk == P_NONE)
		return;

	buffer = _bt_getbuf(rel, blk, BT_READ);
	if (!BufferIsValid(buffer))
		return;

	page = (Page) BufferGetPage(buffer);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	if (PageIsNew(page) || P_IGNORE(opaque))
	{
		_bt_relbuf(rel, buffer);
		return;
	}

	maxoff = PageGetMaxOffsetNumber(page);

	switch (cond)
	{
		case stat:
		{
			btreeIdxInfo->idxStat.numpages++;
			btreeIdxInfo->idxStat.tuplesize+=(BLCKSZ - PageGetFreeSpace(page));
			btreeIdxInfo->idxStat.totalsize+=BLCKSZ;
			btreeIdxInfo->idxStat.numtuple+=maxoff;

			if (level > btreeIdxInfo->idxStat.level)
				btreeIdxInfo->idxStat.level = level;

			if (P_ISLEAF(opaque))
			{
				int dataTuples = maxoff - (P_RIGHTMOST(opaque) ? 0 : 1);
				if (dataTuples < 0) dataTuples = 0;

				btreeIdxInfo->idxStat.numleafpages++;
				btreeIdxInfo->idxStat.leaftuplesize+=(BLCKSZ - PageGetFreeSpace(page));

				for (i = P_FIRSTDATAKEY(opaque); i <= maxoff; i = OffsetNumberNext(i))
				{
					iid = PageGetItemId(page, i);
					if (!ItemIdIsValid(iid) || !ItemIdHasStorage(iid))
						continue;
#if PG_VERSION_NUM >= 130000
					itup = (IndexTuple) PageGetItem(page, iid);
					if (BTreeTupleIsPosting(itup))
						dataTuples += (BTreeTupleGetNPosting(itup) - 1);
#endif
				}
				btreeIdxInfo->idxStat.numleaftuple += dataTuples;
			}
			break;
		}
		case print:
		{
			char *pred;

			while ( (btreeIdxInfo->idxInfo.ptr-((char*)btreeIdxInfo->idxInfo.txt))
					+ level*4 + 256 >= btreeIdxInfo->idxInfo.len )
			{
					int dist=btreeIdxInfo->idxInfo.ptr-((char*)btreeIdxInfo->idxInfo.txt);
					if (btreeIdxInfo->idxInfo.len > MaxAllocSize / 2)
						elog(ERROR, "out of memory in btree_tree");
					btreeIdxInfo->idxInfo.len *= 2;
					btreeIdxInfo->idxInfo.txt=(text*)repalloc(btreeIdxInfo->idxInfo.txt,
							btreeIdxInfo->idxInfo.len);
					btreeIdxInfo->idxInfo.ptr = ((char*)btreeIdxInfo->idxInfo.txt)+dist;
			}

			pred = (char *) palloc(sizeof(char) * level * 4 + 1);
			memset(pred, ' ', level * 4);
			pred[level * 4] = '\0';

			sprintf(btreeIdxInfo->idxInfo.ptr, "%slvl: %d, blk: %u, numTuples: %d\n",
							pred,
							level,
							blk,
							(int)maxoff);

			pfree(pred);
			btreeIdxInfo->idxInfo.ptr=strchr(btreeIdxInfo->idxInfo.ptr,'\0');
			break;
		}
	}

	if (!P_ISLEAF(opaque) && ((level < btreeIdxInfo->idxInfo.maxlevel)
			||(btreeIdxInfo->idxInfo.maxlevel<0)))
	{
		int nchildren = 0;
		BlockNumber *children = (BlockNumber *) palloc(sizeof(BlockNumber) * (maxoff + 1));

		for (i = P_FIRSTDATAKEY(opaque); i <= maxoff; i = OffsetNumberNext(i))
		{
			iid = PageGetItemId(page, i);

			if (!ItemIdIsValid(iid) || !ItemIdHasStorage(iid))
			{
				btreeIdxInfo->idxStat.numinvalidtuple++;
				continue;
			}

			itup = (IndexTuple) PageGetItem(page, iid);
			children[nchildren++] = BTreeTupleGetDownLink(itup);
		}
		_bt_relbuf(rel, buffer);

		for (i = 0; i < nchildren; i++)
			btree_deep_search(rel, level + 1, children[i], btreeIdxInfo, cond);

		pfree(children);
	}
	else
	{
		_bt_relbuf(rel, buffer);
	}
}

/*
 * Print some statistic about btree index
 * This function shows information for live pages only
 * and do not shows information about deleting pages
 *
 * SELECT btree_stat(INDEXNAME);
 */
PG_FUNCTION_INFO_V1(btree_stat);
Datum btree_stat(PG_FUNCTION_ARGS);
Datum
btree_stat(PG_FUNCTION_ARGS)
{
	text		*name=PG_GETARG_TEXT_PP(0);
	RangeVar	*relvar;
	Relation	index;
	List		*relname_list;
	BtreeIdxInfo btreeIdxInfo;

	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *metad;
	BlockNumber rootBlk;

	text *out=(text*)palloc(1024);
	char *ptr=((char*)out)+VARHDRSZ;
	relname_list = textToQualifiedNameList(name);
	relvar = makeRangeVarFromNameList(relname_list);
	index = btree_index_open(relvar);

	memset(&btreeIdxInfo.idxStat, 0, sizeof(IdxStat));

	if (RelationGetNumberOfBlocks(index) == 0)
	{
		btree_index_close(index);
		snprintf(ptr, 1024 - VARHDRSZ,
			"Number of levels:          0\n"
			"Number of pages:           0\n"
			"Number of leaf pages:      0\n"
			"Number of tuples:          0\n"
			"Number of invalid tuples:  0\n"
			"Number of leaf tuples:     0\n"
			"Total size of tuples:      0 bytes\n"
			"Total size of leaf tuples: 0 bytes\n"
			"Total size of index:       0 bytes\n");
		ptr = strchr(ptr, '\0');
		PG_FREE_IF_COPY(name, 0);
		SET_VARSIZE(out, ptr - ((char*)out));
		PG_RETURN_POINTER(out);
	}

	/* Start dts from root */
	metabuf = _bt_getbuf(index, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);
	rootBlk = metad->btm_root;
	UnlockReleaseBuffer(metabuf);

	if (rootBlk != P_NONE)
		btree_deep_search(index, 0, rootBlk, &btreeIdxInfo, stat);

	btree_index_close(index);

	snprintf(ptr, 1024 - VARHDRSZ,
		"Number of levels:          %d\n"
		"Number of pages:           %d\n"
		"Number of leaf pages:      %d\n"
		"Number of tuples:          %d\n"
		"Number of invalid tuples:  %d\n"
		"Number of leaf tuples:     %d\n"
		"Total size of tuples:      "UINT64_FORMAT" bytes\n"
		"Total size of leaf tuples: "UINT64_FORMAT" bytes\n"
		"Total size of index:       "UINT64_FORMAT" bytes\n",
		(rootBlk == P_NONE) ? 0 : btreeIdxInfo.idxStat.level+1,
		btreeIdxInfo.idxStat.numpages,
		btreeIdxInfo.idxStat.numleafpages,
		btreeIdxInfo.idxStat.numtuple,
		btreeIdxInfo.idxStat.numinvalidtuple,
		btreeIdxInfo.idxStat.numleaftuple,
		btreeIdxInfo.idxStat.tuplesize,
		btreeIdxInfo.idxStat.leaftuplesize,
		btreeIdxInfo.idxStat.totalsize);

	ptr=strchr(ptr,'\0');

	PG_FREE_IF_COPY(name, 0);

	SET_VARSIZE(out, ptr-((char*)out));
	PG_RETURN_POINTER(out);
}

typedef struct BtPItem
{
	BlockNumber	   blk;
	OffsetNumber   offset;
	int			   level;
	struct BtPItem *next;
} BtPItem;

typedef struct
{
	List	 *relname_list;
	RangeVar *relvar;
	Relation index;
	Datum	 *dvalues;
	bool	 *nulls;
	BtPItem	 *item;
	MemoryContext tmpctx;
} BtTypeStorage;

static BtPItem*
pushBtPPage( FuncCallContext *funcctx, BlockNumber blk, int level )
{
	BtPItem		  *nitem;
	MemoryContext oldcontext;
	BtTypeStorage *st = (BtTypeStorage*)(funcctx->user_fctx);

	if (RelationGetNumberOfBlocks(st->index) <= blk)
		return st->item;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
	nitem = (BtPItem*)palloc( sizeof(BtPItem) );
	nitem->blk = blk;
	nitem->offset = InvalidOffsetNumber;
	nitem->level = level;
	nitem->next = st->item;
	st->item = nitem;

	MemoryContextSwitchTo(oldcontext);

	return nitem;
}

static BtPItem*
popBtPPage( FuncCallContext *funcctx )
{
	BtTypeStorage *st = (BtTypeStorage*)(funcctx->user_fctx);
	BtPItem  *oitem;

	if (st == NULL || st->item == NULL)
		return NULL;

	oitem = st->item;
	st->item = oitem->next;

	pfree( oitem );
	return st->item;
}

static void btree_close_call(FuncCallContext *funcctx);

static void
btree_print_cleanup_callback(void *arg)
{
	FuncCallContext *funcctx = (FuncCallContext *) arg;
	btree_close_call(funcctx);
}

static void
btree_close_call( FuncCallContext  *funcctx )
{
	BtTypeStorage *st = (BtTypeStorage*)(funcctx->user_fctx);
	if (!st)
		return;

	if (!IsAbortedTransactionBlockState())
	{
		while (st->item)
			popBtPPage(funcctx);

		if (st->index != NULL)
		{
			btree_index_close(st->index);
			st->index = NULL;
		}
	}
	else
	{
		st->item = NULL;
		st->index = NULL;
	}

	if (st->dvalues)
	{
		pfree(st->dvalues);
		st->dvalues = NULL;
	}
	if (st->nulls)
	{
		pfree(st->nulls);
		st->nulls = NULL;
	}
	funcctx->user_fctx = NULL;
}

/*
 * Settings for first call of btree_print
 * Sets the current memory context
 */
static void
btree_setup_firstcall(FuncCallContext  *funcctx, text *name)
{
	MemoryContext oldcontext;
	BtTypeStorage *st;
	TupleDesc	 tupdesc;
	char		  attname[NAMEDATALEN];
	int			  i;
	BlockNumber   blk;
	Buffer		  buffer;
	MemoryContextCallback *cb;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	st=(BtTypeStorage*)palloc( sizeof(BtTypeStorage) );
	memset(st,0,sizeof(BtTypeStorage));
	st->relname_list = textToQualifiedNameList(name);
	st->relvar = makeRangeVarFromNameList(st->relname_list);
	st->index = btree_index_open(st->relvar);
	st->item = NULL;
	st->tmpctx = AllocSetContextCreate(funcctx->multi_call_memory_ctx,
									   "btree_print_per_tuple",
									   ALLOCSET_DEFAULT_SIZES);
	funcctx->user_fctx = (void*)st;

	if (RelationGetNumberOfBlocks(st->index) == 0)
	{
		MemoryContextSwitchTo(oldcontext);
		st->item = NULL;
		return;
	}

	tupdesc = CreateTemplateTupleDesc(st->index->rd_att->natts+2);
	TupleDescInitEntry(tupdesc, 1, "level", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 2, "valid", BOOLOID, -1, 0);
	for (i = 0; i < st->index->rd_att->natts; i++)
	{
		sprintf(attname, "z%d", i+2);
		TupleDescInitEntry(
			tupdesc,
			i+3,
			attname,
			TS_GET_TYPEVAL(st, i, atttypid),
			TS_GET_TYPEVAL(st, i, atttypmod),
			TS_GET_TYPEVAL(st, i, attndims) );
	}
	BlessTupleDesc(tupdesc);


	st->dvalues = (Datum *) palloc((tupdesc->natts+2) * sizeof(Datum));
	st->nulls = palloc((tupdesc->natts+2) * sizeof(*st->nulls));

	funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

	cb = (MemoryContextCallback *) palloc(sizeof(MemoryContextCallback));
	cb->func = btree_print_cleanup_callback;
	cb->arg = (void *) funcctx;
	MemoryContextRegisterResetCallback(funcctx->multi_call_memory_ctx, cb);

	buffer = _bt_gettrueroot(st->index);
	if (BufferIsValid(buffer))
	{
		blk = BufferGetBlockNumber(buffer);
		UnlockReleaseBuffer(buffer);

		MemoryContextSwitchTo(oldcontext);

		pushBtPPage(funcctx, blk, 1);
	}
	else
	{
		MemoryContextSwitchTo(oldcontext);
		st->item = NULL;
	}
}

/*
 * Show index elements for btree from root to MAXLEVEL
 * SELECT btree_tree(INDEXNAME[, MAXLEVEL]);
 */
PG_FUNCTION_INFO_V1(btree_tree);
Datum btree_tree(PG_FUNCTION_ARGS);
Datum
btree_tree(PG_FUNCTION_ARGS)
{
	text			*name=PG_GETARG_TEXT_PP(0);
	RangeVar		*relvar;
	Relation		index;
	List			*relname_list;
	BtreeIdxInfo btreeIdxInfo;
	Buffer			metabuf;
	Page			metapg;
	BTMetaPageData  *metad;
	BlockNumber		rootBlk;

	/*
	 *  If we use MAXLEVEL is not used in SELECT btree_tree(INDEXNAME),
	 *  info.maxlevel set to -1
	 *  If MAXLEVEL is used in btree_tree call, set info.maxlevel = MAXLEVEL
	 */
	btreeIdxInfo.idxInfo.maxlevel = ( PG_NARGS() > 1 ) ? PG_GETARG_INT32(1) : -1;
	btreeIdxInfo.idxInfo.len=1024;
	btreeIdxInfo.idxInfo.txt=(text*)palloc( btreeIdxInfo.idxInfo.len );
	btreeIdxInfo.idxInfo.ptr=((char*)btreeIdxInfo.idxInfo.txt)+VARHDRSZ;
	*btreeIdxInfo.idxInfo.ptr = '\0';

	relname_list = textToQualifiedNameList(name);
	relvar = makeRangeVarFromNameList(relname_list);
	index = btree_index_open(relvar);

	if (RelationGetNumberOfBlocks(index) == 0)
	{
		btree_index_close(index);
		PG_FREE_IF_COPY(name, 0);
		SET_VARSIZE(btreeIdxInfo.idxInfo.txt, VARHDRSZ);
		PG_RETURN_POINTER(btreeIdxInfo.idxInfo.txt);
	}

	/* Start dts from root block */
	metabuf = _bt_getbuf(index, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);
	rootBlk = metad->btm_root;
	UnlockReleaseBuffer(metabuf);

	if (rootBlk != P_NONE)
		btree_deep_search(index, 0, rootBlk, &btreeIdxInfo, print);

	btree_index_close(index);

	btreeIdxInfo.idxInfo.ptr=strchr(btreeIdxInfo.idxInfo.ptr,'\0');

	PG_FREE_IF_COPY(name, 0);

	SET_VARSIZE(btreeIdxInfo.idxInfo.txt,
			btreeIdxInfo.idxInfo.ptr-((char*)btreeIdxInfo.idxInfo.txt));
	PG_RETURN_POINTER(btreeIdxInfo.idxInfo.txt);
}

/*
 * Print objects stored in btree tuples
 * works only if objects in index have textual representation
 * select * from btree_print(INDEXNAME)
 *		as t(level int, valid bool, a box) where level =1;
 */
PG_FUNCTION_INFO_V1(btree_print);
Datum btree_print(PG_FUNCTION_ARGS);
Datum
btree_print(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	BtTypeStorage   *st;
	ItemId		  iid;
	IndexTuple	  ituple;
	HeapTuple	   htuple;
	int				i;
	bool			isnull;
	BTPageOpaque	opaque;
	Datum			result=(Datum)0;
	MemoryContext	oldcontext;

	if (SRF_IS_FIRSTCALL())
	{
		text	*name=PG_GETARG_TEXT_PP(0);
		funcctx = SRF_FIRSTCALL_INIT();
		btree_setup_firstcall(funcctx, name);
		PG_FREE_IF_COPY(name,0);
	}

	funcctx = SRF_PERCALL_SETUP();
	st = (BtTypeStorage*)(funcctx->user_fctx);

	CHECK_FOR_INTERRUPTS();

	if ( !st || !st->item )
	{
		btree_close_call(funcctx);
		SRF_RETURN_DONE(funcctx);
	}

	/* Reset per-tuple memory context from previous iteration (Fix 2) */
	MemoryContextReset(st->tmpctx);

	while (st->item != NULL)
	{
		Buffer buffer = _bt_getbuf(st->index, st->item->blk, BT_READ);
		Page page = BufferGetPage(buffer);
		bool isLeaf;
		BlockNumber downlink = InvalidBlockNumber;
		int current_level;

		opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		if (st->item->offset == InvalidOffsetNumber)
			st->item->offset = P_FIRSTDATAKEY(opaque);

		if (PageIsNew(page) || P_IGNORE(opaque) || st->item->offset > PageGetMaxOffsetNumber(page))
		{
			_bt_relbuf(st->index, buffer);
			popBtPPage(funcctx);
			continue;
		}

		iid = PageGetItemId(page, st->item->offset);
		if (!ItemIdIsValid(iid) || !ItemIdHasStorage(iid))
		{
			st->item->offset = OffsetNumberNext(st->item->offset);
			_bt_relbuf(st->index, buffer);
			continue;
		}

		ituple = (IndexTuple) PageGetItem(page, iid);
		isLeaf = P_ISLEAF(opaque);
		if (!isLeaf)
			downlink = BTreeTupleGetDownLink(ituple);
		current_level = st->item->level;

		/* Switch to per-tuple memory context for datum detoasting/copying */
		oldcontext = MemoryContextSwitchTo(st->tmpctx);

		st->dvalues[0] = Int32GetDatum(current_level);
		st->nulls[0] = ISNOTNULL;
		st->dvalues[1] = BoolGetDatum(true);
		st->nulls[1] = ISNOTNULL;

		for (i = 2; i < funcctx->attinmeta->tupdesc->natts; i++)
		{
			int tup_natts = BTreeTupleGetNAtts(ituple, st->index);

			if (i - 1 > tup_natts)
			{
				st->dvalues[i] = (Datum) 0;
				st->nulls[i] = ISNULL;
			}
			else
			{
				Datum val = index_getattr(ituple, i - 1, st->index->rd_att, &isnull);
				if (isnull)
				{
					st->dvalues[i] = (Datum) 0;
					st->nulls[i] = ISNULL;
				}
				else
				{
					/* Fix 5: detoast & safe copy into tmpctx */
					Form_pg_attribute att = INDEX_GET_ATTR(st->index, i - 2);
					st->dvalues[i] = safe_index_datum(val, att);
					st->nulls[i] = ISNOTNULL;
				}
			}
		}

		/* Advance offset */
		st->item->offset = OffsetNumberNext(st->item->offset);

		/* Fix 3: Release buffer lock and pin BEFORE yielding control! */
		_bt_relbuf(st->index, buffer);

		/* If non-leaf, push downlink child page onto the stack */
		if (!isLeaf && BlockNumberIsValid(downlink))
			pushBtPPage(funcctx, downlink, current_level + 1);

		htuple = heap_formtuple(funcctx->attinmeta->tupdesc, st->dvalues, st->nulls);
#if PG_VERSION_NUM >= 120000
		result = HeapTupleGetDatum(htuple);
#else
		result = TupleGetDatum(funcctx->slot, htuple);
#endif

		MemoryContextSwitchTo(oldcontext);

		SRF_RETURN_NEXT(funcctx, result);
	}

	btree_close_call(funcctx);
	SRF_RETURN_DONE(funcctx);
}

/*
 * Print some statistic about brin index
 * SELECT brin_stat(INDEXNAME);
 */
PG_FUNCTION_INFO_V1(brin_stat);
Datum brin_stat(PG_FUNCTION_ARGS);
Datum
brin_stat(PG_FUNCTION_ARGS)
{
	text		*name=PG_GETARG_TEXT_PP(0);
	RangeVar	*relvar;
	Relation	index;
	List		*relname_list;

	BlockNumber startBlk;
	BlockNumber heapNumBlocks;
	BlockNumber pagesPerRange;
	BrinRevmap *revmap;
	Relation heapRel;
	BrinDesc *bdesc;
	Oid indexOid, heapOid;

	Buffer buf;

	uint32 numRevmapPages=0;
	uint32 numTuples = 0;
	uint32 numEmptyPages = 0;
	uint32 numRegularPages = 0;
	uint64 freeSpace = 0;
	uint64 usedSpace = 0;

	text *out=(text*)palloc(1024);
	char *ptr=((char*)out)+VARHDRSZ;

	relname_list = textToQualifiedNameList(name);
	relvar = makeRangeVarFromNameList(relname_list);
	indexOid = RangeVarGetRelid(relvar, NoLock, false);
	heapOid = IndexGetRelation(indexOid, true);
	if (!OidIsValid(heapOid))
		elog(ERROR, "relation \"%s\" is not an index", relvar->relname);

	/* Strict lock hierarchy: table first, then index */
	heapRel = table_open(heapOid, AccessShareLock);
	index = brin_index_open(indexOid);

	revmap = GEVEL_BRIN_REVMAP_INIT(index, &pagesPerRange);

	/* Determine range of pages to process */
	heapNumBlocks = RelationGetNumberOfBlocks(heapRel);

	startBlk = 0;

	buf = InvalidBuffer;
	bdesc = brin_build_desc(index);

	{
		BlockNumber last_buf_blkno = InvalidBlockNumber;

		/* Scan the revmap */
		for (; startBlk < heapNumBlocks; startBlk += pagesPerRange)
		{
			BrinTuple  *tup;
			OffsetNumber off;
			Page	page;
			Size	tupSize = 0;

			CHECK_FOR_INTERRUPTS();

			numRevmapPages++;
			/* Get on-disk tuple */
			tup = GEVEL_BRIN_GET_TUPLE(revmap, startBlk, &buf, &off, &tupSize,
					   BUFFER_LOCK_SHARE);

			if (tup)
			{
				numTuples++;
				page = BufferGetPage(buf);

				if (last_buf_blkno != BufferGetBlockNumber(buf))
				{
					if (BRIN_IS_REGULAR_PAGE(page))
						numRegularPages++;
					freeSpace += PageGetFreeSpace(page);
					last_buf_blkno = BufferGetBlockNumber(buf);
				}
				usedSpace += MAXALIGN((tupSize > 0) ? tupSize : sizeof(BrinMemTuple) +
						sizeof(BrinValues) * bdesc->bd_tupdesc->natts);
			}
			else
				/* If the revmap page points to void */
				numEmptyPages++;

			if (BufferIsValid(buf))
			{
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buf);
				buf = InvalidBuffer;
			}
		}
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	brin_free_desc(bdesc);
	brinRevmapTerminate(revmap);

	snprintf(ptr, 1024 - VARHDRSZ,
		"Number of revmap pages:        %u\n"
		"Number of empty revmap pages:  %u\n"
		"Number of regular pages:       %u\n"
		"Number of tuples:              %u\n"
		"Used space:                    "UINT64_FORMAT" bytes\n"
		"Free space:                    "UINT64_FORMAT" bytes\n",
		numRevmapPages,
		numEmptyPages,
		numRegularPages,
		numTuples,
		usedSpace,
		freeSpace
		);

	ptr=strchr(ptr,'\0');

	brin_index_close(index);
	table_close(heapRel, AccessShareLock);

	PG_FREE_IF_COPY(name, 0);

	SET_VARSIZE(out, ptr-((char*)out));
	PG_RETURN_POINTER(out);
}

/*
 * Print numbers of heap blocks from revmap
 * and numbers of end blocks from ranges
 */
PG_FUNCTION_INFO_V1(brin_print);
Datum brin_print(PG_FUNCTION_ARGS);
Datum
brin_print(PG_FUNCTION_ARGS)
{
	text	*name=PG_GETARG_TEXT_PP(0);
	RangeVar		*relvar;
	Relation		index;
	List			*relname_list;

	BlockNumber heapBlk;
	BlockNumber numBlocks;
	BlockNumber pagesPerRange;
	BrinRevmap *revmap;
	Relation heapRel;
	Oid indexOid, heapOid;
	int len = 1024;

	text *out=(text*)palloc(len);
	char *ptr=((char*)out)+VARHDRSZ;

	relname_list = textToQualifiedNameList(name);
	relvar = makeRangeVarFromNameList(relname_list);
	indexOid = RangeVarGetRelid(relvar, NoLock, false);
	heapOid = IndexGetRelation(indexOid, true);
	if (!OidIsValid(heapOid))
		elog(ERROR, "relation \"%s\" is not an index", relvar->relname);

	/* Strict lock hierarchy: table first, then index */
	heapRel = table_open(heapOid, AccessShareLock);
	index = brin_index_open(indexOid);

	/* Determine range of pages to process */
	numBlocks = RelationGetNumberOfBlocks(heapRel);
	revmap = GEVEL_BRIN_REVMAP_INIT(index, &pagesPerRange);

	for (heapBlk = 0; heapBlk < numBlocks; heapBlk += pagesPerRange)
	{
		BlockNumber rangeEndBlk;
		BlockNumber rangeBlk;

		CHECK_FOR_INTERRUPTS();

		if (heapBlk + pagesPerRange > numBlocks)
			rangeEndBlk = numBlocks - heapBlk;
		else
			rangeEndBlk = pagesPerRange;

		while ((ptr - ((char*)out)) + 256 >= len)
		{
			int dist = ptr - ((char*)out);
			if (len > MaxAllocSize / 2)
				elog(ERROR, "out of memory in brin_print");
			len *= 2;
			out = (text*)repalloc(out, len);
			ptr = ((char*)out) + dist;
		}

		sprintf(ptr, "Start block: %u; end block: %u\n", heapBlk, heapBlk + rangeEndBlk - 1);
		ptr = strchr(ptr, '\0');

		for (rangeBlk = 0; rangeBlk < rangeEndBlk; rangeBlk++)
		{
			Page page;
			Buffer buf;

			CHECK_FOR_INTERRUPTS();

			buf = ReadBuffer(heapRel, heapBlk + rangeBlk);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);

			while ((ptr - ((char*)out)) + 256 >= len)
			{
				int dist = ptr - ((char*)out);
				if (len > MaxAllocSize / 2)
					elog(ERROR, "out of memory in brin_print");
				len *= 2;
				out = (text*)repalloc(out, len);
				ptr = ((char*)out) + dist;
			}

			sprintf(ptr, "Heap block: %u; offset: %d, free: %d\n",
					heapBlk + rangeBlk,
					(int) PageGetMaxOffsetNumber(page),
					(int) PageGetFreeSpace(page));
			ptr = strchr(ptr, '\0');

			UnlockReleaseBuffer(buf);
		}
	}

	brinRevmapTerminate(revmap);
	brin_index_close(index);
	table_close(heapRel, AccessShareLock);

	PG_FREE_IF_COPY(name, 0);

	SET_VARSIZE(out, ptr - ((char*)out));
	PG_RETURN_POINTER(out);
}
#endif
