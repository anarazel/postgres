/*-------------------------------------------------------------------------
 *
 * indextuple.c
 *	   This file contains index tuple accessor and mutator routines,
 *	   as well as various tuple utilities.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/indextuple.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/itup.h"
#include "access/toast_internals.h"

/*
 * This enables de-toasting of index entries.  Needed until VACUUM is
 * smart enough to rebuild indexes from scratch.
 */
#define TOAST_INDEX_HACK

/* ----------------------------------------------------------------
 *				  index_ tuple interface routines
 * ----------------------------------------------------------------
 */

 /* ----------------
  *		index_form_tuple
  *
  *		As index_form_tuple_context, but allocates the returned tuple in the
  *		CurrentMemoryContext.
  * ----------------
  */
IndexTuple
index_form_tuple(TupleDesc tupleDescriptor,
				 const Datum *values,
				 const bool *isnull)
{
	return index_form_tuple_context(tupleDescriptor, values, isnull,
									CurrentMemoryContext);
}

/* ----------------
 *		index_form_tuple_context
 *
 *		This shouldn't leak any memory; otherwise, callers such as
 *		tuplesort_putindextuplevalues() will be very unhappy.
 *
 *		This shouldn't perform external table access provided caller
 *		does not pass values that are stored EXTERNAL.
 *
 *		Allocates returned tuple in provided 'context'.
 * ----------------
 */
IndexTuple
index_form_tuple_context(TupleDesc tupleDescriptor,
						 const Datum *values,
						 const bool *isnull,
						 MemoryContext context)
{
	char	   *tp;				/* tuple pointer */
	IndexTuple	tuple;			/* return tuple */
	Size		size,
				data_size,
				hoff;
	int			i;
	unsigned short infomask = 0;
	bool		hasnull = false;
	uint16		tupmask = 0;
	int			numberOfAttributes = tupleDescriptor->natts;

#ifdef TOAST_INDEX_HACK
	Datum		untoasted_values[INDEX_MAX_KEYS] = {0};
	bool		untoasted_free[INDEX_MAX_KEYS] = {0};
#endif

	if (numberOfAttributes > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of index columns (%d) exceeds limit (%d)",
						numberOfAttributes, INDEX_MAX_KEYS)));

#ifdef TOAST_INDEX_HACK
	for (i = 0; i < numberOfAttributes; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDescriptor, i);

		untoasted_values[i] = values[i];
		untoasted_free[i] = false;

		/* Do nothing if value is NULL or not of varlena type */
		if (isnull[i] || att->attlen != -1)
			continue;

		/*
		 * If value is stored EXTERNAL, must fetch it so we are not depending
		 * on outside storage.  This should be improved someday.
		 */
		if (VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))
		{
			untoasted_values[i] =
				PointerGetDatum(detoast_external_attr((struct varlena *)
													  DatumGetPointer(values[i])));
			untoasted_free[i] = true;
		}

		/*
		 * If value is above size target, and is of a compressible datatype,
		 * try to compress it in-line.
		 */
		if (!VARATT_IS_EXTENDED(DatumGetPointer(untoasted_values[i])) &&
			VARSIZE(DatumGetPointer(untoasted_values[i])) > TOAST_INDEX_TARGET &&
			(att->attstorage == TYPSTORAGE_EXTENDED ||
			 att->attstorage == TYPSTORAGE_MAIN))
		{
			Datum		cvalue;

			cvalue = toast_compress_datum(untoasted_values[i],
										  att->attcompression);

			if (DatumGetPointer(cvalue) != NULL)
			{
				/* successful compression */
				if (untoasted_free[i])
					pfree(DatumGetPointer(untoasted_values[i]));
				untoasted_values[i] = cvalue;
				untoasted_free[i] = true;
			}
		}
	}
#endif

	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			break;
		}
	}

	if (hasnull)
		infomask |= INDEX_NULL_MASK;

	hoff = IndexInfoFindDataOffset(infomask);
