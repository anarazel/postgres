# Asynchronous & Direct IO

## Design Criteria & Motivation

### Why Asynchronous IO

Until the introduction of asynchronous IO Postgres relied on the operating
system to hide the cost of synchronous IO from Postgres. While this worked
surprisingly well in a lot of workloads, it does not do as good a job on
prefetching and controlled writeback as we would like.

There are important expensive operations like `fdatasync()` where the operating
system cannot hide the storage latency. This is particularly important for WAL
writes, where the ability to asynchronously issue `fdatasync()` or O_DSYNC
writes can yield significantly higher throughput.


### Why Direct / unbuffered IO

The main reason to want to use Direct IO are:

- Lower CPU usage / higher throughput. Particularly on modern storage buffered
  writes are bottlenecked by the operating system having to copy data from the
  kernel's page cache to postgres buffer pool using the CPU. Whereas direct IO
  can often move the data directly between the storage devices and postgres'
  buffer cache, using DMA. While that transfer is ongoing, the CPU is free to
  perform other work.
- Reduced latency - Direct IO can have substantially lower latency than
  buffered IO, which can be impactful for OLTP workloads bottlenecked by WAL
  write latency.
- Avoiding double buffering between operating system cache and postgres'
  shared_buffers.
- Better control over the timing and pace of dirty data writeback.


The main reason *not* to use Direct IO are:

- Without AIO, Direct IO is unusably slow for most purposes.
- Even with AIO, many parts of postgres need to be modified to perform
  explicit prefetching.
- In situations where shared_buffers cannot be set appropriately large,
  e.g. because there are many different postgres instances hosted on shared
  hardware, performance will often be worse then when using buffered IO.


### Deadlock and Starvation Dangers due to AIO

Using AIO in a naive way can easily lead to deadlocks in an environment where
the source/target of AIO are shared resources, like pages in postgres'
shared_buffers.

Consider one backend performing readahead on a table, initiating IO for a
number of buffers ahead of the current "scan position". If that backend then
performs some operation that blocks, or even just is slow, the IO completion
for the asynchronously initiated read may not be processed.

This AIO implementation solves this problem by requiring that AIO methods
either allow AIO completions to be processed by any backend in the system
(e.g. io_uring), or to guarantee that AIO processing will happen even when the
issuing backend is blocked (e.g. worker mode, which offloads completion
processing to the AIO workers).


### IO can be started in critical sections

Using AIO for WAL writes can reduce the overhead of WAL logging substantially:

- AIO allows to start WAL writes eagerly, so they complete before needing to
  wait
- AIO allows to have multiple WAL flushes in progress at the same time
- AIO makes it more realistic to use O\_DIRECT + O\_DSYNC, which can reduce
  the number of roundtrips to storage on some OSs and storage HW (buffered IO
  and direct IO without O_DSYNC needs to issue a write and after the writes
  completion a cache cache flush, whereas O\_DIRECT + O\_DSYNC can use a
  single FUA write).

The need to be able to execute IO in critical sections has substantial design
implication on the AIO subsystem. Mainly because completing IOs (see prior
section) needs to be possible within a critical section, even if the
to-be-completed IO itself was not issued in a critical section. Consider
e.g. the case of a backend first starting a number of writes from shared
buffers and then starting to flush the WAL. Because only a limited amount of
IO can be in-progress at the same time, initiating the IO for flushing the WAL
may require to first finish executing IO executed earlier.


### State for AIO needs to live in shared memory

Because postgres uses a process model and because AIOs need to be
complete-able by any backend much of the state of the AIO subsystem needs to
live in shared memory.

In an `EXEC_BACKEND` build backends executable code and other process local
state is not necessarily mapped to the same addresses in each process due to
ASLR. This means that the shared memory cannot contain pointer to callbacks.


## Design of the AIO Subsystem


### AIO Methods

To achieve portability and performance, multiple methods of performing AIO are
implemented and others are likely worth adding in the future.


