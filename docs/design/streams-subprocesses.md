# Host streams and subprocesses

Approved contract for [#321](https://github.com/kalandra-lang/kalandra/issues/321).

## Byte streams

A ByteReader can fill a caller-owned ByteBuffer, either replacing its initialized
prefix with readInto or extending it with readAppend. A positive request that
returns zero indicates EOF. A short positive result is ordinary input. A
ByteWriter accepts the initialized prefix through write (one count) or writeAll
(the entire prefix on success). Partial writes can have externally visible
effects on error. Both traits use StreamError, preserving system error codes
and distinguishing a broken pipe. Existing std/fs signatures keep FsError.

Files and buffered file wrappers implement the applicable traits. A parent reads
child stdout and stderr through independent owned readers and writes stdin
through an owned writer. Native requests pin their byte storage through
completion. Generic dispatch must preserve the file implementation's buffer reuse.

## Commands and children

command(program, args) constructs an executable invocation with literal arguments.
Working directory and environment overrides apply only to the child. The three
standard endpoints independently support Inherit (default), Pipe and Null.
Starting returns an owned Child or a ProcessError. A partial launch failure
closes acquired endpoints and terminates and reaps a child if already launched.

Each take operation transfers one configured endpoint once. Closing stdin
delivers EOF. A borrowed wait suspends the calling task and caches ProcessStatus,
including the unsigned Windows exit code or POSIX termination signal. A nonzero
exit is an ordinary status. Cancelling a waiter leaves the child with its owner.
Owner cleanup closes untaken endpoints and arranges termination and reaping of
the direct child. Taken endpoints retain their own lifetimes. Descendants can
inherit endpoints and outlive the direct child.

capture(cmd, maxBytes) supplies EOF on stdin, concurrently drains stdout and
stderr, and returns status and two independent byte arrays. maxBytes bounds the
combined payload. Exceeding it terminates and reaps the child and reports
ProcessError.OutputLimit. Pipe backpressure must release the scheduler worker.
Bound handles, requests and retained bytes, including while cleanup is pending.

## Qualification

Exercise file-to-file, file-to-stdin and stdout-to-file byte copies, short reads,
EOF, binary data, moved handles, cancellation during backpressure, repeated waits,
cleanup on failed launch, and both outputs exceeding pipe capacity with one
scheduler worker. Compare existing and generic file-copy paths with identical
data and validate results. Exercise Windows and POSIX process behavior and host
coordination in mixed GPU applications. Publish runnable examples with the API.