#ifdef TOAST_INDEX_HACK
	data_size = heap_compute_data_size(tupleDescriptor,
									   untoasted_values, isnull);
#else
	data_size = heap_compute_data_size(tupleDescriptor,
									   values, isnull);
#endif
	size = hoff + data_size;
	size = MAXALIGN(size);		/* be conservative */

	tp = (char *) MemoryContextAllocZero(context, size);
	tuple = (IndexTuple) tp;

	heap_fill_tuple(tupleDescriptor,
#ifdef TOAST_INDEX_HACK
					untoasted_values,
#else
					values,
#endif
					isnull,
					tp + hoff,
					data_size,
					&tupmask,
					(hasnull ? (bits8 *) tp + sizeof(IndexTupleData) : NULL));

#ifdef TOAST_INDEX_HACK
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (untoasted_free[i])
			pfree(DatumGetPointer(untoasted_values[i]));
	}
#endif

	/*
	 * We do this because heap_fill_tuple wants to initialize a "tupmask"
	 * which is used for HeapTuples, but we want an indextuple infomask. The
	 * only relevant info is the "has variable attributes" field. We have
	 * already set the hasnull bit above.
	 */
	if (tupmask & HEAP_HASVARWIDTH)
		infomask |= INDEX_VAR_MASK;

	/* Also assert we got rid of external attributes */
#ifdef TOAST_INDEX_HACK
	Assert((tupmask & HEAP_HASEXTERNAL) == 0);
#endif

	/*
	 * Here we make sure that the size will fit in the field reserved for it
	 * in t_info.
	 */
	if ((size & INDEX_SIZE_MASK) != size)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row requires %zu bytes, maximum size is %zu",
						size, (Size) INDEX_SIZE_MASK)));

	infomask |= size;

	/*
	 * initialize metadata
	 */
	tuple->t_info = infomask;
	return tuple;
}

/* ----------------
 *		nocache_index_getattr
 *
 *		This gets called from index_getattr() macro, and only in cases
 *		where we can't use cacheoffset and the value is not null.
 * ----------------
 */
