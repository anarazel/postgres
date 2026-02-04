/*-------------------------------------------------------------------------
 *
 * tupmacs.h
 *	  Tuple macros used by both index tuples and heap tuples.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupmacs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPMACS_H
#define TUPMACS_H

#include "catalog/pg_type_d.h"	/* for TYPALIGN macros */
#include "port/pg_bitutils.h"
#include "varatt.h"

/*
 * Check a tuple's null bitmap to determine whether the attribute is null.
 * Note that a 0 in the null bitmap indicates a null, while 1 indicates
 * non-null.
 */
static inline bool
att_isnull(int ATT, const bits8 *BITS)
{
	return !(BITS[ATT >> 3] & (1 << (ATT & 0x07)));
}

/*
 * populate_isnull_array
 *		Transform a tuple's null bitmap into a boolean array.
 *
 * Caller must ensure that the isnull array is sized so it contains
 * at least as many elements as there are bits in the 'bits' array.
 * This is required because we always round 'natts' up to the next multiple
 * of 8.
 */
static inline void
populate_isnull_array(const bits8 *bits, int natts, bool *isnull)
{
	int			nbytes = (natts + 7) >> 3;

	/*
	 * Multiplying a NULL bitmap byte by this value results in the lowest bit
	 * in each byte being set the same as each bit of the bitmap.  We perform
	 * this as 2 32-bit operations rather than a single 64-bit operation as
	 * multiplying by the required value to do this in 64-bits would result in
	 * overflowing a uint64 in some cases.
	 */
#define SPREAD_BITS_MULTIPLIER_32 0x204081U

	for (int i = 0; i < nbytes; i++, isnull += 8)
	{
		uint64		isnull_8;
		bits8		nullbyte = ~bits[i];

		/* convert the lower 4 bits of null bitmap word into 32 bit int */
		isnull_8 = (nullbyte & 0xf) * SPREAD_BITS_MULTIPLIER_32;

		/*
		 * convert the upper 4 bits of null bitmap word into 32 bit int, shift
		 * into the upper 32 bit
		 */
		isnull_8 |= ((uint64) ((nullbyte >> 4) * SPREAD_BITS_MULTIPLIER_32)) << 32;

		/* mask out all other bits apart from the lowest bit of each byte */
		isnull_8 &= UINT64CONST(0x0101010101010101);
		memcpy(isnull, &isnull_8, sizeof(uint64));
	}
}

#ifndef FRONTEND
/*
 * Given an attbyval and an attlen from either a Form_pg_attribute or
 * CompactAttribute and a pointer into a tuple's data area, return the
 * correct value or pointer.
 *
 * We return a Datum value in all cases.  If attbyval is false,  we return the
 * same pointer into the tuple data area that we're passed.  Otherwise, we
 * return the correct number of bytes fetched from the data area and extended
 * to Datum form.
 *
 * Note that T must already be properly aligned for this to work correctly.
 */
#define fetchatt(A,T) fetch_att(T, (A)->attbyval, (A)->attlen)

/*
 * Same, but work from byval/len parameters rather than Form_pg_attribute.
 */
static inline Datum
fetch_att(const void *T, bool attbyval, int attlen)
{
	if (attbyval)
	{
		switch (attlen)
		{
			case sizeof(char):
				return CharGetDatum(*((const char *) T));
			case sizeof(int16):
				return Int16GetDatum(*((const int16 *) T));
			case sizeof(int32):
				return Int32GetDatum(*((const int32 *) T));
			case sizeof(int64):
				return Int64GetDatum(*((const int64 *) T));
			default:
				elog(ERROR, "unsupported byval length: %d", attlen);
				return 0;
		}
	}
	else
		return PointerGetDatum(T);
}

/*
 * Same, but no error checking for invalid attlens for byval types.  This
 * is safe to use when attlen comes from CompactAttribute as we validate the
 * length when populating that struct.
 */
static inline Datum
fetch_att_noerr(const void *T, bool attbyval, int attlen)
{
	if (attbyval)
	{
		switch (attlen)
		{
			case sizeof(int32):
				return Int32GetDatum(*((const int32 *) T));
			case sizeof(int16):
				return Int16GetDatum(*((const int16 *) T));
			case sizeof(char):
				return CharGetDatum(*((const char *) T));
			default:
				Assert(attlen == sizeof(int64));
				return Int64GetDatum(*((const int64 *) T));
		}
	}
	else
		return PointerGetDatum(T);
}


/*
 * align_fetch_then_add
 *		Applies all the functionality of att_pointer_alignby(), fetch_att()
 *		and att_addlength_pointer() resulting in *off pointer to the perhaps
 *		unaligned number of bytes into 'tupptr', ready to deform the next
 *		attribute.
 *
 * tupptr: pointer to the beginning of the tuple, after the header and any
 * NULL bitmask.
 * off: offset in bytes for reading tuple data, possibly unaligned.
 * attbyval, attlen, attalignby are values from CompactAttribute.
 */
