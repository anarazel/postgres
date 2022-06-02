#ifndef MEMUTILS_INTERNAL_H
#define MEMUTILS_INTERNAL_H

#include "utils/memutils.h"

extern void *AllocSetAlloc(MemoryContext context, Size size);
extern void AllocSetFree(void *pointer);
extern void *AllocSetRealloc(void *pointer, Size size);
extern void AllocSetReset(MemoryContext context);
extern void AllocSetDelete(MemoryContext context);
extern MemoryContext AllocSetGetChunkContext(void *pointer);
extern Size AllocSetGetChunkSpace(void *pointer);
extern bool AllocSetIsEmpty(MemoryContext context);
extern void AllocSetStats(MemoryContext context,
						  MemoryStatsPrintFunc printfunc, void *passthru,
						  MemoryContextCounters *totals,
						  bool print_to_stderr);

#ifdef MEMORY_CONTEXT_CHECKING
extern void AllocSetCheck(MemoryContext context);
#endif


extern void *GenerationAlloc(MemoryContext context, Size size);
extern void GenerationFree(void *pointer);
extern void *GenerationRealloc(void *pointer, Size size);
extern void GenerationReset(MemoryContext context);
extern void GenerationDelete(MemoryContext context);
extern MemoryContext GenerationGetChunkContext(void *pointer);
extern Size GenerationGetChunkSpace(void *pointer);
extern bool GenerationIsEmpty(MemoryContext context);
extern void GenerationStats(MemoryContext context,
							MemoryStatsPrintFunc printfunc, void *passthru,
							MemoryContextCounters *totals,
							bool print_to_stderr);

#ifdef MEMORY_CONTEXT_CHECKING
extern void GenerationCheck(MemoryContext context);
#endif


/*
 * These functions implement the MemoryContext API for Slab contexts.
 */
extern void *SlabAlloc(MemoryContext context, Size size);
extern void SlabFree(void *pointer);
extern void *SlabRealloc(void *pointer, Size size);
extern void SlabReset(MemoryContext context);
extern void SlabDelete(MemoryContext context);
extern MemoryContext SlabGetChunkContext(void *pointer);
extern Size SlabGetChunkSpace(void *pointer);
extern bool SlabIsEmpty(MemoryContext context);
extern void SlabStats(MemoryContext context,
					  MemoryStatsPrintFunc printfunc, void *passthru,
					  MemoryContextCounters *totals,
					  bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING
extern void SlabCheck(MemoryContext context);
#endif


typedef enum MemoryContextMethodID
{
	MCTX_ASET_ID = 0,
	MCTX_GENERATION_ID,
	MCTX_SLAB_ID,
} MemoryContextMethodID;


/*
 * This routine handles the context-type-independent part of memory
 * context creation.  It's intended to be called from context-type-
 * specific creation routines, and noplace else.
 */
extern void MemoryContextCreate(MemoryContext node,
								NodeTag tag,
								MemoryContextMethodID method_id,
								MemoryContext parent,
								const char *name);


typedef struct GenericChunkHeader
{
	/* Upper 3-bits of size used for storing the memory context method */
	Size		size;
} GenericChunkHeader;

#define GenericChunkSetSizeandMethod(c, sz, m) (c).size = (sz) | ((Size) m) << (SIZEOF_SIZE_T * 8 - 3)
#define GenericChunkGetSize(c) (((c).size << 3) >> 3)

static inline MemoryContextMethodID
GetMemoryChunkMethodID(void *pointer)
{
	GenericChunkHeader *header;

	/*
	 * Try to detect bogus pointers handed to us, poorly though we can.
	 * Presumably, a pointer that isn't MAXALIGNED isn't pointing at an
	 * allocated chunk.
	 */
	Assert(pointer != NULL);
	Assert(pointer == (void *) MAXALIGN(pointer));

	header = (GenericChunkHeader *) (((char *) pointer) - sizeof(GenericChunkHeader));

	return header->size >> (SIZEOF_SIZE_T * 8 - 3);
}

#endif
