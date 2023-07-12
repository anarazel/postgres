/*
 *	fe_memutils.h
 *		memory management support for frontend code
 *
 *	Copyright (c) 2003-2024, PostgreSQL Global Development Group
 *
 *	src/include/common/fe_memutils.h
 */
#ifndef FE_MEMUTILS_H
#define FE_MEMUTILS_H

/*
 * Flags for pg_malloc_extended and palloc_extended, deliberately named
 * the same as the backend flags.
 */
#define MCXT_ALLOC_HUGE			0x01	/* allow huge allocation (> 1 GB) not
										 * actually used for frontends */
#define MCXT_ALLOC_NO_OOM		0x02	/* no failure if out-of-memory */
#define MCXT_ALLOC_ZERO			0x04	/* zero allocated memory */

#define pg_fe_alloc_freeing_funcs \
	pg_malloc_attr(pg_free), \
	pg_malloc_attr(free), \
	pg_malloc_attr(pfree), \
	pg_malloc_attr(pg_realloc)


#define pg_fe_alloc_attributes(size_at) \
	__attribute__((malloc, pg_fe_alloc_freeing_funcs, alloc_size(size_at), assume_aligned(MAXIMUM_ALIGNOF), returns_nonnull, warn_unused_result))
#define pg_fe_alloc_noerr_attributes(size_at) \
	__attribute__((malloc, pg_fe_alloc_freeing_funcs, alloc_size(size_at), assume_aligned(MAXIMUM_ALIGNOF), warn_unused_result))
#define pg_fe_realloc_attributes(old_at, size_at) \
	__attribute__((pg_fe_alloc_freeing_funcs, alloc_size(size_at), assume_aligned(MAXIMUM_ALIGNOF), \
				   returns_nonnull, warn_unused_result))
#define pg_fe_realloc_noerr_attributes(old_at, size_at) \
	__attribute__((pg_fe_alloc_freeing_funcs, alloc_size(size_at), assume_aligned(MAXIMUM_ALIGNOF), \
				   warn_unused_result))
#define pg_fe_dup_attributes(source_at) \
	__attribute__((malloc, pg_fe_alloc_freeing_funcs, assume_aligned(MAXIMUM_ALIGNOF), returns_nonnull, nonnull(source_at), warn_unused_result))


/*
 * "Safe" memory allocation functions --- these exit(1) on failure
 * (except pg_malloc_extended with MCXT_ALLOC_NO_OOM)
 */
extern void pg_free(void *ptr);
extern void pfree(void *pointer);
extern void *repalloc(void *pointer, Size size);
extern void *pg_realloc(void *ptr, size_t size);

extern char *pg_strdup(const char *in)  pg_fe_dup_attributes(1);
extern void *pg_malloc(size_t size) pg_fe_alloc_attributes(1);
extern void *pg_malloc0(size_t size)  pg_fe_alloc_attributes(1);
extern void *pg_malloc_extended(size_t size, int flags) pg_fe_alloc_noerr_attributes(1);
extern void *pg_realloc(void *ptr, size_t size) pg_fe_realloc_attributes(1, 2);
extern void *repalloc(void *ptr, size_t size) pg_fe_realloc_attributes(1, 2);

/*
 * Variants with easier notation and more type safety
 */

/*
 * Allocate space for one object of type "type"
 */
#define pg_malloc_object(type) ((type *) pg_malloc(sizeof(type)))
#define pg_malloc0_object(type) ((type *) pg_malloc0(sizeof(type)))

/*
 * Allocate space for "count" objects of type "type"
 */
#define pg_malloc_array(type, count) ((type *) pg_malloc(sizeof(type) * (count)))
#define pg_malloc0_array(type, count) ((type *) pg_malloc0(sizeof(type) * (count)))

/*
 * Change size of allocation pointed to by "pointer" to have space for "count"
 * objects of type "type"
 */
#define pg_realloc_array(pointer, type, count) ((type *) pg_realloc(pointer, sizeof(type) * (count)))

/* Equivalent functions, deliberately named the same as backend functions */
extern void *repalloc(void *pointer, Size size) pg_fe_realloc_attributes(1, 2);
extern char *pstrdup(const char *in) pg_fe_dup_attributes(1);
extern char *pnstrdup(const char *in, Size size) pg_fe_dup_attributes(1);
extern void *palloc(Size size) pg_fe_alloc_attributes(1);
extern void *palloc0(Size size) pg_fe_alloc_attributes(1);
extern void *palloc_extended(Size size, int flags) pg_fe_alloc_noerr_attributes(1);

#define palloc_object(type) ((type *) palloc(sizeof(type)))
#define palloc0_object(type) ((type *) palloc0(sizeof(type)))
#define palloc_array(type, count) ((type *) palloc(sizeof(type) * (count)))
#define palloc0_array(type, count) ((type *) palloc0(sizeof(type) * (count)))
#define repalloc_array(pointer, type, count) ((type *) repalloc(pointer, sizeof(type) * (count)))

/* sprintf into a palloc'd buffer --- these are in psprintf.c */
extern char *psprintf(const char *fmt,...) pg_attribute_printf(1, 2) pg_fe_dup_attributes(1);
extern size_t pvsnprintf(char *buf, size_t len, const char *fmt, va_list args) pg_attribute_printf(3, 0);

#endif							/* FE_MEMUTILS_H */