static inline Datum
align_fetch_then_add(const char *tupptr, uint32 *off, bool attbyval, int attlen,
					 uint8 attalignby)
{
	Datum		res;

	if (attlen > 0)
	{
		const char *offset_ptr;

		*off = TYPEALIGN(attalignby, *off);
		offset_ptr = tupptr + *off;
		*off += attlen;
		if (attbyval)
		{
			switch (attlen)
			{
				case sizeof(char):
					return CharGetDatum(*((const char *) offset_ptr));
				case sizeof(int16):
					return Int16GetDatum(*((const int16 *) offset_ptr));
				case sizeof(int32):
					return Int32GetDatum(*((const int32 *) offset_ptr));
				default:

					/*
					 * populate_compact_attribute_internal() should have
					 * checked
					 */
					Assert(attlen == sizeof(int64));
					return Int64GetDatum(*((const int64 *) offset_ptr));
			}
		}
		return PointerGetDatum(offset_ptr);
	}
	else if (attlen == -1)
	{
		if (!VARATT_IS_SHORT(tupptr + *off))
			*off = TYPEALIGN(attalignby, *off);

		res = PointerGetDatum(tupptr + *off);
		*off += VARSIZE_ANY(DatumGetPointer(res));
		return res;
	}
	else
	{
		Assert(attlen == -2);
		*off = TYPEALIGN(attalignby, *off);
		res = PointerGetDatum(tupptr + *off);
		*off += strlen(tupptr + *off) + 1;
		return res;
	}
}

/*
 * first_null_attr
 *		Inspect a NULL bitmask from a tuple and return the 0-based attnum of the
 *		first NULL attribute.  Returns natts if no NULLs were found.
 *
 * We expect that 'bits' contains at least one 0 bit somewhere in the mask,
 * not necessarily < natts.
 */
static inline int
first_null_attr(const bits8 *bits, int natts)
{
	int			lastByte = natts >> 3;
	int			bytenum;
	int			res;

#ifdef USE_ASSERT_CHECKING
	int			firstnull_check = natts;

	/* Do it the slow way and check we get the same answer. */
	for (int i = 0; i < natts; i++)
	{
		if (att_isnull(i, bits))
		{
			firstnull_check = i;
			break;
		}
	}
#endif

	/* Process all bytes up to just before the byte for the natts index */
	for (bytenum = 0; bytenum < lastByte; bytenum++)
	{
		/* break if there's any NULL attrs (a 0 bit) */
		if (bits[bytenum] != 0xFF)
			break;
	}

	res = bytenum << 3;
	res += pg_rightmost_one_pos32(~bits[bytenum]);

	/*
	 * Since we did no masking to mask out bits beyond natts, we may have
	 * found a bit higher than natts, so we must cap to natts
	 */
	res = Min(res, natts);

	Assert(res == firstnull_check);

	return res;
}
#endif							/* FRONTEND */

/*
 * typalign_to_alignby: map a TYPALIGN_xxx value to the numeric alignment
 * value it represents.  (We store TYPALIGN_xxx codes not the real alignment
 * values mainly so that initial catalog contents can be machine-independent.)
 */
static inline uint8
typalign_to_alignby(char typalign)
{
	uint8		alignby;

	switch (typalign)
	{
		case TYPALIGN_CHAR:
			alignby = sizeof(char);
			break;
		case TYPALIGN_SHORT:
			alignby = ALIGNOF_SHORT;
			break;
		case TYPALIGN_INT:
			alignby = ALIGNOF_INT;
			break;
		case TYPALIGN_DOUBLE:
			alignby = ALIGNOF_DOUBLE;
			break;
		default:
#ifndef FRONTEND
			elog(ERROR, "invalid typalign value: %c", typalign);
#else
			fprintf(stderr, "invalid typalign value: %c\n", typalign);
			exit(1);
#endif
			alignby = 0;
			break;
	}
	return alignby;
}

/*
 * att_align_datum aligns the given offset as needed for a datum of alignment
 * requirement attalign and typlen attlen.  attdatum is the Datum variable
 * we intend to pack into a tuple (it's only accessed if we are dealing with
 * a varlena type).  Note that this assumes the Datum will be stored as-is;
 * callers that are intending to convert non-short varlena datums to short
 * format have to account for that themselves.
 */
#define att_align_datum(cur_offset, attalign, attlen, attdatum) \
( \
	((attlen) == -1 && VARATT_IS_SHORT(DatumGetPointer(attdatum))) ? \
	(uintptr_t) (cur_offset) : \
	att_align_nominal(cur_offset, attalign) \
)

/*
 * Similar to att_align_datum, but accepts a number of bytes, typically from
 * CompactAttribute.attalignby to align the Datum by.
 */