Datum
nocache_index_getattr(IndexTuple tup,
					  int attnum,
					  TupleDesc tupleDesc)
{
	CompactAttribute *cattr;
	char	   *tp;				/* ptr to data part of tuple */
	bits8	   *bp = NULL;		/* ptr to null bitmap in tuple */
	int			data_off;		/* tuple data offset */
	int			off;			/* current offset within data */
	int			startAttr;
	int			firstnullattr;
	bool		hasnulls = IndexTupleHasNulls(tup);
	int			i;

	/* Did someone forget to call TupleDescFinalize()? */
	Assert(tupleDesc->firstNonCachedOffAttr >= 0);

	attnum--;

	/*
	 * If there are no NULLs before the required attnum, then we can start at
	 * the highest attribute with a known offset, or the first attribute if
	 * none have a cached offset.  If the tuple has no variable width types,
	 * which is common with indexes, then we can use a slightly cheaper method
	 * of offset calculation, as we just need to add the attlen to the aligned
	 * offset when skipping over columns.  When the tuple contains
	 * variable-width types, we must use att_addlength_pointer(), which does a
	 * bit more branching and is slightly less efficient.
	 */
	data_off = IndexInfoFindDataOffset(tup->t_info);
	tp = (char *) tup + data_off;

	/*
	 * Find the first NULL column, or if there's none set the first NULL to
	 * attnum so that we can forego NULL checking all the way to attnum.
	 */
	if (hasnulls)
	{
		bp = (bits8 *) ((char *) tup + sizeof(IndexTupleData));
		firstnullattr = first_null_attr(bp, attnum);
	}
	else
		firstnullattr = attnum;

	if (tupleDesc->firstNonCachedOffAttr > 0)
	{
		startAttr = Min(tupleDesc->firstNonCachedOffAttr - 1, firstnullattr);
		off = TupleDescCompactAttr(tupleDesc, startAttr)->attcacheoff;
	}
	else
	{
		startAttr = 0;
		off = 0;
	}

	/* Handle tuples with var-width attributes */
	if (IndexTupleHasVarwidths(tup))
	{
		/* Calculate the offset up until the first NULL */
		for (i = startAttr; i < firstnullattr; i++)
		{
			cattr = TupleDescCompactAttr(tupleDesc, i);

			off = att_pointer_alignby(off,
									  cattr->attalignby,
									  cattr->attlen,
									  tp + off);
			off = att_addlength_pointer(off, cattr->attlen, tp + off);
		}

		/* Calculate the offset for any remaining columns. */
		for (; i < attnum; i++)
		{
			Assert(hasnulls);

			if (att_isnull(i, bp))
				continue;

			cattr = TupleDescCompactAttr(tupleDesc, i);

			off = att_pointer_alignby(off,
									  cattr->attalignby,
									  cattr->attlen,
									  tp + off);
			off = att_addlength_pointer(off, cattr->attlen, tp + off);
		}
	}
	else
	{
		/* Handle tuples with only fixed-width attributes */

		/* Calculate the offset up until the first NULL */
		for (i = startAttr; i < firstnullattr; i++)
		{
			cattr = TupleDescCompactAttr(tupleDesc, i);

			Assert(cattr->attlen > 0);
			off = att_pointer_alignby(off,
									  cattr->attalignby,
									  cattr->attlen,
									  tp + off);
			off += cattr->attlen;
		}

		/* Calculate the offset for any remaining columns. */
		for (; i < attnum; i++)
		{
			Assert(hasnulls);

			if (att_isnull(i, bp))
				continue;

			cattr = TupleDescCompactAttr(tupleDesc, i);

			Assert(cattr->attlen > 0);
			off = att_pointer_alignby(off,
									  cattr->attalignby,
									  cattr->attlen,
									  tp + off);
			off += cattr->attlen;
		}
	}

	cattr = TupleDescCompactAttr(tupleDesc, attnum);
	off = att_pointer_alignby(off, cattr->attalignby,
							  cattr->attlen, tp + off);
	return fetchatt(cattr, tp + off);
}

/*
 * Convert an index tuple into Datum/isnull arrays.
 *
 * The caller must allocate sufficient storage for the output arrays.
 * (INDEX_MAX_KEYS entries should be enough.)
 *
 * This is nearly the same as heap_deform_tuple(), but for IndexTuples.
 * One difference is that the tuple should never have any missing columns.
 */
void
index_deform_tuple(IndexTuple tup, TupleDesc tupleDescriptor,
				   Datum *values, bool *isnull)
{
	char	   *tp;				/* ptr to tuple data */
	bits8	   *bp;				/* ptr to null bitmap in tuple */

	/* XXX "knows" t_bits are just after fixed tuple header! */
	bp = (bits8 *) ((char *) tup + sizeof(IndexTupleData));

	tp = (char *) tup + IndexInfoFindDataOffset(tup->t_info);

	index_deform_tuple_internal(tupleDescriptor, values, isnull,
								tp, bp, IndexTupleHasNulls(tup));
}

/*
 * Convert an index tuple into Datum/isnull arrays,
 * without assuming any specific layout of the index tuple header.
 *
 * Caller must supply pointer to data area, pointer to nulls bitmap
 * (which can be NULL if !hasnulls), and hasnulls flag.
 */
