# Asynchronous & Direct IO

## Why Asynchronous IO

Until the introduction of asynchronous IO Postgres relied on the
operating system to hide the cost of synchronous IO from
Postgres. While this worked surprisingly well in a lot of workloads,
it does not do as good a job on prefetching and controlled writeback
as we would like.

There are important expensive operations like fdatasync() where the
operating system cannot hide the storage latency. This is particularly
important for WAL writes, where the ability to asynchronously issue
fdatasync() or O_DSYNC writes can yield significantly higher
throughput.


## Why Direct / unbuffered IO

The main reason to want to use Direct IO are:

- Lower CPU usage / higher throughput. Particularly on modern storage
  buffered writes are bottlenecked by the operating system having to
  copy data from the kernel's page cache to postgres buffer pool using
  the CPU. Whereas direct IO can often move the data directly between
  the storage devices and postgres' buffer cache, using DMA. While
  that transfer is ongoing, the CPU is free to perform other work,
  with little impact.
- Avoiding double buffering between operating system cache and
  postgres' shared_buffers.
- Better control over the timing and pace of dirty data writeback.


The main reason *not* to use Direct IO are:

- Without AIO, Direct IO is unusably slow for most purposes.
- Even with AIO, many parts of postgres need to be modified to perform
  explicit prefetching.
- In situations where shared_buffers cannot be set appropriately
  large, e.g. because there are many different postgres instances
  hosted on shared hardware, performance will often be worse then when
  using buffered IO.


## Deadlock and Starvation Dangers due to AIO

Using AIO in a naive way can easily lead to deadlocks in an
environment where the source/target of AIO are shared resources, like
pages in postgres' shared_buffers.

Consider one backend performing readahead on a table, initiating IO
for a number of buffers ahead of the current "scan position". If that
backend then performs some operation that blocks, or even just is
slow, the IO completion for the asynchronously initiated read may not
be processed.

This AIO implementation solves this problem by requiring that AIO
methods either allow AIO completions to be processed by any backend in
the system (e.g. io_uring, and indirectly posix, via signal handlers),
or to guarantee that AIO processing will happen even when the issuing
backend is blocked (e.g. worker mode, which offloads completion
processing to the AIO workers).


## API

- shared completion callbacks
- backend local completion callbacks
- streaming read / write helpers
- retries
-

## AIO Methods

To achieve portability and performance, several methods of performing
AIO are implemented.

### Worker

`io_method=worker` is available on every platform postgres runs on,
and implements asynchronous IO - from the view of the issuing
process - by dispatching the IO to one of several worker processes
performing the IO in a synchronous manner.


### io_uring

`io_method=io_uring` is available on Linux 5.1+. In contrast to worker
mode it dispatches all IO from within the process, lowering context
switch rate / latency.

### posix_aio

...


## Shared Buffer IO Details

- exclusive locks complete IO when blocked from lock acquisition
- unowned locks
- asynchronous writes can release locks on completion
- conditional locks for acquiring locks on more than one buffer
