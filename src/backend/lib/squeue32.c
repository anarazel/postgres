/*
 * An implementation of the algorithm from the paper "A practical nonblocking
 * queue algorithm using compare-and-swap" by Shann, Huang and Chen.  It
 * provides a fixed-sized queue of uint32 values, for multiple producers and
 * multiple consumers.
 *
 * The paper has {val, ref} items that can be atomically compared-and-swapped,
 * where val is a pointer to user data with NULL to indicate an empty slot, and
 * ref is a counter to avoid the ABA problem.  Instead, we encode the item into
 * a single uint64, but we steal the most significant bit from ref to indicate
 * an occupied slot.  That way we reserve the full range of a uint32 for user
 * data.
 *
 * XXX The "recheck" was added by me, and is needed to avoid skipping non-mepty
 * entries that are inserted by the signal context after we read x as empty!
 * Need to figure out why that isn't needed in the paper's algorithm...
 *
 * XXX This algorithm does two CASs, has a limited data size and a limited ABA
 * counter, Perhaps this would be better, and could even be done as a kind of
 * pseudo-template to support different sizes:
 *
 * http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
 */

#include "postgres.h"

#include "lib/squeue32.h"
#include "port/atomics.h"

#define VAL_SHIFT	32
#define OCC_MASK	((uint32) 0x80000000)
#define REF_MASK	((uint32) 0x7fffffff)

struct squeue32 {
	size_t		size;			/* L */
	pg_atomic_uint64 front;		/* FRONT */
	pg_atomic_uint64 rear;		/* REAR */
	pg_atomic_uint64 data[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * Check if an item is empty.
 */
static inline bool
squeue32_item_is_empty(uint64 item)
{
	return (item & OCC_MASK) == 0;
}

/*
 * Extract the value from an item.
 */
static inline uint32
squeue32_item_value(uint64 item)
{
	Assert(!squeue32_item_is_empty(item));
	return item >> VAL_SHIFT;
}

/*
 * Clear an item.
 */
static inline uint64
squeue32_clear_item(uint64 old_item)
{
	/* Incremented counter with no occupied flag. */
	return (old_item + 1) & REF_MASK;
}

/*
 * Store a new value in an item.
 */
static inline uint64
squeue32_update_item(uint32 value, uint64 old_item)
{
	/* New value, occupied flag and incremented counter. */
	return ((uint64) value << VAL_SHIFT) | OCC_MASK | ((old_item + 1) & REF_MASK);
}

/*
 * How much memory does it take to hold a queue with the given number of items?
 */
size_t
squeue32_estimate(size_t size)
{
	return offsetof(squeue32, data) + sizeof(pg_atomic_uint64) * size;
}

/*
 * Initialize a queue in a memory region that is sufficent in size according to
 * squeue32_estimate().
 */
void
squeue32_init(squeue32 *queue, size_t size)
{
	queue->size = size;
	pg_atomic_init_u64(&queue->front, 0);
	pg_atomic_init_u64(&queue->rear, 0);
	for (size_t i = 0; i < size; ++i)
		pg_atomic_init_u64(&queue->data[i], 0);
}

/*
 * Enqueue a value.  Returns true on success, and false if the queue is full.
 */
bool
squeue32_enqueue(squeue32 *queue, uint32 value)
{
	uint64		rear;
	uint64		x;
	size_t		slot;

enq_try_again:
	/* Read READ. */
	rear = pg_atomic_read_u64(&queue->rear);
	/* Read the rear of the list. */
	slot = rear % queue->size;
	x = pg_atomic_read_u64(&queue->data[slot]);
	/* Are read and x consistent? */
	if (rear != pg_atomic_read_u64(&queue->rear))
		goto enq_try_again;
	/* Is queue full? */
	if (rear == (pg_atomic_read_u64(&queue->front) + queue->size))
		return false;
	/* Is the rear of the list empty? */
	if (squeue32_item_is_empty(x))
	{
		/* Try to store an item. */
		if (pg_atomic_compare_exchange_u64(&queue->data[slot],
										   &x,
										   squeue32_update_item(value, x)))
		{
			/* Try to increment REAR. */
			pg_atomic_compare_exchange_u64(&queue->rear, &rear, rear + 1);
			return true;
		}
	}
	else
	{
		/* Help others increment REAR. */
		pg_atomic_compare_exchange_u64(&queue->rear, &rear, rear + 1);
	}
	/* Enqueue failed, try again. */
	goto enq_try_again;
}

/*
 * Dequeue a value.  Returns true on success, and false if the queue is empty.
 */
bool
squeue32_dequeue(squeue32 *queue, uint32 *value)
{
	uint64		front;
	uint64		x;
	size_t		slot;

deq_try_again:
	/* Read FRONT. */
	front = pg_atomic_read_u64(&queue->front);
	/* Read the front of the list. */
	slot = front % queue->size;
	x = pg_atomic_read_u64(&queue->data[slot]);
	/* Are front and x consistent? */
	if (front != pg_atomic_read_u64(&queue->front))
		goto deq_try_again;
	/* Is queue empty? */
	if (front == pg_atomic_read_u64(&queue->rear))
		return false;
	/* Is the front of the list non-empty? */
	if (!squeue32_item_is_empty(x))
	{
		/* Try to remove an item. */
		if (pg_atomic_compare_exchange_u64(&queue->data[slot],
										   &x,
										   squeue32_clear_item(x)))
		{
			/* Try to increment FRONT. */
			pg_atomic_compare_exchange_u64(&queue->front, &front, front + 1);
			*value = squeue32_item_value(x);
			return true;
		}
	}
	else if (x == pg_atomic_read_u64(&queue->data[slot])) /* XXX recheck */
	{
		/* Help others increment FRONT. */
		pg_atomic_compare_exchange_u64(&queue->front, &front, front + 1);
	}
	/* Dequeue failed, try again. */
	goto deq_try_again;
}
