#include "postgres.h"

#include "lib/ilist.h"
#include "port/pg_bitutils.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_memorychunk.h"
#include "utils/memutils_internal.h"

typedef struct ProxyContext
{
	MemoryContextData header;	/* Standard memory-context fields */

	dlist_head	allocations;
} ProxyContext;

typedef struct ProxyChunk
{
	dlist_node	node;
	size_t		sz;
	ProxyContext *context;
} ProxyChunk;


#define ExternalChunkGetBlock(chunk) \
	(ProxyChunk *) ((char *) chunk - MAXALIGN(sizeof(ProxyChunk)))


MemoryContext
ProxyContextCreate(MemoryContext parent, const char *name)
{
	Size		allocSize = sizeof(ProxyContext);
	ProxyContext *set;

	/*
	 * Allocate the initial block.  Unlike other proxy.c blocks, it starts
	 * with the context header and its block header follows that.
	 */
	set = (ProxyContext *) malloc(allocSize);
	if (set == NULL)
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while creating memory context \"%s\".",
						   name)));
	}

	VALGRIND_CREATE_MEMPOOL(set, 0, false);
	VALGRIND_MEMPOOL_ALLOC(set, set, allocSize);


	dlist_init(&set->allocations);

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) set, T_ProxyContext, MCTX_PROXY_ID,
						parent, name);

	((MemoryContext) set)->mem_allocated = allocSize;

	return (MemoryContext) set;
}

void *
ProxyAlloc(MemoryContext context, Size size, int flags)
{
	ProxyContext *set = (ProxyContext *) context;
	size_t		sz;
	ProxyChunk *proxy_chunk;
	MemoryChunk *chunk;

	/* validate 'size' is within the limits for the given 'flags' */
	MemoryContextCheckSize(context, size, flags);

	sz = MAXALIGN(sizeof(ProxyChunk)) + sizeof(MemoryChunk) + MAXALIGN(size);

	proxy_chunk = (ProxyChunk *) malloc(sz);
	if (proxy_chunk == NULL)
		return MemoryContextAllocationFailure(context, size, flags);

	VALGRIND_MEMPOOL_ALLOC(set, proxy_chunk, size);

	proxy_chunk->sz = sz;
	context->mem_allocated += sz;

	proxy_chunk->context = set;

	dlist_push_tail(&set->allocations, &proxy_chunk->node);

	chunk = (MemoryChunk *) (((char *) proxy_chunk) + MAXALIGN(sizeof(ProxyChunk)));

	MemoryChunkSetHdrMaskExternal(chunk, MCTX_PROXY_ID);

	return MemoryChunkGetPointer(chunk);
}

void
ProxyFree(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	ProxyContext *set;
	ProxyChunk *proxy_chunk;

	VALGRIND_MAKE_MEM_DEFINED(chunk, sizeof(MemoryChunk));

	Assert(MemoryChunkIsExternal(chunk));

	proxy_chunk = ExternalChunkGetBlock(chunk);
	VALGRIND_MAKE_MEM_DEFINED(proxy_chunk, sizeof(ProxyChunk));

	set = proxy_chunk->context;

	dlist_delete_from(&set->allocations, &proxy_chunk->node);

	proxy_chunk->context->header.mem_allocated -= proxy_chunk->sz;

	VALGRIND_MEMPOOL_FREE(proxy_chunk->context, proxy_chunk);

	free(proxy_chunk);
}

