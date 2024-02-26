#ifndef STREAMING_READ_H
#define STREAMING_READ_H

#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/smgr.h"

/* Default tuning, reasonable for many users. */
#define STREAMING_READ_DEFAULT 0x00

/*
 * I/O streams that are performing maintenance work on behalf of potentially
 * many users.
 */
#define STREAMING_READ_MAINTENANCE 0x01

/*
 * We usually avoid issuing prefetch advice automatically when sequential
 * access is detected, but this flag explicitly disables it, for cases that
 * might not be correctly detected.  Explicit advice is known to perform worse
 * than letting the kernel (at least Linux) detect sequential access.
 */
#define STREAMING_READ_SEQUENTIAL 0x02

/*
 * We usually ramp up from smaller reads to larger ones, to support users who
 * don't know if it's worth reading lots of buffers yet.  This flag disables
 * that, declaring ahead of time that we'll be reading all available buffers.
 */
#define STREAMING_READ_FULL 0x04

struct StreamingRead;
typedef struct StreamingRead StreamingRead;

/* Callback that returns the next block number to read. */
typedef BlockNumber (*StreamingReadBufferCB) (StreamingRead *stream,
											  void *callback_private_data,
											  void *per_buffer_data);

extern StreamingRead *streaming_read_buffer_begin(int flags,
												  BufferAccessStrategy strategy,
												  BufferManagerRelation bmr,
												  ForkNumber forknum,
												  StreamingReadBufferCB callback,
												  void *callback_private_data,
												  size_t per_buffer_data_size);
extern Buffer streaming_read_buffer_next(StreamingRead *stream, void **per_buffer_private);
extern void streaming_read_buffer_end(StreamingRead *stream);

#endif