void
index_deform_tuple_internal(TupleDesc tupleDescriptor,
							Datum *values, bool *isnull,
							char *tp, bits8 *bp, int hasnulls)
{
	CompactAttribute *cattr;
	int			natts = tupleDescriptor->natts; /* number of atts to extract */
	int			attnum = 0;
	int			off = 0;		/* offset in tuple data */
	int			cacheoffattrs;
	int			firstnullattr;

	/* Assert to protect callers who allocate fixed-size arrays */
	Assert(natts <= INDEX_MAX_KEYS);

	/* Did someone forget to call TupleDescFinalize()? */
	Assert(tupleDescriptor->firstNonCachedOffAttr >= 0);

	cacheoffattrs = Min(tupleDescriptor->firstNonCachedOffAttr, natts);

	if (hasnulls)
	{
		firstnullattr = first_null_attr(bp, natts);
		cacheoffattrs = Min(cacheoffattrs, firstnullattr);
	}
	else
		firstnullattr = natts;

	if (cacheoffattrs > 0)
	{
#ifdef USE_ASSERT_CHECKING
		/* In Assert enabled builds, verify attcacheoff is correct */
		off = 0;
#endif

		do
		{
			cattr = TupleDescCompactAttr(tupleDescriptor, attnum);

#ifdef USE_ASSERT_CHECKING
			off = att_nominal_alignby(off, cattr->attalignby);
			Assert(off == cattr->attcacheoff);
			off += cattr->attlen;
#endif

			values[attnum] = fetch_att(tp + cattr->attcacheoff, cattr->attbyval,
									   cattr->attlen);
			isnull[attnum] = false;
		} while (++attnum < cacheoffattrs);

		off = cattr->attcacheoff + cattr->attlen;
	}

	for (; attnum < firstnullattr; attnum++)
	{
		cattr = TupleDescCompactAttr(tupleDescriptor, attnum);

		off = att_pointer_alignby(off, cattr->attalignby, cattr->attlen,
								  tp + off);

		isnull[attnum] = false;
		values[attnum] = fetchatt(cattr, tp + off);

		off = att_addlength_pointer(off, cattr->attlen, tp + off);
	}

	for (; attnum < natts; attnum++)
	{
		Assert(hasnulls);

		if (att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			continue;
		}

		cattr = TupleDescCompactAttr(tupleDescriptor, attnum);
		off = att_pointer_alignby(off, cattr->attalignby, cattr->attlen,
								  tp + off);

		isnull[attnum] = false;
		values[attnum] = fetchatt(cattr, tp + off);

		off = att_addlength_pointer(off, cattr->attlen, tp + off);
	}
}

/*
 * Create a palloc'd copy of an index tuple.
 */
IndexTuple
CopyIndexTuple(IndexTuple source)
{
	IndexTuple	result;
	Size		size;

	size = IndexTupleSize(source);
	result = (IndexTuple) palloc(size);
	memcpy(result, source, size);
	return result;
}

/*
 * Create a palloc'd copy of an index tuple, leaving only the first
 * leavenatts attributes remaining.
 *
 * Truncation is guaranteed to result in an index tuple that is no
 * larger than the original.  It is safe to use the IndexTuple with
 * the original tuple descriptor, but caller must avoid actually
 * accessing truncated attributes from returned tuple!  In practice
 * this means that index_getattr() must be called with special care,
 * and that the truncated tuple should only ever be accessed by code
 * under caller's direct control.
 *
 * It's safe to call this function with a buffer lock held, since it
 * never performs external table access.  If it ever became possible
 * for index tuples to contain EXTERNAL TOAST values, then this would
 * have to be revisited.
 */
IndexTuple
index_truncate_tuple(TupleDesc sourceDescriptor, IndexTuple source,
					 int leavenatts)
{
	TupleDesc	truncdesc;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	IndexTuple	truncated;

	Assert(leavenatts <= sourceDescriptor->natts);

	/* Easy case: no truncation actually required */
	if (leavenatts == sourceDescriptor->natts)
		return CopyIndexTuple(source);

	/* Create temporary truncated tuple descriptor */
	truncdesc = CreateTupleDescTruncatedCopy(sourceDescriptor, leavenatts);

	/* Deform, form copy of tuple with fewer attributes */
	index_deform_tuple(source, truncdesc, values, isnull);
	truncated = index_form_tuple(truncdesc, values, isnull);
	truncated->t_tid = source->t_tid;
	Assert(IndexTupleSize(truncated) <= IndexTupleSize(source));

	/*
	 * Cannot leak memory here, TupleDescCopy() doesn't allocate any inner
	 * structure, so, plain pfree() should clean all allocated memory
	 */
	pfree(truncdesc);

	return truncated;
}
