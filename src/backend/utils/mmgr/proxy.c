#include "postgres.h"

#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "lib/ilist.h"

typedef struct ProxyContext
{
	MemoryContextData header;	/* Standard memory-context fields */
	MemoryContext alloc_parent;
	dlist_head allocations;
} ProxyContext;

typedef struct ProxyChunk
{
	dlist_node node;
	void *context;
} ProxyChunk;

static void *ProxyContextAlloc(MemoryContext context, Size size);
static void ProxyContextFree(MemoryContext context, void *pointer);
static void *ProxyContextRealloc(MemoryContext context, void *pointer, Size size);
static void ProxyContextReset(MemoryContext context);
static void ProxyContextDelete(MemoryContext context);
static Size ProxyContextGetChunkSpace(MemoryContext context, void *pointer);
static bool ProxyContextIsEmpty(MemoryContext context);
static void ProxyContextStats(MemoryContext context,
						  MemoryStatsPrintFunc printfunc, void *passthru,
						  MemoryContextCounters *totals);

#ifdef MEMORY_CONTEXT_CHECKING
static void ProxyContextCheck(MemoryContext context);
#endif

/*
 * This is the virtual function table for ProxyContext contexts.
 */
static const MemoryContextMethods ProxyContextMethods = {
	ProxyContextAlloc,
	ProxyContextFree,
	ProxyContextRealloc,
	ProxyContextReset,
	ProxyContextDelete,
	ProxyContextGetChunkSpace,
	ProxyContextIsEmpty,
	ProxyContextStats
#ifdef MEMORY_CONTEXT_CHECKING
	,ProxyContextCheck
#endif
};

MemoryContext
ProxyContextCreate(MemoryContext parent,
				   const char *name)
{
	ProxyContext *proxy;
	MemoryContext alloc_parent;

	proxy = (ProxyContext *) malloc(sizeof(ProxyContext));
	if (proxy == NULL)
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while creating memory context \"%s\".",
						   name)));
	}

	/* find node we can actually allocate in */
	alloc_parent = parent;
	while (alloc_parent != NULL && IsA(alloc_parent, ProxyContext))
		alloc_parent = alloc_parent->parent;
	if (alloc_parent == NULL)
		elog(ERROR, "can't proxy forever");
	proxy->alloc_parent = parent;

	dlist_init(&proxy->allocations);

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) proxy,
						T_ProxyContext,
						&ProxyContextMethods,
						parent,
						name);

	return (MemoryContext) proxy;
}

static void *
ProxyContextAlloc(MemoryContext context, Size size)
{
	ProxyContext *proxy = castNode(ProxyContext, context);
	ProxyChunk *chunk;
	Size alloc_size;

	alloc_size = size + MAXALIGN(sizeof(ProxyChunk));

	chunk = MemoryContextAlloc(proxy->alloc_parent, alloc_size);

	dlist_push_tail(&proxy->allocations, &chunk->node);
	chunk->context = context;

	return (char *) chunk + MAXALIGN(sizeof(ProxyChunk));
}

static void
ProxyContextFree(MemoryContext context, void *pointer)
{
	ProxyChunk *chunk = (ProxyChunk *) ((char *) pointer - MAXALIGN(sizeof(ProxyChunk)));

	dlist_delete(&chunk->node);

	pfree(chunk);
}

static void *
ProxyContextRealloc(MemoryContext context, void *pointer, Size size)
{
	ProxyContext *proxy = castNode(ProxyContext, context);
	ProxyChunk *chunk = (ProxyChunk *) ((char *) pointer - MAXALIGN(sizeof(ProxyChunk)));

	dlist_delete(&chunk->node);

	chunk =repalloc ((char *) chunk, size + MAXALIGN(sizeof(ProxyChunk)));

	dlist_push_tail(&proxy->allocations, &chunk->node);
	chunk->context = context;

	return (char *) chunk + MAXALIGN(sizeof(ProxyChunk));
}

static void
ProxyContextReset(MemoryContext context)
{
	ProxyContext *proxy = castNode(ProxyContext, context);
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, &proxy->allocations)
	{
		ProxyChunk *ptr = dlist_container(ProxyChunk, node, iter.cur);

		dlist_delete(iter.cur);
		pfree(ptr);
	}
}

static void
ProxyContextDelete(MemoryContext context)
{
	ProxyContextReset(context);
	free(context);
}

static Size
ProxyContextGetChunkSpace(MemoryContext context, void *pointer)
{
	ProxyChunk *chunk = (ProxyChunk *) ((char *) pointer - MAXALIGN(sizeof(ProxyChunk)));

	return GetMemoryChunkSpace(chunk);
}

static bool
ProxyContextIsEmpty(MemoryContext context)
{
	ProxyContext *proxy = castNode(ProxyContext, context);

	return dlist_is_empty(&proxy->allocations);
}

static void
ProxyContextStats(MemoryContext context,
				  MemoryStatsPrintFunc printfunc, void *passthru,
				  MemoryContextCounters *totals)
{
	ProxyContext *proxy = castNode(ProxyContext, context);
	Size		nblocks = 0;
	Size		freechunks = 0;
	Size		totalspace = 0;
	Size		freespace = 0;
	Size		chunks = 0;
	dlist_iter iter;

	dlist_foreach(iter, &proxy->allocations)
	{
		ProxyChunk *chunk = dlist_container(ProxyChunk, node, iter.cur);

		totalspace += GetMemoryChunkSpace((void *) chunk);
		chunks++;
	}

	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu chunks proxied to parent, totaling %zu bytes",
				 chunks, totalspace);
		printfunc(context, passthru, stats_string);
	}

	if (totals)
	{
		totals->nblocks += nblocks;
		totals->freechunks += freechunks;
		totals->totalspace += totalspace;
		totals->freespace += freespace;
	}
}

#ifdef MEMORY_CONTEXT_CHECKING
static void
ProxyContextCheck(MemoryContext context)
{
	/* FIXME */
}
#endif
