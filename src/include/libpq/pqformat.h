/*-------------------------------------------------------------------------
 *
 * pqformat.h
 *		Definitions for formatting and parsing frontend/backend messages
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pqformat.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQFORMAT_H
#define PQFORMAT_H

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "port/pg_bswap.h"
#include "varatt.h"

extern void pq_beginmessage(StringInfo buf, char msgtype);
extern void pq_beginmessage_reuse(StringInfo buf, char msgtype);
extern void pq_endmessage(StringInfo buf);
extern void pq_endmessage_reuse(StringInfo buf);

extern void pq_sendcountedtext(StringInfo buf, const char *str, int slen,
							   bool countincludesself);
extern void pq_sendtext(StringInfo buf, const char *str, int slen);
extern void pq_sendstring(StringInfo buf, const char *str);
extern void pq_send_ascii_string(StringInfo buf, const char *str);
extern void pq_sendfloat4(StringInfo buf, float4 f);
extern void pq_sendfloat8(StringInfo buf, float8 f);


/* --------------------------------
 *		pq_begintypsend		- initialize for constructing a bytea result
 * --------------------------------
 */
static inline void
pq_begintypsend(StringInfo buf)
{
	initStringInfo(buf);

	/*
	 * Reserve four bytes for the bytea length word.  We don't need to fill
	 * them with anything (pq_endtypsend will do that), and this function is
	 * enough of a hot spot that it's worth cheating to save some cycles. Note
	 * in particular that we don't bother to guarantee that the buffer is
	 * null-terminated.
	 */
	Assert(buf->maxlen > 4);
	buf->len = 4;
}

/* --------------------------------
 *		pq_begintypsend_with_size - like pq_begintypesend, but with length hint
 *
 * This can be used over pq_begintypesend if the caller can cheaply determine
 * how much data will be sent, reducing the initial size of the
 * StringInfo. The passed in size need not include the overhead of the length
 * word.
 * --------------------------------
 */
static inline void
pq_begintypsend_with_size(StringInfo buf, int size)
{
	initStringInfoWithSize(buf, size + 4);
	/* Reserve four bytes for the bytea length word */
	appendStringInfoSpaces(buf, 4);
}

static inline void
pq_begintypsend_res(StringInfo buf)
{
	Assert(buf && buf->data && buf->len == 0);

	buf->len = 4;
}

/* --------------------------------
 *		pq_endtypsend	- finish constructing a bytea result
 *
 * The data buffer is returned as the palloc'd bytea value.  (We expect
 * that it will be suitably aligned for this because it has been palloc'd.)
 * We assume the StringInfoData is just a local variable in the caller and
 * need not be pfree'd.
 * --------------------------------
 */
static inline bytea *
pq_endtypsend(StringInfo buf)
{
	bytea	   *result = (bytea *) buf->data;

	/* Insert correct length into bytea length word */
	Assert(buf->len >= VARHDRSZ);
	SET_VARSIZE(result, buf->len);

	return result;
}

/*
 * Append a [u]int8 to a StringInfo buffer, which already has enough space
 * preallocated.
 *
 * The use of pg_restrict allows the compiler to optimize the code based on
 * the assumption that buf, buf->len, buf->data and *buf->data don't
 * overlap. Without the annotation buf->len etc cannot be kept in a register
 * over subsequent pq_writeintN calls.
 *
 * The use of StringInfoData * rather than StringInfo is due to MSVC being
 * overly picky and demanding a * before a restrict.
 */
static inline void
pq_writeint8(StringInfoData *pg_restrict buf, uint8 i)
{
	uint8		ni = i;

	Assert(buf->len + (int) sizeof(uint8) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint8));
	buf->len += sizeof(uint8);
}

/*
 * Append a [u]int16 to a StringInfo buffer, which already has enough space
 * preallocated.
 */
static inline void
pq_writeint16(StringInfoData *pg_restrict buf, uint16 i)
{
	uint16		ni = pg_hton16(i);

	Assert(buf->len + (int) sizeof(uint16) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint16));
	buf->len += sizeof(uint16);
}

/*
 * Append a [u]int32 to a StringInfo buffer, which already has enough space
 * preallocated.
 */
static inline void
pq_writeint32(StringInfoData *pg_restrict buf, uint32 i)
{
	uint32		ni = pg_hton32(i);

	Assert(buf->len + (int) sizeof(uint32) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint32));
	buf->len += sizeof(uint32);
}