#define att_datum_alignby(cur_offset, attalignby, attlen, attdatum) \
	( \
	((attlen) == -1 && VARATT_IS_SHORT(DatumGetPointer(attdatum))) ? \
	(uintptr_t) (cur_offset) : \
	TYPEALIGN(attalignby, cur_offset))

/*
 * att_align_pointer performs the same calculation as att_align_datum,
 * but is used when walking a tuple.  attptr is the current actual data
 * pointer; when accessing a varlena field we have to "peek" to see if we
 * are looking at a pad byte or the first byte of a 1-byte-header datum.
 * (A zero byte must be either a pad byte, or the first byte of a correctly
 * aligned 4-byte length word; in either case we can align safely.  A non-zero
 * byte must be either a 1-byte length word, or the first byte of a correctly
 * aligned 4-byte length word; in either case we need not align.)
 *
 * Note: some callers pass a "char *" pointer for cur_offset.  This is
 * a bit of a hack but should work all right as long as uintptr_t is the
 * correct width.
 */
#define att_align_pointer(cur_offset, attalign, attlen, attptr) \
( \
	((attlen) == -1 && VARATT_NOT_PAD_BYTE(attptr)) ? \
	(uintptr_t) (cur_offset) : \
	att_align_nominal(cur_offset, attalign) \
)

/*
 * Similar to att_align_pointer, but accepts a number of bytes, typically from
 * CompactAttribute.attalignby to align the pointer by.
 */
#define att_pointer_alignby(cur_offset, attalignby, attlen, attptr) \
	( \
	((attlen) == -1 && VARATT_NOT_PAD_BYTE(attptr)) ? \
	(uintptr_t) (cur_offset) : \
	TYPEALIGN(attalignby, cur_offset))

/*
 * att_align_nominal aligns the given offset as needed for a datum of alignment
 * requirement attalign, ignoring any consideration of packed varlena datums.
 * There are three main use cases for using this macro directly:
 *	* we know that the att in question is not varlena (attlen != -1);
 *	  in this case it is cheaper than the above macros and just as good.
 *	* we need to estimate alignment padding cost abstractly, ie without
 *	  reference to a real tuple.  We must assume the worst case that
 *	  all varlenas are aligned.
 *	* within arrays and multiranges, we unconditionally align varlenas (XXX this
 *	  should be revisited, probably).
 *
 * In performance-critical loops, avoid using this macro; instead use
 * att_nominal_alignby with a pre-computed alignby value.
 */
#define att_align_nominal(cur_offset, attalign) \
	att_nominal_alignby(cur_offset, typalign_to_alignby(attalign))

/*
 * Similar to att_align_nominal, but accepts a number of bytes, typically from
 * CompactAttribute.attalignby to align the offset by.
 */
#define att_nominal_alignby(cur_offset, attalignby) \
	TYPEALIGN(attalignby, cur_offset)

/*
 * att_addlength_datum increments the given offset by the space needed for
 * the given Datum variable.  attdatum is only accessed if we are dealing
 * with a variable-length attribute.
 */
#define att_addlength_datum(cur_offset, attlen, attdatum) \
	att_addlength_pointer(cur_offset, attlen, DatumGetPointer(attdatum))

/*
 * att_addlength_pointer performs the same calculation as att_addlength_datum,
 * but is used when walking a tuple --- attptr is the pointer to the field
 * within the tuple.
 *
 * Note: some callers pass a "char *" pointer for cur_offset.  This is
 * actually perfectly OK, but probably should be cleaned up along with
 * the same practice for att_align_pointer.
 */
#define att_addlength_pointer(cur_offset, attlen, attptr) \
( \
	((attlen) > 0) ? \
	( \
		(cur_offset) + (attlen) \
	) \
	: (((attlen) == -1) ? \
	( \
		(cur_offset) + VARSIZE_ANY(attptr) \
	) \
	: \
	( \
		AssertMacro((attlen) == -2), \
		(cur_offset) + (strlen((const char *) (attptr)) + 1) \
	)) \
)

#ifndef FRONTEND
/*
 * store_att_byval is a partial inverse of fetch_att: store a given Datum
 * value into a tuple data area at the specified address.  However, it only
 * handles the byval case, because in typical usage the caller needs to
 * distinguish by-val and by-ref cases anyway, and so a do-it-all function
 * wouldn't be convenient.
 */
static inline void
store_att_byval(void *T, Datum newdatum, int attlen)
{
	switch (attlen)
	{
		case sizeof(char):
			*(char *) T = DatumGetChar(newdatum);
			break;
		case sizeof(int16):
			*(int16 *) T = DatumGetInt16(newdatum);
			break;
		case sizeof(int32):
			*(int32 *) T = DatumGetInt32(newdatum);
			break;
		case sizeof(int64):
			*(int64 *) T = DatumGetInt64(newdatum);
			break;
		default:
			elog(ERROR, "unsupported byval length: %d", attlen);
	}
}
#endif							/* FRONTEND */

#endif							/* TUPMACS_H */