void *
ProxyRealloc(void *pointer, Size size, int flags)
{
	MemoryChunk *old_chunk = PointerGetMemoryChunk(pointer);
	ProxyChunk *proxy_chunk,
			   *old_proxy_chunk;
	MemoryChunk *chunk;
	ProxyContext *set;
	size_t		sz;

	VALGRIND_MAKE_MEM_DEFINED(old_chunk, sizeof(MemoryChunk));

	Assert(MemoryChunkIsExternal(old_chunk));

	sz = MAXALIGN(sizeof(ProxyChunk)) + sizeof(MemoryChunk) + MAXALIGN(size);

	old_proxy_chunk = ExternalChunkGetBlock(old_chunk);

	VALGRIND_MAKE_MEM_DEFINED(old_proxy_chunk, sizeof(ProxyChunk));

	set = old_proxy_chunk->context;

	proxy_chunk = malloc(sz);
	if (proxy_chunk == NULL)
		return MemoryContextAllocationFailure((MemoryContext) set, size, flags);

	dlist_delete_from(&set->allocations, &old_proxy_chunk->node);
	old_proxy_chunk->context->header.mem_allocated -= old_proxy_chunk->sz;

	chunk = (MemoryChunk *) (((char *) proxy_chunk) + MAXALIGN(sizeof(ProxyChunk)));


	proxy_chunk->context = set;
	proxy_chunk->sz = sz;
	dlist_push_tail(&set->allocations, &proxy_chunk->node);
	proxy_chunk->context->header.mem_allocated += sz;

	MemoryChunkSetHdrMaskExternal(chunk, MCTX_PROXY_ID);

	memcpy(chunk, old_chunk, Min(proxy_chunk->sz, old_proxy_chunk->sz));

	VALGRIND_MEMPOOL_FREE(proxy_chunk->context, proxy_chunk);
	free(old_proxy_chunk);

	return MemoryChunkGetPointer(chunk);
}

void
ProxyReset(MemoryContext context)
{
	ProxyContext *set = (ProxyContext *) context;

	while (!dlist_is_empty(&set->allocations))
	{
		ProxyChunk *proxy_chunk =
			dlist_container(ProxyChunk, node,
							dlist_pop_head_node(&set->allocations));

		VALGRIND_MEMPOOL_FREE(set, proxy_chunk);

		free(proxy_chunk);
	}
}

void
ProxyDelete(MemoryContext context)
{
	ProxyContext *set = (ProxyContext *) context;

	ProxyReset(context);

	VALGRIND_DESTROY_MEMPOOL(context);

	free(set);
}

MemoryContext
ProxyGetChunkContext(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	ProxyChunk *proxy_chunk;

	VALGRIND_MAKE_MEM_DEFINED(chunk, sizeof(MemoryChunk));

	Assert(MemoryChunkIsExternal(chunk));
	proxy_chunk = ExternalChunkGetBlock(chunk);

	VALGRIND_MAKE_MEM_DEFINED(proxy_chunk, sizeof(ProxyChunk));

	return (MemoryContext) proxy_chunk->context;
}

Size
ProxyGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	ProxyChunk *proxy_chunk;

	VALGRIND_MAKE_MEM_DEFINED(chunk, sizeof(MemoryChunk));

	Assert(MemoryChunkIsExternal(chunk));
	proxy_chunk = ExternalChunkGetBlock(chunk);

	VALGRIND_MAKE_MEM_DEFINED(proxy_chunk, sizeof(ProxyChunk));

	/* FIXME: check what size to return */

	return proxy_chunk->sz;
}

bool
ProxyIsEmpty(MemoryContext context)
{
	ProxyContext *set = (ProxyContext *) context;

	return dlist_is_empty(&set->allocations);
}

void
ProxyStats(MemoryContext context, MemoryStatsPrintFunc printfunc,
		   void *passthru, MemoryContextCounters *totals,
		   bool print_to_stderr)
{
	ProxyContext *set = (ProxyContext *) context;
	dlist_iter	iter;
	Size		totalspace;
	Size		nchunks = 0;

	totalspace = MAXALIGN(sizeof(ProxyContext));

	dlist_foreach(iter, &set->allocations)
	{
		ProxyChunk *proxy_chunk = dlist_container(ProxyChunk, node, iter.cur);

		nchunks++;
		totalspace += proxy_chunk->sz;
	}


	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu chunks;",
				 totalspace, nchunks);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

	if (totals)
	{
		totals->nblocks += nchunks;
		totals->totalspace += totalspace;
	}
}

#ifdef MEMORY_CONTEXT_CHECKING
void
ProxyCheck(MemoryContext context)
{
}
#endif
