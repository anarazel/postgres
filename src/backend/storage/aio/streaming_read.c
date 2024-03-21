/*-------------------------------------------------------------------------
 *
 * streaming_read.c
 *	  Mechanism for buffer access with look-ahead
 *
 * Portions Copyright (c) 2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Code that needs to access relation data typically pins blocks one at a
 * time, often in a predictable order that might be sequential or data-driven.
 * Calling the simple ReadBuffer() function for each block is inefficient,
 * because blocks that are not yet in the buffer pool require I/O operations
 * that are small and might stall waiting for storage.  This mechanism looks
 * into the future and calls StartReadBuffers() and WaitReadBuffers() to
 * physically read multiple neighboring blocks in a single operation ahead of
 * time, with an adaptive look-ahead distance.
 *
 * Client code provides a callback that says which block number it would like
 * to appear in the stream next, one by one, and then pulls buffers out of the
 * stream one by one when it is ready to process them.  When each buffer is
 * returned to the client code, it is pinned exactly as if it had been
 * obtained by ReadBuffer(), but more buffers might be pinned ahead of time
 * behind the scenes as they are being read in.
 *
 * The decision of how far ahead to look is made by the streaming read
 * mechanism, without the client's knowledge, though it can be influenced with
 * hints.  The algorithm for controlling the look-ahead distance tries to
 * classify the stream into three ideal types:
 *
 * A) No I/O is necessary, because the requested blocks are fully cached
 * already.  There is no benefit to looking ahead more than one block, so
 * distance is 1.  This is the initial assumption.
 *
 * B) I/O is necessary, but it is sequential.  It would be useless or perhaps
 * even detrimental to issue prefetch advice, as kernels are expected to do a
 * better job at prefetching sequentially.  Therefore there is no benefit in
 * looking ahead more than MAX_BUFFERS_PER_TRANSFER (the largest physical read
 * size), because our only goal is to coalesce large read system calls.
 * Looking further ahead would only pin many buffers for no benefit, and
 * exercise the callback to do speculative work for no benefit if the client
 * code ends the stream early.
 *
 * C) I/O is necesssary, and it is random.  We limit our distance such that we
 * respect the configured I/O concurrency limits (appropriate GUC or table
 * space setting).  Any I/O that we've started and not yet waited for is
 * considered to be running.
 *
 * The distance increases rapidly and decays slowly, so that it moves towards
 * those levels as different I/O patterns are discovered.  For example, a
 * sequential scan of fully cached data doesn't bother looking ahead, but a
 * sequential scan that hits a region of uncached blocks will start issuing
 * increasingly wide read calls until it plateaus at MAX_BUFFERS_PER_TRANSFER.
 *
 * Note: When direct I/O is enabled, we never reach behavior C because
 * "advice" can't be used and we don't have real asynchronous I/O yet, so
 * there is no way to avoid I/O stalls.  Therefore, we move only between A and
 * B behavior and necessarily suffer I/O stalls in direct I/O mode, so we
 * benefit from coalescing reads, but concurrency will require more
 * infrastructure.
 *
 * The basic data structure is a circular queue of "ranges", each
 * corresponding to a read operation that will be performed for a group of
 * neighbouring blocks.
 *
 *     +------------+
 *     |            |
 *     +------------+
 *     | 10..25     | <-- tail range (buffers to be given to client next)
 *     +------------+
 *     | 26..27     |
 *     +------------+
 *     | 42..42     | <-- head range (to be extended by look-ahead next)
 *     +------------+
 *     |            |
 *     +------------+
 *
 * The "head" of the queue is the one that we are currently trying to grow by
 * looking ahead.  Whenever the current distance and I/O limit allow it, we
 * call the client's callback to get the next block number, and then we check
 * if it can be added to the end of the "head" range, or needs to start a new
 * one.  Extending an existing range is possible only if it's the next block
 * number after the range, and it hasn't reached MAX_BUFFERS_PER_TRANSFER yet.
 * It we can extend it, it's time to call StartReadBuffers(), which pins at
 * least one buffer in the requested range, and if not all of them (for
 * technical reasons, see StartReadBuffers()), we adjust the range to the size
 * actually processed and move the rest into a new head range.
 *
 * The "tail" of the queue is the one that the client is currently pulling
 * pinned buffers from, after calling WaitReadBuffers() to make sure the read
 * has finished (StartReadBuffers() indicated whether that is necessary).
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/aio/streaming_read.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "storage/streaming_read.h"
#include "utils/rel.h"
#include "utils/spccache.h"

/*
 * Element type for StreamingRead's circular array of block ranges.
 */