/*
 * Append a [u]int64 to a StringInfo buffer, which already has enough space
 * preallocated.
 */
static inline void
pq_writeint64(StringInfoData *pg_restrict buf, uint64 i)
{
	uint64		ni = pg_hton64(i);

	Assert(buf->len + (int) sizeof(uint64) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint64));
	buf->len += sizeof(uint64);
}

/*
 * Append a null-terminated text string (with conversion) to a buffer with
 * preallocated space.
 *
 * NB: The pre-allocated space needs to be sufficient for the string after
 * converting to client encoding.
 *
 * NB: passed text string must be null-terminated, and so is the data
 * sent to the frontend.
 */
static inline void
pq_writestring(StringInfoData *pg_restrict buf, const char *pg_restrict str)
{
	int			slen = strlen(str);
	char	   *p;

	p = pg_server_to_client(str, slen);
	if (p != str)				/* actual conversion has been done? */
		slen = strlen(p);

	Assert(buf->len + slen + 1 <= buf->maxlen);

	memcpy(((char *pg_restrict) buf->data + buf->len), p, slen + 1);
	buf->len += slen + 1;

	if (p != str)
		pfree(p);
}

/* append a binary [u]int8 to a StringInfo buffer */
static inline void
pq_sendint8(StringInfo buf, uint8 i)
{
	enlargeStringInfo(buf, sizeof(uint8));
	pq_writeint8(buf, i);
}

/* append a binary [u]int16 to a StringInfo buffer */
static inline void
pq_sendint16(StringInfo buf, uint16 i)
{
	enlargeStringInfo(buf, sizeof(uint16));
	pq_writeint16(buf, i);
}

/* append a binary [u]int32 to a StringInfo buffer */
static inline void
pq_sendint32(StringInfo buf, uint32 i)
{
	enlargeStringInfo(buf, sizeof(uint32));
	pq_writeint32(buf, i);
}

/* append a binary [u]int64 to a StringInfo buffer */
static inline void
pq_sendint64(StringInfo buf, uint64 i)
{
	enlargeStringInfo(buf, sizeof(uint64));
	pq_writeint64(buf, i);
}

/* append a binary byte to a StringInfo buffer */
static inline void
pq_sendbyte(StringInfo buf, uint8 byt)
{
	pq_sendint8(buf, byt);
}

static inline void
pq_sendbytes(StringInfo buf, const void *data, int datalen)
{
	/*
	 * FIXME: used to say: use variant that maintains a trailing null-byte,
	 * out of caution but that doesn't make much sense to me, and proves to be
	 * a performance issue.
	 */
	appendBinaryStringInfoNT(buf, data, datalen);
}

/*
 * Append a binary integer to a StringInfo buffer
 *
 * This function is deprecated; prefer use of the functions above.
 */
static inline void
pq_sendint(StringInfo buf, uint32 i, int b)
{
	switch (b)
	{
		case 1:
			pq_sendint8(buf, (uint8) i);
			break;
		case 2:
			pq_sendint16(buf, (uint16) i);
			break;
		case 4:
			pq_sendint32(buf, (uint32) i);
			break;
		default:
			elog(ERROR, "unsupported integer size %d", b);
			break;
	}
}


extern void pq_begintypsend(StringInfo buf);
extern bytea *pq_endtypsend(StringInfo buf);

extern void pq_puttextmessage(char msgtype, const char *str);
extern void pq_putemptymessage(char msgtype);

extern int	pq_getmsgbyte(StringInfo msg);
extern unsigned int pq_getmsgint(StringInfo msg, int b);
extern int64 pq_getmsgint64(StringInfo msg);
extern float4 pq_getmsgfloat4(StringInfo msg);
extern float8 pq_getmsgfloat8(StringInfo msg);
extern const char *pq_getmsgbytes(StringInfo msg, int datalen);
extern void pq_copymsgbytes(StringInfo msg, char *buf, int datalen);
extern char *pq_getmsgtext(StringInfo msg, int rawbytes, int *nbytes);
extern const char *pq_getmsgstring(StringInfo msg);
extern const char *pq_getmsgrawstring(StringInfo msg);
extern void pq_getmsgend(StringInfo msg);

#endif							/* PQFORMAT_H */