#### Synchronous Mode

`io_method=sync` does not actually perform AIO but allows to use the AIO API
while performing synchronous IO. This can be useful for debugging. The code
for the synchronous mode is also used as a fallback by e.g. the [worker
mode](#Worker) uses it to execute IO that cannot be executed by workers.


#### Worker

`io_method=worker` is available on every platform postgres runs on, and
implements asynchronous IO - from the view of the issuing process - by
dispatching the IO to one of several worker processes performing the IO in a
synchronous manner.


#### io_uring

`io_method=io_uring` is available on Linux 5.1+. In contrast to worker mode it
dispatches all IO from within the process, lowering context switch rate /
latency.


### AIO Handles

The central API piece for postgres' AIO abstraction are AIO handles. To
execute an IO one first has to acquire an IO handle (`pgaio_io_get()`) and
then "defined", i.e. associate an IO operation with the handle.

Often AIO handles are acquired on a higher level and then passed to a lower
level to be fully defined. E.g., for IO to/from shared buffers, bufmgr.c
routines acquire the handle, which is then passed through smgr.c, md.c to be
finally fully defined in fd.c.

The functions used at the lowest level to define the operation are
`pgaio_io_prep_*()`.

Because acquisition of an IO handle
[must always succeed](#IO-can-be-started-in-critical-sections)
and the number of AIO Handles
[has to be limited](#State-for-AIO-needs-to-live-in-shared-memory)
AIO handles can be reused as soon as they have completed. Obviously code needs
to be able to react to IO completion. Shared state can be updated using
[AIO Completion callbacks](#AIO-Callbacks)
and the issuing backend can provide a backend local variable to receive the
result of the IO, as described in
[AIO Result](#AIO-Result)
. An IO can be waited for, by both the issuing and any other backend, using
[AIO Referencs](#AIO-References).


Because an AIO Handle is not executable just after calling `pgaio_io_get()`
and because `pgaio_io_get()` needs to be able to succeed, only a single AIO
Handle may be acquired (i.e. returned by `pgaio_io_get()`) without causing the
IO to have been defined (by, potentially indirectly, causing
`pgaio_io_prep_*()` to have been called). Otherwise a backend could trivially
self-deadlock by using up all AIO Handles without the ability to wait for some
of the IOs to complete.

If it turns out that an AIO Handle is not needed, e.g., because the handle was
acquired before holding a contended lock, it can be released without being
defined using `pgaio_io_release()`.


### AIO Callbacks

Commonly several layers need to react to completion of an IO. E.g. for a read
md.c needs to check if the IO outright failed or was shorter than needed,
bufmgr.c needs to verify the page looks valid and bufmgr.c needs to update the
BufferDesc to update the buffer's state.

The fact that several layers / subsystems need to react to IO completion poses
a few challenges:

- Upper layers should not need to know details of lower layers. E.g. bufmgr.c
  should not assume the IO will pass through md.c.  Therefore upper levels
  cannot know what lower layers would consider an error.

- Lower layers should not need to know about upper layers. E.g. smgr APIs are
  used going through shared buffers but are also used bypassing shared
  buffers. This means that e.g. md.c is not in a position to validate
  checksums.

- Having code in the AIO subsystem for every possible combination of layers
  would lead to a lot of duplication.

The "solution" to this the ability to associate multiple completion callbacks
with a handle. E.g. bufmgr.c can have a callback to update the BufferDesc
state and to verify the page and md.c. another callback to check if the IO
operation was successful.

As [mentioned](#State-for-AIO-needs-to-live-in-shared-memory), shared memory
currently cannot contain function pointers. Because of that completion
callbacks are not directly identified by function pointers but by IDs
(`PgAioHandleSharedCallbackID`).  A substantial added benefit is that that
allows callbacks to be identified by much smaller amount of memory (a single
byte currently).

In addition to completion, AIO callbacks also are called to "prepare" an
IO. This is, e.g., used to acquire buffer pins owned by the AIO subsystem for
IO to/from shared buffers, which is required to handle the case where the
issuing backend errors out and releases its own pins.

As [explained earlier](#IO-can-be-started-in-critical-sections) IO completions
need to be safe to execute in critical sections. To allow the backend that
issued the IO to error out in case of failure [AIO Result](#AIO-Results) can
be used.


### AIO Subjects

In addition to the completion callbacks describe above, each AIO Handle has
exactly one "subject". Each subject has some space inside an AIO Handle with
information specific to the subject and can provide callbacks to allow to
reopen the underlying file (required for worker mode) and to describe the IO
operation (used for debug logging and error messages).


### AIO References

As [described above](#AIO-Handles) can be reused immediately after completion
and therefore cannot be used to wait for completion of the IO. Waiting is
enabled using AIO references, which do not just identify an AIO Handle but
also include the handles "generation".

A reference to an AIO Handle can be acquired using `pgaio_io_get_ref()` and
then waited upon using `pgaio_io_ref_wait()`.


### AIO Results

As AIO completion callbacks
[are executed in critical sections](#IO-can-be-started-in-critical-sections)
and [may be executed by any backend](#Deadlock-and-Starvation-Dangers-due-to-AIO)
completion callbacks cannot be used to, e.g., make the query that triggered an
IO ERROR out.

To allow to react to failing IOs the issuing backend can pass a pointer to a
`PgAioReturn` in backend local memory. Before an AIO Handle is reused the
`PgAioReturn` is filled with information about the IO. This includes
information about whether the IO was successful (as a value of
`PgAioResultStatus`) and enough information to raise an error in case of a
failure (via `pgaio_result_log()`, with the error details encoded in
`PgAioResult`).

XXX: "return" vs "result" vs "result status" seems quite confusing. The naming
should be improved.


### AIO Errors

It would be very convenient to have shared completion callbacks encode the
details of errors as an `ErrorData` that could be raised at a later
time. Unfortunately doing so would require allocating memory. While elog.c can
guarantee (well, kinda) that logging a message will not run out of memory,
that only works because a very limited number of messages are in the process
of being logged.  With AIO a large number of concurrently issued AIOs might
fail.

To avoid the need for preallocating a potentially large amount of memory (in
shared memory no less!), completion callbacks instead have to encode errors in
a more compact format that can be converted into an error message.


### AIO Bounce Buffers

For some uses of AIO there is no convenient memory location as the source /
destination of an AIO. E.g. when data checksums are enabled, writes from
shared buffers currently cannot be done directly from shared buffers, as a
shared buffer lock still allows some modification, e.g., for hint bits(see
`FlushBuffer()`). If the write were done in-place, such modifications can
cause the checksum to fail.

For synchronous IO this is solved by copying the buffer to separate memory
before computing the checksum and using that copy as the source buffer for the
AIO.

However, for AIO that is not a workable solution:
- Instead of a single buffer many buffers are required, as many IOs might be
  in flight
- When using the [worker method](#worker), the source/target of IO needs to be
  in shared memory, otherwise the workers won't be able to access the memory.

The AIO subsystem addresses this by providing a limited number of bounce
buffers that can be used as the source / target for IO. A bounce buffer be
acquired with `pgaio_bounce_buffer_get()` and multiple bounce buffers can be
associated with an AIO Handle with `pgaio_io_assoc_bounce_buffer()`.

Bounce buffers are automatically released when the IO completes.


## Helpers

Using the low-level AIO API introduces too much complexity to do so all over
the tree. Most uses of AIO should be done via reusable, higher-level, helpers.


### Read Stream

A common and very beneficial use of AIO are reads where a substantial number
of to-be-read locations are known ahead of time. E.g., for a sequential scan
the set of blocks that need to be read can be determined solely by knowing the
current position and checking the buffer mapping table.
[Read stream](../../include/storage/read_stream.h)
makes it comparatively easy to use AIO for such use cases.