typedef struct StreamingReadRange
{
	bool		need_wait;
	bool		advice_issued;
	BlockNumber blocknum;
	int			nblocks;
	int			per_buffer_data_index;
	Buffer		buffers[MAX_BUFFERS_PER_TRANSFER];
	ReadBuffersOperation operation;
} StreamingReadRange;

/*
 * Streaming read object.
 */
struct StreamingRead
{
	int			max_ios;
	int			ios_in_progress;
	int			max_pinned_buffers;
	int			pinned_buffers;
	int			next_tail_buffer;
	int			distance;
	bool		started;
	bool		finished;
	bool		advice_enabled;

	StreamingReadBufferCB callback;
	void	   *callback_private;

	BufferAccessStrategy strategy;
	BufferManagerRelation bmr;
	ForkNumber	forknum;

	/* Sometimes we need to buffer one block for flow control. */
	BlockNumber unget_blocknum;
	void	   *unget_per_buffer_data;

	/* Next expected block, for detecting sequential access. */
	BlockNumber seq_blocknum;

	/* Space for optional per-buffer private data. */
	size_t		per_buffer_data_size;
	void	   *per_buffer_data;

	/* Circular buffer of ranges. */
	int			size;
	int			head;		/* Look-ahead pins buffers at this end */
	int			tail;		/* Client consumes from this end */
	StreamingReadRange ranges[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * Create a new streaming read object that can be used to perform the
 * equivalent of a series of ReadBuffer() calls for one fork of one relation.
 * Internally, it generates larger vectored reads where possible by looking
 * ahead.
 */
StreamingRead *
streaming_read_buffer_begin(int flags,
							BufferAccessStrategy strategy,
							BufferManagerRelation bmr,
							ForkNumber forknum,
							StreamingReadBufferCB callback,
							void *callback_private,
							size_t per_buffer_data_size)
{
	StreamingRead *stream;
	int			size;
	int			max_ios;
	uint32		max_pinned_buffers;
	Oid			tablespace_id;

	/*
	 * Make sure our bmr's smgr and persistent are populated.  The caller
	 * asserts that the storage manager will remain valid.
	 */
	if (!bmr.smgr)
	{
		bmr.smgr = RelationGetSmgr(bmr.rel);
		bmr.relpersistence = bmr.rel->rd_rel->relpersistence;
	}

	/*
	 * Decide how many assumed I/Os we will allow to run concurrently.  That
	 * is, advice to the kernel to tell it that we will soon read.  This
	 * number also affects how far we look ahead for opportunities to start
	 * more I/Os.
	 */
	tablespace_id = bmr.smgr->smgr_rlocator.locator.spcOid;
	if (!OidIsValid(MyDatabaseId) ||
		(bmr.rel && IsCatalogRelation(bmr.rel)) ||
		IsCatalogRelationOid(bmr.smgr->smgr_rlocator.locator.relNumber))
	{
		/*
		 * Avoid circularity while trying to look up tablespace settings or
		 * before spccache.c is ready.
		 */
		max_ios = effective_io_concurrency;
	}
	else if (flags & STREAMING_READ_MAINTENANCE)
		max_ios = get_tablespace_maintenance_io_concurrency(tablespace_id);
	else
		max_ios = get_tablespace_io_concurrency(tablespace_id);

	/*
	 * Choose a maximum number of buffers we're prepared to pin.  We try to
	 * pin fewer if we can, though.  We clamp it to at least
	 * MAX_BUFFER_PER_TRANSFER so that we can have a chance to build up a full
	 * sized read, even when max_ios is zero.
	 */
	max_pinned_buffers = Max(max_ios * 4, MAX_BUFFERS_PER_TRANSFER);

	/* Don't allow this backend to pin more than its share of buffers. */
	if (SmgrIsTemp(bmr.smgr))
		LimitAdditionalLocalPins(&max_pinned_buffers);
	else
		LimitAdditionalPins(&max_pinned_buffers);
	Assert(max_pinned_buffers > 0);

	/*
	 * stream->ranges is a circular buffer.  When it is empty, head == tail.
	 * When it is full, there is an empty element between head and tail.  The
	 * largest number of occupied ranges we could have is one per pinned
	 * buffer.
	 */
	size = max_pinned_buffers + 2; /* XXX 1 */

	stream = (StreamingRead *)
		palloc0(offsetof(StreamingRead, ranges) +
				sizeof(stream->ranges[0]) * size);

	stream->max_ios = max_ios;
	stream->per_buffer_data_size = per_buffer_data_size;
	stream->max_pinned_buffers = max_pinned_buffers;
	stream->strategy = strategy;
	stream->size = size;

	stream->bmr = bmr;
	stream->forknum = forknum;
	stream->callback = callback;
	stream->callback_private = callback_private;

	stream->unget_blocknum = InvalidBlockNumber;

#ifdef USE_PREFETCH

	/*
	 * This system supports prefetching advice.  As long as direct I/O isn't
	 * enabled, and the caller hasn't promised sequential access, we can use
	 * it.
	 */
	if ((io_direct_flags & IO_DIRECT_DATA) == 0 &&
		(flags & STREAMING_READ_SEQUENTIAL) == 0)
		stream->advice_enabled = true;
#endif

	/*
	 * Skip the initial ramp-up phase if the caller says we're going to be
	 * reading the whole relation.  This way we start out doing full-sized
	 * reads.
	 */
	if (flags & STREAMING_READ_FULL)
		stream->distance = Min(MAX_BUFFERS_PER_TRANSFER, stream->max_pinned_buffers);
	else
		stream->distance = 1;

	/*
	 * Space for the callback to store extra data along with each block.  Note
	 * that we need one more than max_pinned_buffers, so we can return a
	 * pointer to a slot that can't be overwritten until the next call.
	 */
	if (per_buffer_data_size)
		stream->per_buffer_data = palloc(per_buffer_data_size * size);

	return stream;
}

/*
 * Find the per-buffer data index for the Nth block of a range.
 */
static int
get_per_buffer_data_index(StreamingRead *stream, StreamingReadRange *range, int n)
{
	int			result;

	/*
	 * Find slot in the circular buffer of per-buffer data, without using the
	 * expensive % operator.
	 */
	result = range->per_buffer_data_index + n;
	while (result >= stream->size)
		result -= stream->size;
	Assert(result == (range->per_buffer_data_index + n) % stream->size);

	return result;
}

/*
 * Return a pointer to the per-buffer data by index.
 */
static void *
get_per_buffer_data_by_index(StreamingRead *stream, int per_buffer_data_index)
{
	return (char *) stream->per_buffer_data +
		stream->per_buffer_data_size * per_buffer_data_index;
}

/*
 * Return a pointer to the per-buffer data for the Nth block of a range.
 */
static void *
get_per_buffer_data(StreamingRead *stream, StreamingReadRange *range, int n)
{
	return get_per_buffer_data_by_index(stream,
										get_per_buffer_data_index(stream,
																  range,
																  n));
}

/*
 * Start reading the head range, and create a new head range.  The new head
 * range is returned.  It may not be empty, if StartReadBuffers() couldn't
 * start the entire range; in that case the returned range contains the
 * remaining portion of the range.
 */
static StreamingReadRange *
streaming_read_start_head_range(StreamingRead *stream)
{
	StreamingReadRange *head_range;
	StreamingReadRange *new_head_range;
	int			nblocks_pinned;
	int			flags;

	/* Caller should make sure we never exceed max_ios. */
	Assert((stream->ios_in_progress < stream->max_ios) ||
		   (stream->ios_in_progress == 0 && stream->max_ios == 0));

	/* Should only call if the head range has some blocks to read. */
	head_range = &stream->ranges[stream->head];
	Assert(head_range->nblocks > 0);

	/*
	 * If advice hasn't been suppressed, and this system supports it, this
	 * isn't a strictly sequential pattern, then we'll issue advice.
	 */
	if (stream->advice_enabled &&
		stream->max_ios > 0 &&
		stream->started &&
		head_range->blocknum != stream->seq_blocknum)
		flags = READ_BUFFERS_ISSUE_ADVICE;
	else
		flags = 0;

	/* Suppress advice on the first call, because it's too late to benefit. */
	if (!stream->started)
		stream->started = true;

	/* We shouldn't be trying to pin more buffers that we're allowed to. */
	Assert(stream->pinned_buffers + head_range->nblocks <= stream->max_pinned_buffers);

	/* Start reading as many blocks as we can from the head range. */
	nblocks_pinned = head_range->nblocks;
	head_range->need_wait =
		StartReadBuffers(stream->bmr,
						 head_range->buffers,
						 stream->forknum,
						 head_range->blocknum,
						 &nblocks_pinned,
						 stream->strategy,
						 flags,
						 &head_range->operation);

	Assert(stream->pinned_buffers <= stream->max_pinned_buffers);

	if (head_range->need_wait && (flags & READ_BUFFERS_ISSUE_ADVICE))
	{
		/*
		 * Since we've issued advice, we count an I/O in progress until we
		 * call WaitReadBuffers().
		 */
		head_range->advice_issued = true;
		stream->ios_in_progress++;
		Assert(stream->ios_in_progress <= stream->max_ios);
	}

	/*
	 * StartReadBuffers() might have pinned fewer blocks than we asked it to,
	 * but always at least one.
	 */
	Assert(nblocks_pinned <= head_range->nblocks);
	Assert(nblocks_pinned >= 1);
	stream->pinned_buffers += nblocks_pinned;

	/*
	 * Remember where the next block would be after that, so we can detect
	 * sequential access next time.
	 */
	stream->seq_blocknum = head_range->blocknum + nblocks_pinned;

	/*
	 * Create a new head range.  There must be space, because we have enough
	 * elements for every range to hold just one block, up to the pin limit.
	 */
	Assert(stream->size > stream->max_pinned_buffers);
	Assert((stream->head + 1) % stream->size != stream->tail);
	if (++stream->head == stream->size)
		stream->head = 0;
	new_head_range = &stream->ranges[stream->head];
	new_head_range->nblocks = 0;
	new_head_range->advice_issued = false;

	/*
	 * If we didn't manage to start the whole read above, we split the range,
	 * moving the remainder into the new head range.
	 */
	if (nblocks_pinned < head_range->nblocks)
	{
		int			nblocks_remaining = head_range->nblocks - nblocks_pinned;

		head_range->nblocks = nblocks_pinned;

		new_head_range->blocknum = head_range->blocknum + nblocks_pinned;
		new_head_range->nblocks = nblocks_remaining;
	}

	/* The new range has per-buffer data starting after the previous range. */
	new_head_range->per_buffer_data_index =
		get_per_buffer_data_index(stream, head_range, nblocks_pinned);

	return new_head_range;
}

/*
 * Ask the callback which block it would like us to read next, with a small
 * buffer in front to allow streaming_unget_block() to work.
 */
static BlockNumber
streaming_get_block(StreamingRead *stream, void *per_buffer_data)
{
	BlockNumber result;

	if (unlikely(stream->unget_blocknum != InvalidBlockNumber))
	{
		/*
		 * If we had to unget a block, now it is time to return that one
		 * again.
		 */
		result = stream->unget_blocknum;
		stream->unget_blocknum = InvalidBlockNumber;

		/*
		 * The same per_buffer_data element must have been used, and still
		 * contains whatever data the callback wrote into it.  So we just
		 * sanity-check that we were called with the value that
		 * streaming_unget_block() pushed back.
		 */
		Assert(per_buffer_data == stream->unget_per_buffer_data);
	}
	else
	{
		/* Use the installed callback directly. */
		result = stream->callback(stream, stream->callback_private, per_buffer_data);
	}

	return result;
}

/*
 * In order to deal with short reads in StartReadBuffers(), we sometimes need
 * to defer handling of a block until later.  This *must* be called with the
 * last value returned by streaming_get_block().
 */
static void
streaming_unget_block(StreamingRead *stream, BlockNumber blocknum, void *per_buffer_data)
{
	Assert(stream->unget_blocknum == InvalidBlockNumber);
	stream->unget_blocknum = blocknum;
	stream->unget_per_buffer_data = per_buffer_data;
}

static void
streaming_read_look_ahead(StreamingRead *stream)
{
	StreamingReadRange *range;

	/* If we're finished, don't look ahead. */
	if (stream->finished)
		return;

	/*
	 * We we've already started the maximum allowed number of I/Os, don't look
	 * ahead.  There is a special case for max_ios == 0.
	 */
	if (stream->max_ios > 0 && stream->ios_in_progress == stream->max_ios)
		return;

	/* Can't pin any more buffers. */
	if (stream->pinned_buffers == stream->distance)
		return;

	/*
	 * Keep trying to add new blocks to the end of the head range while doing
	 * so wouldn't exceed the distance limit.
	 */
	range = &stream->ranges[stream->head];
	while (stream->pinned_buffers + range->nblocks < stream->distance)
	{
		BlockNumber blocknum;
		void	   *per_buffer_data;

		/* Do we have a full-sized range? */
		if (range->nblocks == lengthof(range->buffers))
		{
			/* Start as much of it as we can. */
			range = streaming_read_start_head_range(stream);

			/* If we're now at the I/O limit, stop here. */
			if (stream->ios_in_progress == stream->max_ios)
				return;

			/*
			 * That might have only been partially started, but always
			 * processes at least one so that'll do for now.
			 */
			Assert(range->nblocks < lengthof(range->buffers));
		}

		/* Find per-buffer data slot for the next block. */
		per_buffer_data = get_per_buffer_data(stream, range, range->nblocks);

		/* Find out which block the callback wants to read next. */
		blocknum = streaming_get_block(stream, per_buffer_data);
		if (blocknum == InvalidBlockNumber)
		{
			/* End of stream. */
			stream->finished = true;
			break;
		}

		/*
		 * Is there a head range that we cannot extend, because the requested
		 * block is not consecutive?
		 */
		if (range->nblocks > 0 &&
			range->blocknum + range->nblocks != blocknum)
		{
			/* Yes.  Start it, so we can begin building a new one. */
			range = streaming_read_start_head_range(stream);

			/*
			 * It's possible that it was only partially started, and we have a
			 * new range with the remainder.  Keep starting I/Os until we get
			 * it all out of the way, or we hit the I/O limit.
			 */
			while (range->nblocks > 0 && stream->ios_in_progress < stream->max_ios)
				range = streaming_read_start_head_range(stream);

			/*
			 * We do have to worry about I/O capacity running out if the head
			 * range was split.  In that case we have to 'unget' the block
			 * returned by the callback.
			 */
			if (stream->ios_in_progress == stream->max_ios)
			{
				streaming_unget_block(stream, blocknum, per_buffer_data);
				return;
			}
		}

		/* If we have a new, empty range, initialize the start block. */
		if (range->nblocks == 0)
			range->blocknum = blocknum;

		/* This block extends the range by one. */
		Assert(range->blocknum + range->nblocks == blocknum);
		range->nblocks++;
	};

	/*
	 * Normally we don't start the head range, preferring to give it a chance
	 * to grow to full size once more buffers have been consumed.  In cases
	 * where that can't possibly happen, we might as well start the read
	 * immediately.
	 */
	if ((range->nblocks > 0 && stream->finished) ||
		(range->nblocks == stream->distance))
		streaming_read_start_head_range(stream);
}

Buffer
streaming_read_buffer_next(StreamingRead *stream, void **per_buffer_data)
{
	StreamingReadRange *tail_range;

	for (;;)
	{
		if (stream->tail != stream->head)
		{
			tail_range = &stream->ranges[stream->tail];

			/*
			 * Do we need to wait for a ReadBuffers operation to finish before
			 * returning the buffers in this range?
			 */
			if (tail_range->need_wait)
			{
				int			distance;

				Assert(stream->next_tail_buffer == 0);
				WaitReadBuffers(&tail_range->operation);
				tail_range->need_wait = false;

				/*
				 * We don't really know if the kernel generated a physical I/O
				 * when we issued advice, let alone when it finished, but it
				 * has certainly finished now because we've performed the
				 * read.
				 */
				if (tail_range->advice_issued)
				{

					Assert(stream->ios_in_progress > 0);
					stream->ios_in_progress--;

					/*
					 * Look-ahead distance ramps up rapidly if we're issuing
					 * advice, so we can search for new more I/Os to start.
					 */
					distance = stream->distance * 2;
					distance = Min(distance, stream->max_pinned_buffers);
					stream->distance = distance;
				}
				else
				{
					/*
					 * There is no point in increasing look-ahead distance if
					 * we've already reached the full I/O size, since we're
					 * not issuing advice.  Extra distance would only pin more
					 * buffers for no benefit.
					 */
					if (stream->distance > MAX_BUFFERS_PER_TRANSFER)
					{
						/*
						 * Look-ahead distance gradually decays to full I/O
						 * size.
						 */
						stream->distance--;
					}
					else
					{
						/*
						 * Look-ahead distance ramps up rapidly, but not more
						 * that the full I/O size.
						 */
						distance = stream->distance * 2;
						distance = Min(distance, MAX_BUFFERS_PER_TRANSFER);
						distance = Min(distance, stream->max_pinned_buffers);
						stream->distance = distance;
					}
				}
			}
			else if (stream->next_tail_buffer == 0)
			{
				/* No I/O necessary. Look-ahead distance gradually decays. */
				if (stream->distance > 1)
					stream->distance--;
			}

			/* Are there more buffers available in this range? */
			if (stream->next_tail_buffer < tail_range->nblocks)
			{
				int			buffer_index;
				Buffer		buffer;

				buffer_index = stream->next_tail_buffer++;
				buffer = tail_range->buffers[buffer_index];

				Assert(BufferIsValid(buffer));

				/* We are giving away ownership of this pinned buffer. */
				Assert(stream->pinned_buffers > 0);
				stream->pinned_buffers--;

				if (per_buffer_data)
					*per_buffer_data = get_per_buffer_data(stream, tail_range, buffer_index);

				/* We may be able to get another I/O started. */
				streaming_read_look_ahead(stream);

				return buffer;
			}

			/* Advance tail to next range. */
			if (++stream->tail == stream->size)
				stream->tail = 0;
			stream->next_tail_buffer = 0;
		}
		else
		{
			/*
			 * If tail crashed into head, and head is not empty, then it is
			 * time to start that range.  Otherwise, force a look-ahead, to
			 * kick start the stream.
			 */
			Assert(stream->tail == stream->head);
			if (stream->ranges[stream->head].nblocks > 0)
			{
				streaming_read_start_head_range(stream);
			}
			else
			{
				streaming_read_look_ahead(stream);

				/* Finished? */
				if (stream->tail == stream->head &&
					stream->ranges[stream->head].nblocks == 0)
					break;
			}
		}
	}

	Assert(stream->pinned_buffers == 0);

	return InvalidBuffer;
}

void
streaming_read_buffer_end(StreamingRead *stream)
{
	Buffer		buffer;

	/* Stop looking ahead. */
	stream->finished = true;

	/* Unpin anything that wasn't consumed. */
	while ((buffer = streaming_read_buffer_next(stream, NULL)) != InvalidBuffer)
		ReleaseBuffer(buffer);

	Assert(stream->pinned_buffers == 0);
	Assert(stream->ios_in_progress == 0);

	/* Release memory. */
	if (stream->per_buffer_data)
		pfree(stream->per_buffer_data);

	pfree(stream);
}
